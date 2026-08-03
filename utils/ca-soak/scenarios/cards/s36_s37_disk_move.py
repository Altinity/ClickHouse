"""S36 MOVE PART/PARTITION between local and CA disks + S37 multi-disk storage policies (P1).

Both cards run on the `multidisk` compose variant (`docker-compose-multidisk.yml` +
`configs/storage_conf_multidisk_ch{1,2}.xml`), which adds TWO plain local disks (`local1`, `local2`)
alongside the same shared `ca` disk used everywhere else, wired into two storage policies:

- `ca_local`  (2 disks): volume `hot` = local1 (`max_data_part_size_bytes` routes big parts straight
  to the CA volume), volume `cas` = ca.
- `ca_local3` (3 disks): volume `hot` = local1 + local2 (round-robin JBOD), volume `cas` = ca.

S36 ("MOVE PART/PARTITION between local and CA disks, both directions") proves the explicit
`ALTER TABLE ... MOVE PART|PARTITION TO DISK ...` lifecycle: TO-CA publishes through the normal
CAS build path (blobs/manifest/refs, dedup applies); OFF-CA drops the CAS refs so deferred GC
reclaims the vacated content; concurrent SELECTs never fail during either direction; `fsck` is clean
after each leg; and a chaos leg (hard-kill the server mid-`MOVE PART`) proves the move is atomic —
the part ends up EITHER fully on its original disk OR fully on the destination, never split/duplicated.

S37 ("multi-disk storage policies") proves policy-driven placement and lifecycle on top of the same
disks: `max_data_part_size_bytes` routes oversized parts straight to the CA volume; a TTL MOVE rule
relocates a part to the CA volume in the background, and an explicit MOVE brings it back (the same
both-direction lifecycle as S36, but the TO-CA leg is policy/TTL-triggered instead of explicit);
`system.parts.disk_name` / `system.disks` are cross-checked for truthfulness; a clean restart
re-attaches every part to its recorded disk; a merge whose SOURCE parts sit on two different local
disks (the 3-disk `ca_local3` policy) still produces one policy-selected output part with correct
CAS publish/skip; and a chaos leg restarts mid-policy-triggered move.

`ALTER TABLE ... MOVE PART|PARTITION TO DISK|VOLUME` is implemented in `MergeTreeData` (the base
class shared by `MergeTree` and `ReplicatedMergeTree`), NOT replicated via the `ReplicatedMergeTree`
log — it is a per-replica physical relocation. Both cards therefore drive every MOVE from `node1`
only and read `system.parts` back from `node1`; `node2`'s copy of the same logical data may sit on a
different disk and that is expected, not a bug.

Dev scale keeps chaos-leg payloads a few MiB so the CAS upload/relocate window is wide enough for a
best-effort race against a hard kill (same "best-effort, not a hang-fix" honesty as S13's kill
timing) while staying well under the local-vs-CA `max_data_part_size_bytes` threshold everywhere it
must (the ordinary MOVE legs) and comfortably over it where routing is deliberately tested (S37).
"""

import threading
import time

from soak.chaos import Fault, FaultTarget, FaultAction, apply_fault
from ..framework import cluster_boot, gc as gc_mod, lifecycle, observe, sql
from ..framework import assertions as assertions_mod
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024
# Must match the `max_data_part_size_bytes` on the `hot` volume in storage_conf_multidisk_ch{1,2}.xml.
ROUTE_THRESHOLD_BYTES = 4 * MIB


def _parts_by_disk(node, table, partition=None):
    """{part_name: disk_name} for active parts of `table` (optionally scoped to one partition)."""
    where = f"table='{table}' AND active"
    if partition is not None:
        where += f" AND partition='{partition}'"
    txt = node.query(f"SELECT name, disk_name FROM system.parts WHERE {where} FORMAT TabSeparated")
    out = {}
    for line in txt.splitlines():
        if not line:
            continue
        name, disk = line.split("\t")
        out[name] = disk
    return out


def _move_with_lock_retry(node, alter_sql, *, timeout=600, retries=10, sleep_s=2.0):
    """Run an `ALTER TABLE ... MOVE PART|PARTITION` statement, retrying a bounded number of times
    if it raises `PART_IS_TEMPORARILY_LOCKED` ("participating in background process"): a background
    merge/mutation grabbing the same part right after an insert or a replicated DDL (e.g. right
    after `REMOVE TTL`) is a benign, transient race with ClickHouse's own part-locking, not a real
    MOVE failure -- retry until the background process releases the lock. Any other error (or the
    retry budget running out) propagates immediately, unchanged."""
    for attempt in range(retries):
        try:
            node.command(alter_sql, timeout=timeout)
            return
        except Exception as e:
            if "PART_IS_TEMPORARILY_LOCKED" not in str(e) or attempt == retries - 1:
                raise
            time.sleep(sleep_s)


def _wait_all_on_disk(node, table, disk_name, *, partition=None, timeout_s=120, poll_s=2.0):
    """Poll system.parts until every active part of `table` (optionally scoped to `partition`) sits
    on `disk_name`, or the timeout elapses. Returns the final {part: disk} placement."""
    deadline = time.monotonic() + timeout_s
    placement = _parts_by_disk(node, table, partition=partition)
    while time.monotonic() < deadline:
        if placement and all(d == disk_name for d in placement.values()):
            return placement
        time.sleep(poll_s)
        placement = _parts_by_disk(node, table, partition=partition)
    return placement


def _spawn_reader(node, query, errors, stop_event):
    def _loop():
        while not stop_event.is_set():
            try:
                node.query(query, timeout=60)
            except Exception as e:  # noqa: BLE001 - collected, not raised, from a background thread
                errors.append(str(e)[:200])
            time.sleep(0.05)
    th = threading.Thread(target=_loop, daemon=True)
    th.start()
    return th


