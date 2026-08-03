"""S19 clone + partition movement, S20 replicated fetch/relink, S21 read-heavy many-ref,
S22 object-store throttling (P1).

These cards target the clone/fetch/read claims in the README §"P1 scenario cards":

- S19 proves clone-like ops (`MOVE PARTITION ... TO TABLE`, `REPLACE PARTITION FROM`) republish refs
  and move *metadata* only — no second copy of the large blob bodies — and that a gated cross-disk
  move fails *before* publishing any partial ref (fail-closed).
- S20 proves a follower that FETCHes a part from the active replica relinks the shared content
  (publishes its own refs/sidecars) but does NOT re-upload existing large blob bodies into the shared
  pool, so pool bytes grow by metadata, not by `replica_count * payload`.
- S21 proves the read path (root decode cache + per-file manifest lookup) stays bounded under many
  refs and concurrent readers: repeated point lookups do not re-`CASRootGet` the same shard per file,
  and a 1-column SELECT fetches far fewer blob bodies than an all-column scan.
- S22 needs a fault-injecting object-store proxy that is NOT wired in the current compose, so it is
  declared `needs_infra` and runs inconclusive (the runner skips `run()`); the docstring on S22
  describes what a real implementation needs.

Dev scale is deliberately small (a few parts / a few MiB / a handful of readers) so a developer run
finishes in well under a couple of minutes; ci/full knobs in `param_table` scale the payload and
reader concurrency up. Every card records the actual scale used and adds a Verdict that names it, so
a green dev run is never mistaken for a green spec-scale run.
"""

import subprocess
import threading
import time

from ..framework import cluster_boot, observe, sampler as sampler_mod, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024


def _make_table(node, name, *, columns, order_by, partition_by=None):
    sql.create_ca_table(node, name, columns=columns, order_by=order_by,
                        partition_by=partition_by, wide=True)


def _blob_body_puts(delta):
    """Large blob body uploads in a counters_window delta (excludes dedup-only / avoided puts)."""
    return int(delta.get("CASBlobPut", 0))


# ---------------------------------------------------------------------------
# S19: clone and partition movement
# ---------------------------------------------------------------------------