def _disks_summary(node):
    """{disk_name: {total_space, free_space, unreserved_space}} from system.disks (best-effort)."""
    try:
        txt = node.query(
            "SELECT name, total_space, free_space, unreserved_space FROM system.disks "
            "FORMAT TabSeparated")
    except Exception as e:
        return {"error": str(e)}
    out = {}
    for line in txt.splitlines():
        if not line:
            continue
        cols = line.split("\t")
        name = cols[0]
        out[name] = {
            "total_space": int(cols[1]), "free_space": int(cols[2]),
            "unreserved_space": int(cols[3])}
    return out


# ---------------------------------------------------------------------------
# S36: MOVE PART / MOVE PARTITION between local and CA disks, both directions
# ---------------------------------------------------------------------------

@register
class S36(Scenario):
    name = "S36"
    title = "MOVE PART/PARTITION between local and CA disks (both directions)"
    priority = "P1"
    compose_variant = "multidisk"
    param_table = {
        # dev: a few small partitions (well under the 4 MiB routing threshold, so everything lands
        # on `local1` first), plus one larger single-partition part for the chaos leg.
        "dev": {"partitions": 3, "parts_per_partition": 2, "rows_per_part": 300, "payload_bytes": 1024,
                "chaos_rows": 300, "chaos_payload_bytes": 65536, "kill_delay_s": 0.4, "down_s": 3,
                "heal_timeout_s": 240},
        # Prefill parts MUST stay under the hot volume's max_data_part_size_bytes = 4 MiB or routing
        # (correctly) sends them straight to `ca` and the "lands on local1" check fails (2026-07-18
        # S36/S37 RCA: the old ci row's 3000x2048 = 5.86 MiB/part did exactly that). Scale part
        # COUNT (partitions/parts_per_partition), never per-part bytes, past that cap.
        "ci": {"partitions": 6, "parts_per_partition": 3, "rows_per_part": 3000, "payload_bytes": 1024,
               "chaos_rows": 600, "chaos_payload_bytes": 131072, "kill_delay_s": 0.6, "down_s": 4,
               "heal_timeout_s": 300},
        "full": {"partitions": 10, "parts_per_partition": 4, "rows_per_part": 2500, "payload_bytes": 1024,
                 "chaos_rows": 1200, "chaos_payload_bytes": 524288, "kill_delay_s": 1.0, "down_s": 6,
                 "heal_timeout_s": 360},
    }

    CHAOS_PK = 99  # a partition id distinct from the prefilled 0..partitions-1 range

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        table = "s36_move"
        nparts = int(p["partitions"])
        ppp = int(p["parts_per_partition"])
        rows = int(p["rows_per_part"])
        payload = int(p["payload_bytes"])
        chaos_rows = int(p["chaos_rows"])
        chaos_payload = int(p["chaos_payload_bytes"])
        kill_delay_s = float(p["kill_delay_s"])
        down_s = int(p["down_s"])
        heal_timeout_s = int(p["heal_timeout_s"])
        result.observations["scale"] = {
            "partitions": nparts, "parts_per_partition": ppp, "rows_per_part": rows,
            "payload_bytes": payload, "chaos_part_bytes_approx": chaos_rows * chaos_payload,
        }
        result.add(Verdict("scale used", "spec target = MOVE PART/PARTITION both directions + chaos",
                           f"{nparts} partitions x {ppp} parts, chaos part ~"
                           f"{chaos_rows*chaos_payload/MIB:.1f} MiB (scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        for n in cl.nodes():
            sql.create_ca_table(n, table, columns="id UInt64, pk UInt8, payload String",
                                order_by="id", partition_by="pk",
                                extra_settings={"storage_policy": "'ca_local'"})

        # Prefill: every (rows*payload) part stays well under the 4 MiB routing threshold, so
        # everything lands on `local1` by the policy's own placement rule (not a test artifact).
        op = 0
        for pk in range(nparts):
            for _ in range(ppp):
                gen = (f"SELECT {op} + number AS id, toUInt8({pk}) AS pk, "
                       f"randomString({payload}) AS payload FROM numbers({rows})")
                sql.insert_values(cl.node1, table, gen, timeout=600)
                op += rows
        cl.node1.command(f"SYSTEM SYNC REPLICA {table}", timeout=300)

        oracle_before = cl.node1.query(sql.table_checksum_query(table)).strip()
        placement0 = _parts_by_disk(cl.node1, table)
        all_local = bool(placement0) and all(d == "local1" for d in placement0.values())
        result.observations["initial_placement"] = placement0
        result.add(Verdict.check(
            "initial parts land on the local disk (below the routing threshold)",
            "disk_name=local1 for every part", placement0, all_local,
            "" if all_local else "a prefill part did not land on local1 — either the routing "
                                 "threshold or the default-volume placement is not what's assumed"))

        part_names = sorted(placement0.keys())
        move_part_name = part_names[0]
        move_pk = nparts - 1  # move this whole partition as the PARTITION-variant target

        # --- TO-CA: MOVE PART + MOVE PARTITION, with concurrent SELECTs running throughout --------
        errors_to_ca = []
        stop_to_ca = threading.Event()
        reader = _spawn_reader(cl.node1, f"SELECT count(), sum(sipHash64(*)) FROM {table} FORMAT Null",
                               errors_to_ca, stop_to_ca)
        counters_to_ca = _common.counters_window(ctx)
        try:
            ctx.log(f"S36: MOVE PART '{move_part_name}' TO DISK 'ca'")
            cl.node1.command(f"ALTER TABLE {table} MOVE PART '{move_part_name}' TO DISK 'ca'", timeout=600)
            ctx.log(f"S36: MOVE PARTITION {move_pk} TO DISK 'ca'")
            cl.node1.command(f"ALTER TABLE {table} MOVE PARTITION {move_pk} TO DISK 'ca'", timeout=600)
        finally:
            stop_to_ca.set()
            reader.join(timeout=10)
        to_ca_delta = counters_to_ca().get("_total", {})
        result.observations["concurrent_select_errors_to_ca"] = errors_to_ca[:10]
        result.add(Verdict.check(
            "concurrent SELECTs succeed during the TO-CA move", "0 errors",
            f"{len(errors_to_ca)} errors", not errors_to_ca,
            "" if not errors_to_ca else f"{errors_to_ca[:3]}"))

        placement_to_ca = _parts_by_disk(cl.node1, table)
        result.observations["placement_after_to_ca"] = placement_to_ca
        moved_part_ok = placement_to_ca.get(move_part_name) == "ca"
        moved_partition_ok = all(
            d == "ca" for n, d in _parts_by_disk(cl.node1, table, partition=str(move_pk)).items())
        result.add(Verdict.check("MOVE PART TO DISK 'ca' relocates the part", "disk_name=ca",
                                 placement_to_ca.get(move_part_name), moved_part_ok))
        result.add(Verdict.check("MOVE PARTITION TO DISK 'ca' relocates every part in it",
                                 "disk_name=ca for the whole partition",
                                 _parts_by_disk(cl.node1, table, partition=str(move_pk)),
                                 moved_partition_ok))

        blob_puts = int(to_ca_delta.get("CASBlobPut", 0)) + int(to_ca_delta.get("CASBlobPutDedup", 0))
        result.observations["to_ca_counters"] = {
            k: int(to_ca_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobPutDedup", "CASManifestPut", "CASRootCompareSwap")}
        result.add(Verdict.check(
            "TO-CA move publishes via the normal build path",
            "blobs/manifest/refs written (CASBlobPut/CASBlobPutDedup > 0)",
            f"CASBlobPut+Dedup={blob_puts}", blob_puts > 0,
            "" if blob_puts > 0 else "no blob/manifest writes observed during the TO-CA move"))

        fsck_to_ca = lifecycle.fsck_summary()
        result.observations["fsck_after_to_ca"] = fsck_to_ca
        result.add(Verdict.check("fsck clean after the TO-CA move", "dangling==0",
                                 fsck_to_ca.get("dangling"), fsck_to_ca.get("dangling") == 0))

        oracle_after_to_ca = cl.node1.query(sql.table_checksum_query(table)).strip()
        result.add(Verdict.check("data unchanged after the TO-CA move", oracle_before,
                                 oracle_after_to_ca, oracle_after_to_ca == oracle_before))

        # --- OFF-CA: MOVE PART + MOVE PARTITION back to local, with concurrent SELECTs -------------
        errors_off_ca = []
        stop_off_ca = threading.Event()
        reader2 = _spawn_reader(cl.node1, f"SELECT count(), sum(sipHash64(*)) FROM {table} FORMAT Null",
                                errors_off_ca, stop_off_ca)
        counters_off_ca = _common.counters_window(ctx)
        try:
            ctx.log(f"S36: MOVE PART '{move_part_name}' TO DISK 'local1'")
            cl.node1.command(f"ALTER TABLE {table} MOVE PART '{move_part_name}' TO DISK 'local1'", timeout=600)
            ctx.log(f"S36: MOVE PARTITION {move_pk} TO DISK 'local1'")
            cl.node1.command(f"ALTER TABLE {table} MOVE PARTITION {move_pk} TO DISK 'local1'", timeout=600)
        finally:
            stop_off_ca.set()
            reader2.join(timeout=10)
        result.observations["concurrent_select_errors_off_ca"] = errors_off_ca[:10]
        result.add(Verdict.check(
            "concurrent SELECTs succeed during the OFF-CA move", "0 errors",
            f"{len(errors_off_ca)} errors", not errors_off_ca,
            "" if not errors_off_ca else f"{errors_off_ca[:3]}"))

        placement_off_ca = _parts_by_disk(cl.node1, table)
        result.observations["placement_after_off_ca"] = placement_off_ca
        back_part_ok = placement_off_ca.get(move_part_name) == "local1"
        back_partition_ok = all(
            d == "local1" for n, d in _parts_by_disk(cl.node1, table, partition=str(move_pk)).items())
        result.add(Verdict.check("MOVE PART TO DISK 'local1' relocates the part back", "disk_name=local1",
                                 placement_off_ca.get(move_part_name), back_part_ok))
        result.add(Verdict.check("MOVE PARTITION TO DISK 'local1' relocates every part back",
                                 "disk_name=local1 for the whole partition",
                                 _parts_by_disk(cl.node1, table, partition=str(move_pk)),
                                 back_partition_ok))

        oracle_after_off_ca = cl.node1.query(sql.table_checksum_query(table)).strip()
        result.add(Verdict.check("data unchanged after the OFF-CA move", oracle_before,
                                 oracle_after_off_ca, oracle_after_off_ca == oracle_before))

        fsck_off_ca = lifecycle.fsck_summary()
        result.observations["fsck_after_off_ca"] = fsck_off_ca
        result.add(Verdict.check("fsck clean after the OFF-CA move", "dangling==0",
                                 fsck_off_ca.get("dangling"), fsck_off_ca.get("dangling") == 0))

        # OFF-CA drops the CAS refs the two moved parts held; deferred GC must reclaim that content
        # (no permanent orphans). Mirrors checkpoint.end_checkpoint's two-step drive: a bounded
        # residual after forced_gc_to_fixpoint is typically CONDEMNED content (fsck pending-gc) that
        # only graduates once the ack floor advances via each server's periodic retired-view sync
        # (~mount_renew_period) -- which forced_gc_to_fixpoint's faster poll can outrun, reporting a
        # "stable" but nonzero residual as if it were the true fixpoint. drain_condemned_pipeline
        # drives that graduation to completion; only a residual that survives BOTH steps is a real
        # leak.
        residual, history = gc_mod.forced_gc_to_fixpoint(
            cl, lifecycle.unreachable_probe(), log_fn=ctx.log)
        if residual and residual > 0:
            ctx.log(f"S36: draining condemned graduation pipeline after OFF-CA move (residual={residual})")
            residual, drain_hist = gc_mod.drain_condemned_pipeline(
                cl, lifecycle.unreachable_probe(), log_fn=ctx.log)
            history = history + drain_hist
        result.observations["gc_after_off_ca"] = {"residual": residual, "rounds": len(history),
                                                   "history": history}
        result.add(Verdict.check(
            "GC reclaims the vacated CA content within bounded rounds",
            "residual reaches 0", f"residual={residual} after {len(history)} round(s)",
            residual == 0,
            "" if residual == 0 else "content vacated by the OFF-CA move was not fully reclaimed"))

        # --- dedup-on-TO-CA: moving a part whose content already exists in the pool must dedup,
        # not re-upload -------------------------------------------------------------------------
        dedup_table_a = "s36_dedup_a"
        dedup_table_b = "s36_dedup_b"
        dedup_rows = 200
        for t in (dedup_table_a, dedup_table_b):
            for n in cl.nodes():
                sql.create_ca_table(n, t, columns="id UInt64, payload String", order_by="id",
                                    extra_settings={"storage_policy": "'ca_local'"})
        # A deterministic (non-random) payload so table B's part is BYTE-IDENTICAL to table A's
        # part: repeat() is the same on every call, unlike randomString() (which the rest of this
        # scenario relies on being unique per part, to keep unrelated dedup out of the other
        # assertions above).
        dedup_gen = (f"SELECT number AS id, repeat('cas-move-dedup-probe-', 100) AS payload "
                    f"FROM numbers({dedup_rows})")
        sql.insert_values(cl.node1, dedup_table_a, dedup_gen, timeout=300)
        sql.insert_values(cl.node1, dedup_table_b, dedup_gen, timeout=300)
        cl.node1.command(f"SYSTEM SYNC REPLICA {dedup_table_a}", timeout=120)
        cl.node1.command(f"SYSTEM SYNC REPLICA {dedup_table_b}", timeout=120)

        # A/B differential: table A's move pays the real upload cost (CASBlobPut > 0 in ITS OWN
        # window); table B's byte-identical move must then dedup-resolve those same blobs instead
        # of re-uploading them (CASBlobPut == 0 in ITS OWN window). CASBlobPutDedup is recorded as
        # an observation only, not a pass-condition: when the dedup resolves above the per-blob
        # path (identical whole-part manifest), that counter never increments even though B
        # correctly performed zero uploads -- the real requirement is "did not re-upload", which
        # CASBlobPut captures directly.
        counters_dedup_a = _common.counters_window(ctx)
        cl.node1.command(f"ALTER TABLE {dedup_table_a} MOVE PARTITION ID 'all' TO DISK 'ca'", timeout=300)
        delta_a = counters_dedup_a().get("_total", {})
        a_puts = int(delta_a.get("CASBlobPut", 0))
        a_dedup_puts = int(delta_a.get("CASBlobPutDedup", 0))

        counters_dedup_b = _common.counters_window(ctx)
        cl.node1.command(f"ALTER TABLE {dedup_table_b} MOVE PARTITION ID 'all' TO DISK 'ca'", timeout=300)
        delta_b = counters_dedup_b().get("_total", {})
        b_puts = int(delta_b.get("CASBlobPut", 0))
        b_dedup_puts = int(delta_b.get("CASBlobPutDedup", 0))

        result.observations["dedup_on_to_ca_counters"] = {
            "table_a_CasBlobPut": a_puts, "table_a_CasBlobPutDedup": a_dedup_puts,
            "table_b_CasBlobPut": b_puts, "table_b_CasBlobPutDedup": b_dedup_puts,
        }
        dedup_ok = a_puts > 0 and b_puts == 0
        result.add(Verdict.check(
            "MOVE TO-CA of byte-identical content dedups instead of re-uploading",
            "table A uploads (CASBlobPut>0) and table B's byte-identical move does not (CASBlobPut==0)",
            f"table_a CASBlobPut={a_puts} / table_b CASBlobPut={b_puts}", dedup_ok,
            "" if dedup_ok else
            "table A's real upload or table B's dedup-skip did not happen as expected"))

        oracle_dedup_a = cl.node1.query(sql.table_checksum_query(dedup_table_a)).strip()
        oracle_dedup_b = cl.node1.query(sql.table_checksum_query(dedup_table_b)).strip()
        result.add(Verdict.check(
            "dedup-probe tables read back identical data after the TO-CA moves",
            oracle_dedup_a, oracle_dedup_b, oracle_dedup_a == oracle_dedup_b))

        # --- chaos leg: hard-kill the server mid-MOVE PART -> atomic complete-or-rollback ----------
        gen_chaos = (f"SELECT number AS id, toUInt8({self.CHAOS_PK}) AS pk, "
                    f"randomString({chaos_payload}) AS payload FROM numbers({chaos_rows})")
        sql.insert_values(cl.node1, table, gen_chaos, timeout=600)
        chaos_placement_before = _parts_by_disk(cl.node1, table, partition=str(self.CHAOS_PK))
        result.observations["chaos_part_placement_before"] = chaos_placement_before
        if not chaos_placement_before:
            result.add(Verdict.inconclusive(
                "restart mid-MOVE PART is atomic (complete-or-rollback)",
                "exactly one consistent copy after a kill mid-move",
                "could not find the freshly-inserted chaos part in system.parts"))
        else:
            chaos_part_name = sorted(chaos_placement_before.keys())[0]
            oracle_before_chaos = cl.node1.query(sql.table_checksum_query(table)).strip()
            move_error = {}

            def _mover():
                try:
                    cl.node1.command(
                        f"ALTER TABLE {table} MOVE PART '{chaos_part_name}' TO DISK 'ca'", timeout=600)
                except Exception as e:  # noqa: BLE001 - the kill is expected to abort this
                    move_error["err"] = str(e)[:300]

            mover_thread = threading.Thread(target=_mover, daemon=True)
            mover_thread.start()
            time.sleep(kill_delay_s)
            ctx.log(f"S36 chaos: KILL ch1 mid-MOVE PART '{chaos_part_name}' (best-effort timing, "
                    f"kill_delay_s={kill_delay_s})")
            apply_fault(Fault(t_offset=0, target=FaultTarget.CH1, action=FaultAction.KILL,
                              duration_s=down_s))
            mover_thread.join(timeout=60)
            healthy = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
            result.observations["chaos_move_error"] = move_error.get("err")
            result.observations["chaos_healthy_after_restart"] = healthy
            if not healthy:
                result.add(Verdict.check(
                    "cluster recovers after the mid-move kill", "healthy within timeout",
                    f"not healthy within {heal_timeout_s}s", False))
            else:
                rows_after = int(cl.node1.scalar(
                    f"SELECT count() FROM {table} WHERE pk={self.CHAOS_PK}") or -1)
                chaos_placement_after = _parts_by_disk(cl.node1, table, partition=str(self.CHAOS_PK))
                oracle_after_chaos = cl.node1.query(sql.table_checksum_query(table)).strip()
                # Consistency = exactly one logical copy survives: row count unchanged, no active
                # part duplication for this partition (a real DOUBLE-MOVE bug would show 2x rows or
                # 2 disjoint active parts covering the same ids), and the full-table checksum is
                # stable (the kill must not have corrupted or duplicated data anywhere else either).
                consistent = (
                    rows_after == chaos_rows and
                    len(chaos_placement_after) >= 1 and
                    oracle_after_chaos == oracle_before_chaos)
                result.observations["chaos_part_placement_after"] = chaos_placement_after
                result.observations["chaos_rows_after"] = rows_after
                result.add(Verdict.check(
                    "restart mid-MOVE PART is atomic (complete-or-rollback)",
                    f"rows=={chaos_rows}, one consistent copy, checksum unchanged",
                    f"rows={rows_after} disks={set(chaos_placement_after.values())} "
                    f"checksum_stable={oracle_after_chaos == oracle_before_chaos} "
                    f"mover_error={move_error.get('err')}",
                    consistent,
                    "" if consistent else
                    "the killed MOVE left a half-moved or duplicated part — the move is not atomic"))

        # --- final quiesced checkpoint ---------------------------------------------------------
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S36 replica agreement")
        end = _common.standard_end(ctx, result, [table])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after the full MOVE lifecycle", "dangling==0",
                                 dangling, dangling == 0))
        assertions_mod.assert_reclaimable_drained(
            result, "content vacated by every MOVE reclaimed",
            end.get("residual_unreachable"), end.get("fsck_detail"))


# ---------------------------------------------------------------------------
# S37: multi-disk storage policies (local+CA, local+local+CA)
# ---------------------------------------------------------------------------

@register
class S37(Scenario):
    name = "S37"
    title = "multi-disk storage policies (local+CA, local+local+CA)"
    priority = "P1"
    compose_variant = "multidisk"
    param_table = {
        "dev": {
            "small_rows": 100, "small_payload_bytes": 1024,
            "big_rows": 40, "big_payload_bytes": 131072,       # ~5.1 MiB > 4 MiB threshold -> direct to ca
            "ttl_rows": 100, "ttl_payload_bytes": 1024,
            "mixed_parts": 4, "mixed_rows": 60, "mixed_payload_bytes": 65536,  # ~3.75 MiB/part
            "restart_timeout_s": 240, "kill_delay_s": 0.4, "down_s": 3,
        },
        "ci": {
            "small_rows": 1000, "small_payload_bytes": 2048,
            "big_rows": 80, "big_payload_bytes": 262144,       # ~20 MiB
            "ttl_rows": 1000, "ttl_payload_bytes": 2048,
            # mixed (leg-4) parts must stay UNDER the 4 MiB hot-volume cap to land on local1/local2
            # (2026-07-18 RCA: 120x131072 = 15 MiB/part routed all of them to `ca`). Scale count.
            "mixed_parts": 6, "mixed_rows": 48, "mixed_payload_bytes": 65536,
            "restart_timeout_s": 300, "kill_delay_s": 0.6, "down_s": 4,
        },
        "full": {
            "small_rows": 10000, "small_payload_bytes": 4096,
            "big_rows": 200, "big_payload_bytes": 524288,      # ~100 MiB
            "ttl_rows": 10000, "ttl_payload_bytes": 4096,
            "mixed_parts": 10, "mixed_rows": 56, "mixed_payload_bytes": 65536,
            "restart_timeout_s": 420, "kill_delay_s": 1.0, "down_s": 6,
        },
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        small_rows, small_payload = int(p["small_rows"]), int(p["small_payload_bytes"])
        big_rows, big_payload = int(p["big_rows"]), int(p["big_payload_bytes"])
        ttl_rows, ttl_payload = int(p["ttl_rows"]), int(p["ttl_payload_bytes"])
        mixed_parts = int(p["mixed_parts"])
        mixed_rows, mixed_payload = int(p["mixed_rows"]), int(p["mixed_payload_bytes"])
        restart_timeout_s = int(p["restart_timeout_s"])
        kill_delay_s = float(p["kill_delay_s"])
        down_s = int(p["down_s"])

        result.observations["scale"] = {
            "small_bytes_per_part": small_rows * small_payload,
            "big_bytes_per_part": big_rows * big_payload,
            "route_threshold_bytes": ROUTE_THRESHOLD_BYTES,
            "mixed_parts": mixed_parts, "mixed_bytes_per_part": mixed_rows * mixed_payload,
        }
        result.add(Verdict("scale used", "spec target = policy routing + TTL + mixed-disk merge + restart",
                           f"small~{small_rows*small_payload/1024:.0f}KiB big~"
                           f"{big_rows*big_payload/MIB:.1f}MiB mixed~{mixed_parts}x"
                           f"{mixed_rows*mixed_payload/MIB:.1f}MiB (scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; only --scale full approaches the spec target"))

        route_table = "s37_route"
        ttl_table = "s37_ttl"
        mixed_table = "s37_mixed"
        all_tables = [route_table, ttl_table, mixed_table]

        # --- leg 1: max_data_part_size_bytes routes big parts straight to the ca volume -----------
        for n in cl.nodes():
            sql.create_ca_table(n, route_table, columns="id UInt64, payload String", order_by="id",
                                extra_settings={"storage_policy": "'ca_local'"})
        sql.insert_random(cl.node1, route_table, rows=small_rows, payload_bytes=small_payload, op_id=0)
        sql.insert_random(cl.node1, route_table, rows=big_rows, payload_bytes=big_payload,
                          op_id=10_000_000)
        cl.node1.command(f"SYSTEM SYNC REPLICA {route_table}", timeout=300)
        placement_route = _parts_by_disk(cl.node1, route_table)
        result.observations["route_placement"] = placement_route
        disks_seen = set(placement_route.values())
        routed_ok = "local1" in disks_seen and "ca" in disks_seen
        result.add(Verdict.check(
            "max_data_part_size_bytes routes the oversized part straight to the ca volume",
            "one part on local1 (small) and one part on ca (big, over threshold)",
            placement_route, routed_ok,
            "" if routed_ok else "expected both a local1 part (small insert) and a ca part (big "
                                 "insert, over the 4 MiB routing threshold) — the volume-level "
                                 "max_data_part_size_bytes routing did not take effect"))

        # --- leg 2: TTL MOVE to the CA volume (background/policy-triggered), then MOVE back -------
        for n in cl.nodes():
            sql.create_ca_table(n, ttl_table, columns="id UInt64, ts DateTime, payload String",
                                order_by="id", ttl="ts + INTERVAL 1 SECOND TO VOLUME 'cas'",
                                extra_settings={"storage_policy": "'ca_local'"})
        gen_ttl = (f"SELECT number AS id, now() - 10 AS ts, randomString({ttl_payload}) AS payload "
                  f"FROM numbers({ttl_rows})")
        sql.insert_values(cl.node1, ttl_table, gen_ttl, timeout=600)
        placement_ttl_pre = _parts_by_disk(cl.node1, ttl_table)
        result.observations["ttl_placement_before_ttl"] = placement_ttl_pre

        counters_ttl = _common.counters_window(ctx)
        # MATERIALIZE TTL recomputes the TTL info and schedules the background move; the actual
        # relocation runs on the background move pool, so poll for it rather than assuming it is
        # synchronous within the ALTER.
        cl.node1.command(f"ALTER TABLE {ttl_table} MATERIALIZE TTL", timeout=300)
        placement_ttl_post = _wait_all_on_disk(cl.node1, ttl_table, "ca", timeout_s=120)
        ttl_delta = counters_ttl().get("_total", {})
        result.observations["ttl_placement_after_ttl"] = placement_ttl_post
        ttl_moved_ok = bool(placement_ttl_post) and all(d == "ca" for d in placement_ttl_post.values())
        result.add(Verdict.check(
            "TTL MOVE (background, policy-triggered) relocates the part to the ca volume",
            "disk_name=ca for every part after MATERIALIZE TTL", placement_ttl_post, ttl_moved_ok,
            "" if ttl_moved_ok else
            "TTL MOVE TO VOLUME 'cas' did not relocate the part within the poll budget"))
        result.observations["ttl_move_counters"] = {
            k: int(ttl_delta.get(k, 0)) for k in ("CASBlobPut", "CASBlobPutDedup", "CASManifestPut")}

        # Neutralize the (permanently-expired) TTL rule now that the TTL-driven TO-CA move is
        # verified, BEFORE the explicit move-back and the downstream legs. The rule
        # `ts + INTERVAL 1 SECOND TO VOLUME 'cas'` on rows with ts=now()-10 stays expired forever, so
        # the background TTL mover keeps re-pulling the part to 'cas' on every evaluation -- which
        # otherwise races the move-back verdict, leg-5's clean-restart placement-stability check, and
        # the chaos leg's explicit-MOVE atomicity check (background TTL mover competing with the
        # explicit move / restart window). This race was latent until MOVE-to-CA was fixed this round:
        # previously the TTL move failed (Code 236 promote collision), so the part stayed stuck on
        # local1 and those legs passed by accident on a broken feature. Issue it on node1 only -- it
        # is a replicated ALTER, so a second REMOVE TTL on node2 would fail (BAD_ARGUMENTS: nothing to
        # remove) once node1's removal replicates.
        cl.node1.command(f"ALTER TABLE {ttl_table} REMOVE TTL", timeout=120)

        # "back": explicit MOVE off the CA volume (same both-direction lifecycle as S36, but this
        # time the TO-CA leg was policy/TTL-triggered instead of an explicit ALTER MOVE).
        errors_ttl_back = []
        stop_ttl_back = threading.Event()
        reader_ttl = _spawn_reader(
            cl.node1, f"SELECT count() FROM {ttl_table} FORMAT Null", errors_ttl_back, stop_ttl_back)
        try:
            # `REMOVE TTL` just above is a replicated ALTER; a background merge/mutation racing its
            # replication can still hold the part under `PART_IS_TEMPORARILY_LOCKED` for a moment --
            # retry rather than let a benign race fail the whole scenario (observed 2026-07-17).
            _move_with_lock_retry(
                cl.node1, f"ALTER TABLE {ttl_table} MOVE PARTITION ID 'all' TO VOLUME 'hot'")
        finally:
            stop_ttl_back.set()
            reader_ttl.join(timeout=10)
        result.add(Verdict.check(
            "concurrent SELECTs succeed during the TTL-volume MOVE back", "0 errors",
            f"{len(errors_ttl_back)} errors", not errors_ttl_back))
        placement_ttl_back = _parts_by_disk(cl.node1, ttl_table)
        result.observations["ttl_placement_after_move_back"] = placement_ttl_back
        back_ok = bool(placement_ttl_back) and all(d == "local1" for d in placement_ttl_back.values())
        result.add(Verdict.check(
            "explicit MOVE TO VOLUME 'hot' brings the TTL-moved part back to local",
            "disk_name=local1", placement_ttl_back, back_ok))

        # --- leg 3: system.parts.disk_name / system.disks are truthful ----------------------------
        disks_summary = _disks_summary(cl.node1)
        result.observations["disks_summary"] = disks_summary
        sane_disks = isinstance(disks_summary, dict) and "error" not in disks_summary and all(
            d.get("total_space", 0) >= 0 and d.get("free_space", 0) >= 0 and
            d.get("free_space", 0) <= d.get("total_space", 1 << 62)
            for d in disks_summary.values())
        expected_names = {"local1", "local2", "ca"}
        names_present = expected_names.issubset(set(disks_summary.keys())) if sane_disks else False
        result.add(Verdict.check(
            "system.disks reports sane, truthful per-disk space accounting",
            "local1/local2/ca present, 0 <= free_space <= total_space",
            disks_summary, sane_disks and names_present,
            "" if (sane_disks and names_present) else
            "system.disks missing an expected disk or reported an insane space figure"))

        # --- leg 4: mixed-disk merge (sources on local1 AND local2) via the 3-disk policy ---------
        for n in cl.nodes():
            sql.create_ca_table(n, mixed_table, columns="id UInt64, payload String", order_by="id",
                                extra_settings={"storage_policy": "'ca_local3'"})
        for i in range(mixed_parts):
            sql.insert_random(cl.node1, mixed_table, rows=mixed_rows, payload_bytes=mixed_payload,
                              op_id=i * mixed_rows)
        placement_mixed_pre = _parts_by_disk(cl.node1, mixed_table)
        result.observations["mixed_placement_before_merge"] = placement_mixed_pre
        sources_on_both = {"local1", "local2"}.issubset(set(placement_mixed_pre.values()))
        result.add(Verdict.check(
            "round-robin JBOD placement spreads source parts across local1 AND local2",
            "both disks used by the hot volume", placement_mixed_pre, sources_on_both,
            "" if sources_on_both else
            "all source parts landed on one disk — the mixed-disk-merge leg is not exercised as "
            "intended (JBOD round_robin default may differ, or too few parts were inserted)"))

        counters_merge = _common.counters_window(ctx)
        cl.node1.command(f"OPTIMIZE TABLE {mixed_table} FINAL", timeout=600)
        merge_delta = counters_merge().get("_total", {})
        placement_mixed_post = _parts_by_disk(cl.node1, mixed_table)
        result.observations["mixed_placement_after_merge"] = placement_mixed_post
        result.observations["mixed_merge_counters"] = {
            k: int(merge_delta.get(k, 0)) for k in ("CASBlobPut", "CASBlobPutDedup", "CASManifestPut")}
        merged_ok = len(placement_mixed_post) == 1 and next(iter(placement_mixed_post.values())) in (
            "local1", "local2", "ca")
        result.add(Verdict.check(
            "a merge over mixed-disk sources produces ONE output part on a policy-selected disk",
            "exactly 1 active part after OPTIMIZE FINAL, on a disk that belongs to the policy",
            placement_mixed_post, merged_ok,
            "" if merged_ok else "expected exactly one merged part on a valid policy disk"))
        oracle_mixed = cl.node1.query(sql.table_checksum_query(mixed_table)).strip()

        # --- leg 5: clean restart re-attaches every part to its recorded disk ---------------------
        placement_before_restart = {
            route_table: _parts_by_disk(cl.node1, route_table),
            ttl_table: _parts_by_disk(cl.node1, ttl_table),
            mixed_table: _parts_by_disk(cl.node1, mixed_table),
        }
        ctx.log("S37: clean restart of both ClickHouse servers")
        apply_fault(Fault(t_offset=0, target=FaultTarget.BOTH, action=FaultAction.RESTART, duration_s=0))
        healthy = cluster_boot.wait_healthy(cl, timeout_s=restart_timeout_s, log_fn=ctx.log)
        result.observations["restart_healthy"] = healthy
        if not healthy:
            result.add(Verdict.check("cluster recovers after clean restart", "healthy within timeout",
                                     f"not healthy within {restart_timeout_s}s", False))
        else:
            placement_after_restart = {
                route_table: _parts_by_disk(cl.node1, route_table),
                ttl_table: _parts_by_disk(cl.node1, ttl_table),
                mixed_table: _parts_by_disk(cl.node1, mixed_table),
            }
            result.observations["placement_after_restart"] = placement_after_restart
            reattached_ok = placement_before_restart == placement_after_restart
            result.add(Verdict.check(
                "clean restart re-attaches every part to its recorded disk",
                "disk_name unchanged for every part across the restart",
                {"before": placement_before_restart, "after": placement_after_restart},
                reattached_ok,
                "" if reattached_ok else
                "at least one part came back on a different disk (or vanished/duplicated) after a "
                "clean restart"))
            unknown_disk_warns = 0
            for node in cl.nodes():
                try:
                    since = ctx.extra.get("since_event_time")
                    where = "(message ILIKE '%unknown disk%' OR message ILIKE '%not found on disk%')"
                    if since:
                        where += f" AND event_time >= '{since}'"
                    v = node.scalar(
                        f"SELECT count() FROM system.text_log WHERE level <= 'Warning' AND {where}")
                    unknown_disk_warns += int(v or 0)
                except Exception as e:
                    ctx.log(f"S37 text_log probe on {node.container} failed: {str(e)[:120]}")
            result.observations["unknown_disk_warnings"] = unknown_disk_warns
            result.add(Verdict.check("no unknown-disk warnings after restart", "0",
                                     unknown_disk_warns, unknown_disk_warns == 0))
            oracle_mixed_after = cl.node1.query(sql.table_checksum_query(mixed_table)).strip()
            result.add(Verdict.check("mixed-merge data unchanged across restart", oracle_mixed,
                                     oracle_mixed_after, oracle_mixed_after == oracle_mixed))

        # --- chaos leg: restart mid-policy-triggered MOVE (TTL volume move) -----------------------
        # `ttl_table` already holds the rows from the TTL leg above (never truncated here — the
        # pre-existing rows make the checksum-stability check stronger, since a half-moved part
        # would corrupt more than just the newly-inserted partition). The oracle must therefore be
        # self-grounding: read the row count right before this insert and add `ttl_rows`, rather
        # than comparing against the constant `ttl_rows` alone (that constant only ever matched an
        # empty table and made this verdict unsatisfiable on every run).
        rows_before_chaos_insert = int(cl.node1.scalar(f"SELECT count() FROM {ttl_table}") or -1)
        gen_chaos = (f"SELECT number AS id, now() - 10 AS ts, randomString({ttl_payload}) AS payload "
                    f"FROM numbers({ttl_rows})")
        sql.insert_values(cl.node1, ttl_table, gen_chaos, timeout=600)
        # This part is already on 'local1' (the MOVE-back leg above left ttl_table there); force TTL
        # recompute is skipped — instead drive an explicit MOVE TO VOLUME 'cas' (same code path as the
        # background TTL mover; explicit so we control exactly when it runs) and race a kill against it.
        chaos_placement_before = _parts_by_disk(cl.node1, ttl_table)
        result.observations["chaos_placement_before"] = chaos_placement_before
        oracle_before_chaos = cl.node1.query(sql.table_checksum_query(ttl_table)).strip()
        move_error = {}

        def _mover():
            try:
                # Same benign lock race as the move-back leg above can in principle fire here too
                # (a background merge on the part just inserted); retry it so the kill below races
                # the actual MOVE instead of an unrelated transient lock -- any other error (in
                # particular the kill itself, mid-flight) still propagates to the `except` below.
                _move_with_lock_retry(
                    cl.node1, f"ALTER TABLE {ttl_table} MOVE PARTITION ID 'all' TO VOLUME 'cas'")
            except Exception as e:  # noqa: BLE001 - the kill is expected to abort this
                move_error["err"] = str(e)[:300]

        mover_thread = threading.Thread(target=_mover, daemon=True)
        mover_thread.start()
        time.sleep(kill_delay_s)
        ctx.log(f"S37 chaos: KILL ch1 mid-policy-MOVE (best-effort timing, kill_delay_s={kill_delay_s})")
        apply_fault(Fault(t_offset=0, target=FaultTarget.CH1, action=FaultAction.KILL, duration_s=down_s))
        mover_thread.join(timeout=60)
        healthy_chaos = cluster_boot.wait_healthy(cl, timeout_s=restart_timeout_s, log_fn=ctx.log)
        result.observations["chaos_move_error"] = move_error.get("err")
        result.observations["chaos_healthy_after_restart"] = healthy_chaos
        if not healthy_chaos:
            result.add(Verdict.check("cluster recovers after the mid-policy-move kill",
                                     "healthy within timeout",
                                     f"not healthy within {restart_timeout_s}s", False))
        else:
            rows_after = int(cl.node1.scalar(f"SELECT count() FROM {ttl_table}") or -1)
            chaos_placement_after = _parts_by_disk(cl.node1, ttl_table)
            oracle_after_chaos = cl.node1.query(sql.table_checksum_query(ttl_table)).strip()
            expected_rows = rows_before_chaos_insert + ttl_rows
            consistent = (
                rows_after == expected_rows and
                len(chaos_placement_after) >= 1 and
                oracle_after_chaos == oracle_before_chaos)
            result.observations["chaos_placement_after"] = chaos_placement_after
            result.observations["chaos_rows_after"] = rows_after
            result.add(Verdict.check(
                "restart mid-policy-MOVE is atomic (complete-or-rollback)",
                f"rows=={expected_rows} (pre-existing {rows_before_chaos_insert} + inserted {ttl_rows}), "
                f"one consistent copy, checksum unchanged",
                f"rows={rows_after} disks={set(chaos_placement_after.values())} "
                f"checksum_stable={oracle_after_chaos == oracle_before_chaos} "
                f"mover_error={move_error.get('err')}",
                consistent,
                "" if consistent else
                "the killed policy-driven MOVE left a half-moved or duplicated part"))

        # --- final quiesced checkpoint -----------------------------------------------------------
        for t in all_tables:
            _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t),
                                          name=f"S37 replica agreement [{t}]")
        end = _common.standard_end(ctx, result, all_tables, table_filter="table LIKE 's37_%'")
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after the multi-disk policy lifecycle", "dangling==0",
                                 dangling, dangling == 0))
        assertions_mod.assert_reclaimable_drained(
            result, "content vacated by every policy move reclaimed",
            end.get("residual_unreachable"), end.get("fsck_detail"))