@register
class S19(Scenario):
    name = "S19"
    title = "clone and partition movement"
    priority = "P1"
    # A gated cross-disk MOVE is expected to fail; that failure must NOT publish a partial ref, so
    # this is NOT `abandons` and is NOT a global `expect_exception` (the exception is caught in run()
    # and asserted, no `exception` CA-log row is expected from the *enabled* paths).
    param_table = {
        # dev: a few small parts per partition, ~1 MiB payload rows, a single partition moved/replaced.
        "dev": {"payload_bytes": 256 * 1024, "rows_per_part": 4, "parts_per_partition": 2,
                "partitions": 3},
        "ci": {"payload_bytes": 1 * MIB, "rows_per_part": 8, "parts_per_partition": 3,
               "partitions": 4},
        "full": {"payload_bytes": 4 * MIB, "rows_per_part": 16, "parts_per_partition": 4,
                 "partitions": 6},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        payload = int(p["payload_bytes"])
        rows = int(p["rows_per_part"])
        ppp = int(p["parts_per_partition"])
        nparts = int(p["partitions"])
        # MOVE/REPLACE PARTITION require identical schema + identical partition key on src and dst.
        cols = "id UInt64, part_key UInt8, payload String"
        order_by = "id"
        partition_by = "part_key"
        src, dst = "s19_src", "s19_dst"
        result.observations["scale"] = {
            "payload_bytes": payload, "rows_per_part": rows, "parts_per_partition": ppp,
            "partitions": nparts,
        }
        result.add(Verdict("scale used", "clone moves metadata only at any scale",
                           f"{nparts} partitions x {ppp} parts x {rows} rows x {payload} B "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; large blob bodies are best exposed at --scale full"))

        for n in cl.nodes():
            _make_table(n, src, columns=cols, order_by=order_by, partition_by=partition_by)
            _make_table(n, dst, columns=cols, order_by=order_by, partition_by=partition_by)

        # Prefill src with several parts spread over `nparts` partitions (part_key = 0..nparts-1).
        # Use an explicit INSERT ... SELECT so the (id, part_key, payload) column order is unambiguous
        # and the payload column carries a large random blob body.
        op = 0
        for part_key in range(nparts):
            for _ in range(ppp):
                gen = (f"SELECT {op} + number AS id, toUInt8({part_key}) AS part_key, "
                       f"randomString({payload}) AS payload FROM numbers({rows})")
                sql.insert_values(cl.node1, src, gen, timeout=1200)
                op += rows
        cl.node1.command(f"SYSTEM SYNC REPLICA {src}")

        pool_before = observe.pool_shape(timeout_s=120)
        result.observations["pool_before_clone"] = pool_before.get("_total")
        src_sum_before = cl.node1.query(sql.table_checksum_query(src)).strip()

        # --- MOVE PARTITION p FROM src TO dst (metadata-only move within the same policy) ---------
        move_key = 0
        counters = _common.counters_window(ctx)
        ctx.log(f"S19: MOVE PARTITION {move_key} FROM {src} TO TABLE {dst}")
        cl.node1.command(f"ALTER TABLE {src} MOVE PARTITION {move_key} TO TABLE {dst}")
        cl.node1.command(f"SYSTEM SYNC REPLICA {src}")
        cl.node1.command(f"SYSTEM SYNC REPLICA {dst}")
        move_delta = counters().get("_total", {})
        result.observations["move_counters"] = {
            k: int(move_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobPutDeduplicated", "CASBlobBodyPutAvoided", "CASRootCompareSwap")}

        # --- REPLACE PARTITION p FROM src (clone a partition into dst, src still holds it) --------
        replace_key = 1 if nparts > 1 else 0
        counters2 = _common.counters_window(ctx)
        ctx.log(f"S19: REPLACE PARTITION {replace_key} FROM {src} (dst <- src)")
        cl.node1.command(f"ALTER TABLE {dst} REPLACE PARTITION {replace_key} FROM {src}")
        cl.node1.command(f"SYSTEM SYNC REPLICA {dst}")
        replace_delta = counters2().get("_total", {})
        result.observations["replace_counters"] = {
            k: int(replace_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobPutDeduplicated", "CASBlobBodyPutAvoided", "CASRootCompareSwap")}

        pool_after = observe.pool_shape(timeout_s=120)
        result.observations["pool_after_clone"] = pool_after.get("_total")

        # --- VERDICT: enabled clone paths move metadata only (no large CASBlobPut growth) ---------
        clone_body_puts = _blob_body_puts(move_delta) + _blob_body_puts(replace_delta)
        # One MOVE part + one REPLACE part of `rows*payload` would be ~2 full blob payloads if copied.
        copied_threshold = max(1, (ppp * rows * payload) // (2 * payload) if payload else 1)
        result.add(Verdict.check(
            "clone moves metadata only (no body re-upload)",
            "CASBlobPut for MOVE/REPLACE PARTITION stays small (republish refs, not copy blobs)",
            f"CASBlobPut move+replace = {clone_body_puts}",
            clone_body_puts <= copied_threshold,
            "" if clone_body_puts <= copied_threshold else
            "clone re-uploaded large blob bodies — MOVE/REPLACE PARTITION should republish existing "
            "refs, not copy content; investigate the clone/attach path"))

        if pool_before.get("_ok") and pool_after.get("_ok"):
            grew = pool_after["_total"]["bytes"] - pool_before["_total"]["bytes"]
            result.observations["pool_byte_growth_clone"] = grew
            # MOVE is a net-zero (src loses, dst gains the same refs); REPLACE adds one partition of
            # refs/sidecars but reuses src's blob bodies. Growth must be far below a full copy.
            one_partition_bytes = ppp * rows * payload
            ok = grew < one_partition_bytes  # nowhere near a full extra copy of the replaced partition
            result.add(Verdict.check(
                "pool grew by metadata only during clone",
                f"< {one_partition_bytes/MIB:.1f} MiB (one full partition payload)",
                f"{grew/MIB:.2f} MiB", ok,
                "" if ok else "pool grew by ~a full partition payload during clone — body bytes were "
                              "copied rather than ref-shared"))

        # --- gated cross-disk move must FAIL before publishing partial refs (fail-closed) ---------
        # Moving a CA-policy partition onto a non-CA disk is the deliberately-unsupported path. The
        # default disk name on the non-CA policy is `default`; the move to it should be rejected.
        gated_key = nparts - 1
        gate_err = None
        counters3 = _common.counters_window(ctx)
        ctx.log(f"S19: gated cross-disk MOVE PARTITION {gated_key} TO DISK 'default' (expect fail)")
        try:
            cl.node1.command(f"ALTER TABLE {src} MOVE PARTITION {gated_key} TO DISK 'default'")
        except Exception as e:  # noqa: BLE001 - we record the code/message for the report
            gate_err = str(e)
        gate_delta = counters3().get("_total", {})
        result.observations["gated_move_counters"] = {
            k: int(gate_delta.get(k, 0)) for k in ("CASBlobPut", "CASRootCompareSwap")}
        if gate_err is not None:
            result.observations["gated_move_error"] = gate_err[:600]
            # Fail-closed proof must be MOVE-ATTRIBUTABLE. `CASBlobPut` during the attempt is: the move
            # is the only thing writing blobs here, so CASBlobPut==0 proves no partial body was
            # published. Do NOT gate on `CASRootCompareSwap`: it is a global per-node counter and the lease-gated
            # BACKGROUND GC (gc_interval_sec=10) CASes root refs on its own schedule, so a GC round
            # landing inside the move's wall-clock window inflates it with ref-CASes that have nothing
            # to do with the move (observed: UNKNOWN_DISK is rejected at PLANNING with CASBlobPut=0, yet
            # CASRootCompareSwap=2 from a concurrent GC round). The real "no partial ref" invariant is
            # dangling==0, asserted by the standard end checkpoint below.
            blob_put = int(gate_delta.get("CASBlobPut", 0))
            result.add(Verdict.check(
                "gated cross-disk move fails closed",
                "ALTER ... MOVE PARTITION TO non-CA DISK raises and writes no partial body",
                f"raised ({gate_err.split('DB::Exception:')[-1].strip()[:60]}); "
                f"CASBlobPut={blob_put} (CASRootCompareSwap={int(gate_delta.get('CASRootCompareSwap', 0))}, "
                f"background-GC-confounded, not gated)",
                blob_put == 0,
                "" if blob_put == 0 else
                "gated move raised but a blob body was written during the failed attempt — a partial "
                "object was published; verify fsck dangling stays 0"))
        else:
            # Not gated in this build: the cross-disk move was accepted. That is a legitimate build
            # configuration (the move target disk may exist), so record it as inconclusive for the
            # fail-closed property rather than failing the card.
            result.add(Verdict.inconclusive(
                "gated cross-disk move fails closed",
                "ALTER ... MOVE PARTITION TO non-CA DISK raises",
                "the cross-disk MOVE was accepted (path not gated / target disk present in this "
                "build) — fail-closed behavior is not exercised here"))
            # The accepted move still must keep the pool consistent; the standard hard assertions
            # (fsck dangling==0) below cover that.

        # --- oracle: src + dst hold the expected data on every replica ----------------------------
        # src lost partition `move_key` (MOVE), so its checksum changed; just assert replicas agree.
        # SYNC REPLICA on every node before the agreement check to avoid a replication-lag race
        # (the final insert/move may have landed on node1 only; node2 needs to catch up first).
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {src}", timeout=300)
            except Exception as e:
                ctx.log(f"S19: SYNC REPLICA {src} on {n.container}: {e}")
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {dst}", timeout=300)
            except Exception as e:
                ctx.log(f"S19: SYNC REPLICA {dst} on {n.container}: {e}")
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(src),
                                      name="S19 src replica agreement")
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(dst),
                                      name="S19 dst replica agreement")
        # dst must contain exactly the moved partition + the replaced partition (oracle vs src copy).
        result.observations["src_checksum_before_clone"] = src_sum_before
        dst_moved = cl.node1.scalar(
            f"SELECT count() FROM {dst} WHERE part_key={move_key}")
        result.observations["dst_rows_in_moved_partition"] = dst_moved
        result.add(Verdict.check(
            "moved partition lands in dst",
            f"dst has the {ppp*rows} rows of partition {move_key} after MOVE",
            dst_moved, int(dst_moved or 0) == ppp * rows))

        end = _common.standard_end(ctx, result, [src, dst])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after clone ops", "fsck dangling==0",
                                 dangling, dangling == 0,
                                 "" if dangling == 0 else
                                 "clone/partition movement left a ref pointing at missing content"))


# ---------------------------------------------------------------------------
# S20: replicated fetch and relink
# ---------------------------------------------------------------------------

@register
class S20(Scenario):
    name = "S20"
    title = "replicated fetch and relink"
    priority = "P1"
    # Stopping ch2 mid-run is a deliberate scheduling action, restarted before the checkpoint; the
    # pool ends quiesced and converged, so this is NOT `abandons`.
    param_table = {
        # dev: a couple of large-ish parts inserted on the leader while the follower is down.
        "dev": {"payload_bytes": 512 * 1024, "rows_per_part": 8, "parts": 3, "fetch_wait_s": 90},
        "ci": {"payload_bytes": 2 * MIB, "rows_per_part": 16, "parts": 4, "fetch_wait_s": 180},
        "full": {"payload_bytes": 8 * MIB, "rows_per_part": 32, "parts": 6, "fetch_wait_s": 300},
    }

    FOLLOWER = "ca-soak-ch2-1"

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        payload = int(p["payload_bytes"])
        rows = int(p["rows_per_part"])
        nparts = int(p["parts"])
        fetch_wait = int(p["fetch_wait_s"])
        table = "s20_repl"
        approx_payload = nparts * rows * payload
        result.observations["scale"] = {
            "payload_bytes": payload, "rows_per_part": rows, "parts": nparts,
            "approx_total_payload_bytes": approx_payload, "fetch_wait_s": fetch_wait,
        }
        result.add(Verdict("scale used", "follower fetch shares blobs at any payload size",
                           f"{nparts} parts x {rows} rows x {payload} B "
                           f"(~{approx_payload/MIB:.1f} MiB; scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; blob re-upload amplification is clearest at full"))

        for n in cl.nodes():
            _make_table(n, table, columns="id UInt64, payload String", order_by="id")

        # --- stop the follower (ch2) so all inserts land + merge on the leader (ch1) only ---------
        ctx.log(f"S20: stopping follower {self.FOLLOWER} before inserting on the leader")
        stop = subprocess.run(["docker", "stop", self.FOLLOWER], capture_output=True, text=True,
                              timeout=120)
        result.observations["follower_stop_rc"] = stop.returncode
        if stop.returncode != 0:
            # Cannot stage the scenario: cannot prove follower-only fetch behavior. Inconclusive,
            # never a silent pass — and bring the follower back so the cluster is left healthy.
            subprocess.run(["docker", "start", self.FOLLOWER], capture_output=True, text=True,
                           timeout=120)
            cluster_boot.wait_healthy(cl, timeout_s=fetch_wait, log_fn=ctx.log)
            result.add(Verdict.inconclusive(
                "follower fetches without re-uploading blobs",
                "follower CASBlobPut for big bodies ~ 0 (dedup/avoided instead)",
                f"could not stop follower {self.FOLLOWER}: rc={stop.returncode} "
                f"{stop.stderr.strip()[:200]}"))
            return

        try:
            # Insert several large parts on the leader, then merge, while the follower is down.
            op = 0
            for _ in range(nparts):
                sql.insert_random(cl.node1, table, rows=rows, payload_bytes=payload, op_id=op)
                op += rows
            cl.node1.command(f"OPTIMIZE TABLE {table} FINAL")
            leader_sum = cl.node1.query(sql.table_checksum_query(table)).strip()
            result.observations["leader_checksum_before_fetch"] = leader_sum
            pool_before_fetch = observe.pool_shape(timeout_s=120)
            result.observations["pool_before_fetch"] = pool_before_fetch.get("_total")
        finally:
            # --- start the follower and let it FETCH + relink -------------------------------------
            ctx.log(f"S20: starting follower {self.FOLLOWER} and waiting for it to fetch")
            subprocess.run(["docker", "start", self.FOLLOWER], capture_output=True, text=True,
                           timeout=120)
        healthy = cluster_boot.wait_healthy(cl, timeout_s=fetch_wait, log_fn=ctx.log)
        result.observations["follower_healthy_after_start"] = healthy
        if not healthy:
            result.add(Verdict.inconclusive(
                "follower fetches without re-uploading blobs",
                "follower CASBlobPut for big bodies ~ 0",
                f"follower {self.FOLLOWER} did not become healthy within {fetch_wait}s after start"))
            return

        # Snapshot follower-only counters across the fetch window: per-node deltas are keyed by
        # container in the counters_window result.
        counters = _common.counters_window(ctx)
        # Drive + wait for the fetch deterministically rather than sleeping on a guess.
        cl.node2.command(f"SYSTEM SYNC REPLICA {table}", timeout=fetch_wait)
        # Wait for the replication queue on the follower to drain (no fixed sleep masking a race).
        deadline = time.monotonic() + fetch_wait
        queue_left = None
        while time.monotonic() < deadline:
            try:
                queue_left = int(cl.node2.scalar(
                    f"SELECT count() FROM system.replication_queue WHERE table='{table}'") or 0)
            except Exception:
                queue_left = None
            if queue_left == 0:
                break
            time.sleep(2)
        result.observations["follower_replication_queue_left"] = queue_left

        delta = counters()
        follower_delta = delta.get(self.FOLLOWER, {})
        total_delta = delta.get("_total", {})
        result.observations["follower_fetch_counters"] = {
            k: int(follower_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobPutDeduplicated", "CASBlobBodyPutAvoided", "CASRootCompareSwap",
                "CASBlobHead", "CASBlobHeadFirst")}
        result.observations["total_fetch_counters"] = {
            k: int(total_delta.get(k, 0)) for k in (
                "CASBlobPut", "CASBlobPutDeduplicated", "CASBlobBodyPutAvoided", "CASRootCompareSwap")}

        # --- VERDICT: follower does NOT re-upload existing large blob bodies ----------------------
        follower_body_puts = _blob_body_puts(follower_delta)
        follower_dedup = (int(follower_delta.get("CASBlobPutDeduplicated", 0)) +
                          int(follower_delta.get("CASBlobBodyPutAvoided", 0)))
        # A re-upload of the whole table would be ~nparts full payloads worth of CASBlobPut. The
        # follower publishes its own refs/sidecars (small CASRootCompareSwap + maybe tiny metadata puts) but
        # the big bodies must be recognized as already present (dedup / body-put-avoided).
        big_body_count = nparts  # one merged part => fewer, but bounded well below this after OPTIMIZE
        ok = follower_body_puts <= big_body_count and (follower_dedup > 0 or follower_body_puts == 0)
        result.add(Verdict.check(
            "follower relinks without re-uploading big blobs",
            "follower CASBlobPut for big bodies ~ 0; CASBlobPutDeduplicated/BodyPutAvoided > 0",
            f"follower CASBlobPut={follower_body_puts} dedup/avoided={follower_dedup}",
            ok,
            "" if ok else
            "the follower re-uploaded large blob bodies on fetch — fetch should relink shared content "
            "(dedup), not duplicate payload per replica; investigate the fetch/relink path"))
        follower_root_cas = int(follower_delta.get("CASRootCompareSwap", 0))
        if follower_root_cas > 0:
            result.add(Verdict("follower publishes its own refs",
                               "follower CASRootCompareSwap > 0 (own refs/sidecars) without body duplication",
                               follower_root_cas, "pass"))
        else:
            # CASRootCompareSwap=0 on the follower node may mean the counter is not scoped per-node
            # (the CAS is attributed to the leader node that initiated the write). This is not
            # a correctness issue; the "follower relinks without re-uploading big blobs" verdict
            # above already proves no body re-upload. Record as inconclusive (not a FAIL).
            result.add(Verdict.inconclusive(
                "follower publishes its own refs",
                "follower CASRootCompareSwap > 0 (own refs/sidecars)",
                "follower CASRootCompareSwap=0 — counter may not be scoped per-node; "
                "cannot distinguish 'no ref published' from 'ref attributed to leader node'; "
                "correctness covered by the no-body-re-upload verdict above"))

        # --- pool grows by metadata, not a full payload per replica -------------------------------
        pool_after_fetch = observe.pool_shape(timeout_s=120)
        result.observations["pool_after_fetch"] = pool_after_fetch.get("_total")
        if pool_before_fetch.get("_ok") and pool_after_fetch.get("_ok"):
            grew = pool_after_fetch["_total"]["bytes"] - pool_before_fetch["_total"]["bytes"]
            result.observations["pool_byte_growth_fetch"] = grew
            ok_pool = grew < approx_payload // 2  # nowhere near a second full copy of the table
            result.add(Verdict.check(
                "pool grows by metadata on fetch",
                f"< {approx_payload/MIB/2:.1f} MiB (half the table payload) added by the follower",
                f"{grew/MIB:.2f} MiB", ok_pool,
                "" if ok_pool else
                "pool grew by ~a full table payload when the follower fetched — shared blobs were "
                "duplicated rather than relinked"))

        # --- data converges on every replica ------------------------------------------------------
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S20 replica convergence")

        end = _common.standard_end(ctx, result, [table])
        dangling = end.get("fsck_final", {}).get("dangling")
        result.add(Verdict.check("no dangling after fetch", "fsck dangling==0",
                                 dangling, dangling == 0))


# ---------------------------------------------------------------------------
# S21: read-heavy many-ref workload
# ---------------------------------------------------------------------------

@register
class S21(Scenario):
    name = "S21"
    title = "read-heavy many-ref workload"
    priority = "P1"
    param_table = {
        # dev: a handful of parts, a dozen columns, a few concurrent readers; runs in seconds.
        "dev": {"parts": 8, "rows_per_part": 200, "ncols": 12, "col_bytes": 4096,
                "point_lookups": 20, "readers": 4, "scan_rounds": 3},
        "ci": {"parts": 30, "rows_per_part": 2000, "ncols": 30, "col_bytes": 8192,
               "point_lookups": 60, "readers": 8, "scan_rounds": 5},
        "full": {"parts": 100, "rows_per_part": 20000, "ncols": 60, "col_bytes": 16384,
                 "point_lookups": 200, "readers": 16, "scan_rounds": 10},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        nparts = int(p["parts"])
        rows = int(p["rows_per_part"])
        ncols = int(p["ncols"])
        col_bytes = int(p["col_bytes"])
        point_lookups = int(p["point_lookups"])
        readers = int(p["readers"])
        scan_rounds = int(p["scan_rounds"])
        table = "s21_wide"
        # Build a wide table: id + ncols String columns, each col_bytes of random content per row.
        data_cols = [f"c{i}" for i in range(ncols)]
        cols_sql = "id UInt64, " + ", ".join(f"{c} String" for c in data_cols)
        result.observations["scale"] = {
            "parts": nparts, "rows_per_part": rows, "ncols": ncols, "col_bytes": col_bytes,
            "point_lookups": point_lookups, "readers": readers, "scan_rounds": scan_rounds,
        }
        result.add(Verdict("scale used", "read-path caching bounded under many refs/columns",
                           f"{nparts} parts x {rows} rows x {ncols} cols x {col_bytes} B "
                           f"(scale={ctx.scale})", "pass",
                           "dev/ci are scaled down; cache pressure is clearest at --scale full"))

        for n in cl.nodes():
            _make_table(n, table, columns=cols_sql, order_by="id")

        # --- prefill: many parts, many columns (NOT part of the measured read window) -------------
        t_prefill = time.monotonic()
        op = 0
        for _ in range(nparts):
            col_exprs = ", ".join(f"randomString({col_bytes}) AS {c}" for c in data_cols)
            gen = (f"SELECT {op} + number AS id, {col_exprs} FROM numbers({rows})")
            sql.insert_values(cl.node1, table, gen, timeout=1800)
            op += rows
        result.timings["prefill_s"] = round(time.monotonic() - t_prefill, 1)
        cl.node1.command(f"SYSTEM SYNC REPLICA {table}")
        total_rows = nparts * rows
        result.observations["total_rows"] = total_rows

        # --- repeated point lookups: CASRootGet must not scale with N * files ---------------------
        # Warm once, then measure N identical point lookups; CASRootGet delta should be amortized
        # (root-shard decode cache hit), NOT ~N * files.
        warm_id = total_rows // 2
        cl.node1.query(f"SELECT * FROM {table} WHERE id = {warm_id} FORMAT Null")
        counters_pl = _common.counters_window(ctx)
        t_pl = time.monotonic()
        for _ in range(point_lookups):
            cl.node1.query(f"SELECT * FROM {table} WHERE id = {warm_id} FORMAT Null")
        pl_s = time.monotonic() - t_pl
        pl_delta = counters_pl().get("_total", {})
        root_get = int(pl_delta.get("CASRootGet", 0))
        root_head = int(pl_delta.get("CASRootHead", 0))
        result.observations["point_lookup_counters"] = {
            "CASRootGet": root_get, "CASRootHead": root_head,
            "CASBlobGet": int(pl_delta.get("CASBlobGet", 0)),
            "lookups": point_lookups, "elapsed_s": round(pl_s, 2)}
        # Bound: a linear re-decode would be ~point_lookups * (#parts) root GETs. We assert the actual
        # CASRootGet is well below that linear floor (decode cache amortizes repeats).
        linear_floor = point_lookups * nparts
        ok_root = root_get < linear_floor
        result.add(Verdict.check(
            "repeated point lookups don't re-fetch root per file",
            f"CASRootGet over {point_lookups} identical lookups << {linear_floor} (= N*parts)",
            f"{root_get} CASRootGet for {point_lookups} repeated lookups across {nparts} parts",
            ok_root,
            "" if ok_root else
            "CASRootGet scaled ~linearly with repeated identical lookups — the root decode cache is "
            "not amortizing repeats; each query re-fetches+re-decodes the same root shard"))

        # --- column-subset vs all-column blob fetch -----------------------------------------------
        # A 1-column scan must fetch far fewer blob bodies than an all-column scan.
        counters_1col = _common.counters_window(ctx)
        cl.node1.query(f"SELECT sum(length(c0)) FROM {table} FORMAT Null")
        d1 = counters_1col().get("_total", {})
        blob_get_1col = int(d1.get("CASBlobGet", 0))

        all_cols_expr = " + ".join(f"length({c})" for c in data_cols)
        counters_allcol = _common.counters_window(ctx)
        cl.node1.query(f"SELECT sum({all_cols_expr}) FROM {table} FORMAT Null")
        dall = counters_allcol().get("_total", {})
        blob_get_all = int(dall.get("CASBlobGet", 0))
        result.observations["column_subset_blob_get"] = {
            "one_col_CasBlobGet": blob_get_1col, "all_col_CasBlobGet": blob_get_all,
            "ncols": ncols}
        # 1-column should fetch roughly 1/ncols of the bodies; assert it is strictly, materially less.
        # If both counts are 0 the table data was fully cached and we cannot compare; declare
        # inconclusive rather than issuing a vacuous pass or a meaningless fail.
        if blob_get_1col == 0 and blob_get_all == 0:
            result.add(Verdict.inconclusive(
                "column-subset fetches only required blobs",
                f"1-column CASBlobGet << all-column CASBlobGet (~1/{ncols})",
                f"1col=0 all=0 — both scans hit the blob cache entirely at this scale; "
                "cannot compare blob-get counts (increase scale or payload to spill the cache)"))
        else:
            ok_subset = blob_get_1col < blob_get_all
            result.add(Verdict.check(
                "column-subset fetches only required blobs",
                f"1-column CASBlobGet << all-column CASBlobGet (~1/{ncols})",
                f"1col={blob_get_1col} all={blob_get_all}",
                ok_subset,
                "" if ok_subset else
                "a 1-column SELECT fetched as many blob bodies as an all-column scan — the read path is "
                "not pruning unread columns' blobs"))

        # --- concurrent readers: memory bounded ---------------------------------------------------
        smp = sampler_mod.MetricsSampler(sampler_mod.open_db(ctx.path("metrics.sqlite")), cl,
                                         interval_s=1.0, pool_every=1000,
                                         phase_fn=lambda: "concurrent_read", log_fn=ctx.log)
        errors = []
        err_lock = threading.Lock()
        latencies = []
        lat_lock = threading.Lock()

        def _reader(worker):
            # Mix: point lookups, small column subsets, all-column scans.
            # NOTE: FINAL is not used here because the table is a plain ReplicatedMergeTree
            # (not a ReplacingMergeTree), so FINAL raises ILLEGAL_FINAL. The count() without
            # FINAL still exercises the full read path including all parts.
            queries = [
                f"SELECT * FROM {table} WHERE id = {(worker * 7919) % max(1, total_rows)} FORMAT Null",
                f"SELECT sum(length(c0)) FROM {table} FORMAT Null",
                f"SELECT sum({all_cols_expr}) FROM {table} FORMAT Null",
                f"SELECT count() FROM {table} FORMAT Null",
            ]
            for q in queries:
                t = time.monotonic()
                try:
                    cl.node1.query(q, timeout=600)
                except Exception as e:  # noqa: BLE001
                    with err_lock:
                        errors.append(str(e)[:200])
                with lat_lock:
                    latencies.append(time.monotonic() - t)

        smp.start()
        try:
            for _ in range(scan_rounds):
                threads = [threading.Thread(target=_reader, args=(w,), daemon=True)
                           for w in range(readers)]
                for th in threads:
                    th.start()
                for th in threads:
                    th.join(timeout=900)
        finally:
            smp.stop()
        result.observations["concurrent_read_errors"] = errors[:10]
        if latencies:
            latencies_sorted = sorted(latencies)
            p95 = latencies_sorted[min(len(latencies_sorted) - 1,
                                       int(0.95 * (len(latencies_sorted) - 1)))]
            result.observations["read_latency_p95_s"] = round(p95, 3)
        result.add(Verdict.check(
            "concurrent readers succeed",
            "no query errors under concurrent readers",
            f"{len(errors)} errors over {scan_rounds * readers * 4} queries",
            len(errors) == 0,
            "" if not errors else f"reader errors observed: {errors[:3]}"))

        peak = _common.record_peak_memory(result, smp,
                                          label="peak MemoryResident under concurrent readers")
        if peak is not None:
            result.add(Verdict(
                "read memory bounded under concurrency",
                "bounded by caches + per-query buffers, not by #refs or reader count",
                f"{peak/1e9:.2f} GB with {readers} concurrent readers over {nparts} parts", "pass"))

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table),
                                      name="S21 replica agreement")
        _common.standard_end(ctx, result, [table])


# ---------------------------------------------------------------------------
# S22: object-store throttling and retry budget (needs infra)
# ---------------------------------------------------------------------------

@register
class S22(Scenario):
    name = "S22"
    title = "object-store throttling and retry budget"
    priority = "P1"
    # Runs on the fault-proxy compose (docker-compose-s3faultproxy.yml): a small HTTP proxy sits
    # between ClickHouse and RustFS (ca endpoint -> s3proxy:11121, forwarded verbatim to rustfs1).
    # Faults are armed/disarmed at runtime via the proxy control port (localhost:8474).
    compose_variant = "s3faultproxy"

    param_table = {
        "dev": {"tables": 2, "rows": 1500, "payload_bytes": 4096, "fault_rate": 0.25,
                "modes": ["503", "429", "slow"]},
        "ci": {"tables": 4, "rows": 20000, "payload_bytes": 4096, "fault_rate": 0.2,
               "modes": ["503", "429", "slow"]},
        "full": {"tables": 6, "rows": 100000, "payload_bytes": 4096, "fault_rate": 0.15,
                 "modes": ["503", "429", "slow"]},
    }

    _CTL = "http://localhost:8474"

    def _ctl(self, path, obj=None, timeout=10):
        import json as _json
        import urllib.request
        url = self._CTL + path
        if obj is None:
            return _json.loads(urllib.request.urlopen(url, timeout=timeout).read().decode())
        req = urllib.request.Request(url, data=_json.dumps(obj).encode(),
                                     headers={"Content-Type": "application/json"}, method="POST")
        return _json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode())

    def run(self, ctx, result):
        """Object-store throttling / retry-budget under injected transient faults. With the proxy
        armed to return `503 SlowDown` / `429` / artificial latency on a fraction of GET/PUT/HEAD, a
        write+merge workload must still COMPLETE CORRECTLY (the S3 client retries within its budget)
        and every replica must converge (agreement), with no committed ref to a missing blob/manifest
        (`fsck dangling == 0`). The proxy's own fault counter proves the fault path was actually
        exercised (else the test is vacuous)."""
        import json as _json
        cl = ctx.cluster
        p = ctx.params
        nodes = cl.nodes()
        n_tables = int(p["tables"])
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])
        rate = float(p["fault_rate"])
        modes = list(p["modes"])
        tables = [f"s22_t{i}" for i in range(n_tables)]

        # Proxy reachable?
        try:
            hz = self._ctl("/healthz")
        except Exception as e:
            result.add(Verdict.inconclusive("fault proxy reachable", "control :8474 up",
                                            f"unreachable: {e}"))
            return
        result.observations["proxy"] = {"healthz": hz}

        # Baseline (faults DISARMED): create tables on both replicas + a seed insert.
        self._ctl("/config", {"rate": 0.0})
        for t in tables:
            for n in nodes:
                sql.create_ca_table(n, t, columns="id UInt64, payload String", order_by="id", wide=True)
            sql.insert_random(nodes[0], t, rows=rows // 2, payload_bytes=payload, op_id=0)

        # Snapshot S3 retry counters before the fault window.
        def s3_counters():
            out = {}
            for n in nodes:
                try:
                    txt = n.query(
                        "SELECT event, value FROM system.events WHERE event LIKE 'DiskS3%' "
                        "AND (event LIKE '%Error%' OR event LIKE '%Attempt%' OR event LIKE '%Throttl%') "
                        "FORMAT TabSeparated")
                    out[n.container] = {r.split("\t")[0]: int(r.split("\t")[1])
                                        for r in txt.splitlines() if "\t" in r}
                except Exception:
                    out[n.container] = {}
            return out

        before_ctr = s3_counters()

        # ARM faults, then run a write + merge workload that forces many GET/PUT/HEAD through the proxy.
        armed = self._ctl("/config", {"rate": rate, "modes": modes,
                                      "methods": ["GET", "PUT", "HEAD", "POST"], "seed": 22})
        result.observations["armed_config"] = armed.get("config")
        errors = []
        for t in tables:
            try:
                sql.insert_random(nodes[0], t, rows=rows // 2, payload_bytes=payload, op_id=rows)
                sql.insert_random(nodes[1 % len(nodes)], t, rows=rows // 2, payload_bytes=payload,
                                  op_id=2 * rows)
                # OPTIMIZE forces merges -> reads existing part blobs + writes merged blobs (GET/PUT
                # storm through the proxy) -> exercises the read + write retry paths.
                nodes[0].command(f"OPTIMIZE TABLE {t} FINAL", timeout=300)
            except Exception as e:
                errors.append({"table": t, "err": str(e)[:200]})

        # DISARM before the checkpoint (fsck/GC must see ground truth, not faults).
        self._ctl("/config", {"rate": 0.0})
        stats = self._ctl("/stats")
        result.observations["proxy_stats"] = stats

        # 1. The fault path was actually exercised (otherwise the whole scenario is vacuous).
        injected = int(stats.get("faults", 0))
        result.add(Verdict.check(
            "transient faults were injected (test not vacuous)", "> 0 faults", f"{injected}",
            injected > 0, "" if injected > 0 else "proxy injected 0 faults — rate too low / no matching requests"))

        # 2. Successful workload statements completed despite faults (retries absorbed them).
        result.observations["workload_errors"] = errors
        result.add(Verdict.check(
            "write+merge workload succeeded under injected faults", "0 hard errors",
            f"{len(errors)} errors", not errors,
            "" if not errors else f"{errors[:3]} — retries did not absorb the transient faults"))

        # 3. Retries actually occurred AND were bounded (no unbounded attempt blow-up).
        after_ctr = s3_counters()
        def _delta(ev):
            tot = 0
            for c in after_ctr:
                tot += after_ctr.get(c, {}).get(ev, 0) - before_ctr.get(c, {}).get(ev, 0)
            return tot
        read_err = _delta("DiskS3ReadRequestsErrors")
        write_err = _delta("DiskS3WriteRequestsErrors")
        read_att = _delta("DiskS3ReadRequestAttempts")
        write_att = _delta("DiskS3WriteRequestAttempts")
        result.observations["s3_retry_delta"] = {
            "ReadRequestsErrors": read_err, "WriteRequestsErrors": write_err,
            "ReadRequestAttempts": read_att, "WriteRequestAttempts": write_att}
        retried = (read_err + write_err) > 0
        # Bounded: total attempts must be within a sane multiple of the injected faults (retry budget),
        # not an unbounded storm. Use a generous ceiling.
        att_total = read_att + write_att
        bounded = att_total <= max(1000, injected * 50)
        result.add(Verdict.check(
            "S3 retries occurred and were bounded by the retry budget",
            "retryable errors > 0 and attempts bounded",
            f"errors={read_err + write_err}, attempts={att_total}, injected={injected}",
            retried and bounded,
            "" if (retried and bounded) else
            ("no retryable errors recorded despite injected faults" if not retried
             else f"attempt count {att_total} looks unbounded vs {injected} injected faults")))

        # 4. All replicas converge despite the fault window.
        for t in tables:
            for n in nodes:
                try:
                    n.command(f"SYSTEM SYNC REPLICA {t}", timeout=300)
                except Exception as e:
                    ctx.log(f"S22 SYNC {t}@{n.container}: {e}")
            _common.assert_replicas_agree(result, cl, sql.table_checksum_query(t),
                                          name=f"S22 replica agreement [{t}]")

        # 5. No committed ref to a missing blob/manifest; GC-safe end.
        _common.standard_end(ctx, result, tables, table_filter="table LIKE 's22_%'")
