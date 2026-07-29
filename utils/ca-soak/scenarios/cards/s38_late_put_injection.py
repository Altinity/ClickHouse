"""S38 unclean handover, recovery seal, and late-PUT injection (P0). RETIRED PREMISE — HELD FOR T14.

THIS CARD CANNOT PASS AND FAILS AT ENTRY. Everything it asserts on (the T_mat wait and its log line,
`unclean_epoch_boundary_seen`, the sentinel recovery seal, the LIST-based late-ref-log detector) was
retired by Stage A tasks 6 and 12 — see `S38.RETIRED_PREMISE` below for the per-step mapping and the
commits. The body is preserved verbatim as `_run_retired_body` because T14's rewrite (detection ->
fence-held) needs to see what the card was actually testing; the description below is therefore a
record of the OLD mechanism, not of current behaviour.

End-to-end soak card for the rev.6 ref-lease-exclusivity plan (task 14, final task of the 14-task
plan; tasks 1-13 landed through `c1693936a3f`). Exercises the whole unclean-handover story on a real
2-node cluster against the built server binary:

  1. `kill -9` `ch1` mid append-storm, restart it. Assert from `system.text_log` on `ch1`: the
     mount-claim OBSERVATION wait fires (`CasStore.cpp`'s `on_wait_start`, "a stale-looking mount
     lease is held ... observing its write-token"), the `materialization_grace_ms` (T_mat) wait fires
     ("... follows an unclean predecessor; waiting {} ms (materialization grace) ..."), and NO
     `delete_pending retired entry recovered in-degree` sparing warning appears (`CasBlobInDegree.cpp`)
     — that class of warning means a delete-pending entry's in-degree was recovered non-structurally,
     which this clean unclean-recovery path must never trigger. A SEPARATE, quiesced
     `docker restart` (graceful `SIGTERM`, `MountPriorState::Clean` farewell) must pay NO
     `materialization grace` wait — the log line's absence is the assertion. This clean-restart check
     runs LAST in the card (after steps 2/3 below), not here: `unclean_epoch_boundary_seen` is a flag
     of the CURRENT live mount only (`CasStore.cpp:487`), so an intervening clean restart would mount
     a fresh epoch with the flag false again and the step-2 seal would never publish.

  2. Recovery seal: the lazy per-namespace recovery (`CasStore.cpp` ~line 1227-1270) — because
     `unclean_epoch_boundary_seen` was latched by the T_mat wait — publishes a recovery seal covering
     `{my_epoch-1, MAX}` and increments the `CasRefRecoverySealPublished` ProfileEvent. In practice
     this fires during ch1's own startup (table ATTACH already enumerates the CA table's committed
     manifest set), strictly BEFORE `/ping` ever answers — confirmed empirically; this card asserts
     the counter is nonzero on the freshly-restarted process rather than bracketing an explicit touch.

  3. Late-PUT injection: a dead-epoch `_log` object is injected DIRECTLY into the RustFS pool via
     `boto3` (bypassing the writer entirely — this is what a straggling / out-of-band PUT from the
     dead predecessor, arriving after the recovery seal was already published, looks like on the
     wire). The injected object's `RefTxnId` is `{dead_epoch, huge_seq}`: same epoch as the sealed
     region, a sequence number far above anything the storm could have allocated — so it is
     `sealed_from < id <= seal.snapshot_id`, the exact `reportLateLogsIfAny` (`CasOrphanManifestSweep.cpp`
     :108-150) "T_mat violation" classification. Driving GC exercises `runManifestSweepCursorPass`
     (`CasGc.cpp:1343`) -> `sweepManifestCursorPage` -> `activeManifestKeys` ->
     `reportLateLogsIfAny`, which must emit exactly one `ref_late_log_detected` CA-log event and
     NEVER apply the injected log's (zero) ops to live state — the resurrect invariant
     ([[feedback_ca_resurrect_invariant]]) for ref-log txns, not just blobs. The table's checksum is
     asserted unchanged by the injection ("queries return only sealed truth").

  NOTE on timing vs. the literal task brief: the brief describes injecting "while the successor is
  inside its T_mat wait". Reading `Store::open` (`CasStore.cpp:480-494`) and the lazy per-namespace
  seal (`CasStore.cpp:1227-1270`) together shows T_mat is specifically the window during which a
  late predecessor PUT is safely ABSORBED into the recovery LIST (born-covered, no anomaly) — an
  object landing DURING that window is exactly what T_mat exists to make safe, not a violation.
  `reportLateLogsIfAny` only fires for a log whose id is ABOVE `sealed_from`, i.e. one that lands
  AFTER the per-namespace recovery LIST already ran (which happens lazily, on first touch, strictly
  AFTER `Store::open` and its T_mat wait return). This card therefore injects AFTER the first
  post-restart touch (once the seal is durable, confirmed via `CasRefRecoverySealPublished` and a
  direct pool listing) rather than literally during the mount-time wait — that is the only timing
  that can mechanically produce a `RefLateLogDetected` event, per the cited source. The long T_mat
  configured for this variant (`docker-compose-s38.yml`, 45s vs the 30s default) is kept anyway so
  step 1's wait assertions are long and unambiguous in the logs.

  4. Regression sweep (driven by the caller as a separate step, not by this card): S13/S15/S18 once
     each on a fresh cluster, since this plan touched the fence/remount and shard-lifecycle paths
     those cards exercise.

Dev scale is deliberately small (a couple dozen small inserts) so a developer run finishes in a few
minutes despite the long T_mat wait baked into the `s38` compose variant; ci/full scale up the storm.
"""

import struct
import threading
import time

from soak.chaos import Fault, FaultTarget, FaultAction, apply_fault

from ..framework import cluster_boot, gc as gc_mod, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s38_handover"

# S3 endpoint published by docker-compose-s38.yml (rustfs1 18121 -> 11121) so the card can inject a
# raw object directly into the pool from the host process, without an extra throwaway container.
_S3_ENDPOINT = "http://localhost:18121"
_S3_BUCKET = "test"
_POOL_PREFIX = "soak_pool"   # matches observe.POOL_DIR / storage_conf.xml's endpoint sub-path

# One below the seal's own `std::numeric_limits<uint64_t>::max()` sentinel (`CasStore.cpp:1241`) —
# comfortably above anything a dev-scale storm could allocate as a real `ref_sequence`, while still
# satisfying `id <= seal.snapshot_id` (same epoch, ordinal component below the MAX sentinel).
_HUGE_SEQ = 0xFFFFFFFFFFFFFFFE


# ---------------------------------------------------------------------------
# RefLogTxn wire encoding (CasRefLogCodec.cpp encodeRefLogTxn/decodeRefLogTxn):
#   u32 format_version=1 | u32 ns_len + ns bytes | u64 writer_epoch | u64 ref_sequence | u32 op_count
# little-endian throughout; op_count=0 is a fully valid, decodable, side-effect-free transaction (no
# RefOp is written) — chosen deliberately so the injected object can never poison a REAL later fold
# (manifestEdgesOfTxn of an empty op list is empty) even though several GC/orphan-sweep code paths do
# GET+decode any log above their cursor.
# ---------------------------------------------------------------------------

def _encode_ref_log_txn(ns: str, writer_epoch: int, ref_sequence: int) -> bytes:
    ns_b = ns.encode()
    out = struct.pack("<I", 1)                      # format_version
    out += struct.pack("<I", len(ns_b)) + ns_b       # ns (len-prefixed)
    out += struct.pack("<Q", writer_epoch)
    out += struct.pack("<Q", ref_sequence)
    out += struct.pack("<I", 0)                      # op_count = 0
    return out


def _render_ref_txn_id(writer_epoch: int, ref_sequence: int) -> str:
    """Mirrors `renderRefTxnId` (CasRefIds.h): two 16-digit lowercase hex fields joined by '-'."""
    return f"{writer_epoch:016x}-{ref_sequence:016x}"


def _s3_client():
    import boto3
    from botocore.config import Config
    return boto3.client(
        "s3", endpoint_url=_S3_ENDPOINT, aws_access_key_id="clickhouse",
        aws_secret_access_key="clickhouse", region_name="us-east-1",
        config=Config(s3={"addressing_style": "path"}, retries={"max_attempts": 5}))


def _list_common_prefixes(s3, prefix: str) -> list:
    resp = s3.list_objects_v2(Bucket=_S3_BUCKET, Prefix=prefix, Delimiter="/")
    return [p["Prefix"] for p in resp.get("CommonPrefixes", [])]


# `RootNamespace` is per-SERVER (`server_root_id`, e.g. "ca_soak_ch1"), not per-table: a bare
# `cas/refs/<server_root_id>/` LIST shows only ONE child, "store/" — the actual per-table namespace
# nests further, `store/<uuid-shard3>/<uuid>@cas@/`, ending in the `_log`/`_snap`/`_cleanup` siblings
# (confirmed empirically against a live pool: `cas/refs/ca_soak_ch1/store/b73/<uuid>@cas@/_log/`).
# Walk down while each level has exactly one child (true for a single-table pool) until a level's
# children include one of the three ref-object leaf names; that walked path IS the namespace string
# `refsNamespacePrefix` expects. Returns None (ambiguous/unexpected shape) rather than guessing.
_REF_LEAF_NAMES = {"_log", "_snap", "_cleanup"}


def _discover_table_namespace(s3, server_root_id: str, max_depth: int = 8):
    cur = f"{_POOL_PREFIX}/cas/refs/{server_root_id}/"
    for _ in range(max_depth):
        children = _list_common_prefixes(s3, cur)
        leafs = {c.rstrip("/").rsplit("/", 1)[-1] for c in children}
        if leafs & _REF_LEAF_NAMES:
            return cur[len(f"{_POOL_PREFIX}/cas/refs/"):].rstrip("/")
        if len(children) != 1:
            return None   # ambiguous (0 or >1 children before reaching a leaf) — never guess
        cur = children[0]
    return None


def _text_log_count(node, since: str, needle: str) -> int:
    """Count `system.text_log` rows containing `needle` (case-insensitive) at/after `since`. Flushes
    logs first (system log tables buffer in memory) — mirrors observe.gc_log_rows's flush pattern."""
    try:
        node.command("SYSTEM FLUSH LOGS")
        v = node.scalar(
            f"SELECT count() FROM system.text_log WHERE event_time >= '{since}' "
            f"AND message ILIKE '%{needle}%'")
        return int(v or 0)
    except Exception:
        return -1   # probe failure is distinct from a genuine 0 — caller treats <0 as inconclusive


@register
class S38(Scenario):
    name = "S38"
    title = "unclean handover, recovery seal, late-PUT injection"
    priority = "P0"
    compose_variant = "s38"
    param_table = {
        "dev": {"storm_inserts": 20, "rows_per_insert": 50, "payload_bytes": 512,
                "kill_delay_s": 1.5, "kill_down_s": 3, "heal_timeout_s": 180},
        "ci": {"storm_inserts": 60, "rows_per_insert": 300, "payload_bytes": 1024,
               "kill_delay_s": 2.0, "kill_down_s": 4, "heal_timeout_s": 240},
        "full": {"storm_inserts": 150, "rows_per_insert": 1000, "payload_bytes": 2048,
                  "kill_delay_s": 3.0, "kill_down_s": 5, "heal_timeout_s": 300},
    }

    # The premise this card was written against no longer exists. Raised as the FIRST statement of
    # `run`, before the card touches the cluster: the runner turns a raised exception into a FAIL, so
    # this is loud and it is not a skip. It is deliberately NOT `needs_infra` (that yields
    # INCONCLUSIVE, i.e. a silent skip, which the house rule forbids) and deliberately not a late
    # assertion — a card that boots, runs a storm, restarts a node and only then fails on a log line
    # that can never appear reads like a product bug, which is a worse state than failing at entry.
    RETIRED_PREMISE = (
        "S38's premise was retired by Stage A. This card asserts on mechanisms that no longer exist, "
        "so it CANNOT pass and must not be read as a product failure:\n"
        "  - the `materialization grace` (T_mat) LOG_INFO line of step 1 — the wait itself, and its "
        "`materialization_grace_ms` setting, were retired OUTRIGHT by Stage A task 12 "
        "(`ff9f36a056f`); recovery fences a dying epoch's straggler with an in-band EpochSeal "
        "written as a conditional create instead of waiting for it;\n"
        "  - `unclean_epoch_boundary_seen`, the flag step 1 latches and step 2 depends on — retired "
        "by the same commit (sealing is decided by arithmetic: every epoch below the live one is "
        "closed, however its mount died);\n"
        "  - `CasRefRecoverySealPublished` and the sentinel recovery seal of step 2 — retired "
        "EARLIER, by Stage A task 6 (`6f06ba05815`);\n"
        "  - `reportLateLogsIfAny` / the `ref_late_log_detected` event of step 3 — the LIST-based "
        "late-ref-log detector, retired by task 6's `d74c726ef9e`.\n"
        "So this card was already broken before task 12; task 12 only moved the failure earlier. "
        "The rewrite (detection -> fence-held) is T14's; until then this entry guard is the "
        "disposition. Do not 'fix' it by deleting assertions — the scenario it describes still needs "
        "an equivalent, and T14 owes it."
    )

    def run(self, ctx, result):
        raise RuntimeError(self.RETIRED_PREMISE)

    def _run_retired_body(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        storm_inserts = int(p["storm_inserts"])
        rows = int(p["rows_per_insert"])
        payload = int(p["payload_bytes"])
        kill_delay_s = float(p["kill_delay_s"])
        kill_down_s = int(p["kill_down_s"])
        heal_timeout_s = int(p["heal_timeout_s"])
        result.observations["scale"] = {
            "storm_inserts": storm_inserts, "rows_per_insert": rows, "payload_bytes": payload,
            "note": "DEV-scale: a couple dozen small inserts before the kill; ci/full scale up the "
                    "storm. The T_mat wait itself (45s, docker-compose-s38.yml) is FIXED across "
                    "scales — it is a config knob of the variant, not a workload-size parameter.",
        }

        sql.create_ca_table(cl.node1, _TABLE, columns="id UInt64, payload String", order_by="id",
                            wide=True)
        sql.create_ca_table(cl.node2, _TABLE, columns="id UInt64, payload String", order_by="id",
                            wide=True)

        # =====================================================================================
        # Step 1: kill -9 ch1 mid append-storm, restart, assert observation + T_mat wait lines.
        # =====================================================================================
        since_kill = cl.node1.scalar("SELECT toString(now())")
        stop = threading.Event()
        wl_stats = {"inserts_ok": 0, "inserts_failed": 0}

        def _storm():
            i = 0
            while not stop.is_set() and i < storm_inserts:
                try:
                    sql.insert_random(cl.node1, _TABLE, rows=rows, payload_bytes=payload, op_id=i * rows)
                    wl_stats["inserts_ok"] += 1
                except Exception as e:
                    wl_stats["inserts_failed"] += 1
                    ctx.log(f"S38 storm insert failed (adversarial, continuing): {str(e)[:160]}")
                i += 1

        wl_thread = threading.Thread(target=_storm, daemon=True)
        wl_thread.start()
        time.sleep(kill_delay_s)
        ctx.log(f"S38: KILL ch1 mid append-storm (down {kill_down_s}s)")
        apply_fault(Fault(t_offset=0, target=FaultTarget.CH1, action=FaultAction.KILL,
                          duration_s=kill_down_s))
        stop.set()
        wl_thread.join(timeout=120)
        result.observations["storm_stats"] = wl_stats

        healthy = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
        result.add(Verdict.check(
            "ch1 recovers after kill -9", "healthy within heal_timeout_s",
            f"healthy={healthy}", healthy,
            "" if healthy else "ch1 did not answer /ping within the heal timeout — the T_mat wait "
                               "(45s) plus observation wait must both fit inside it"))
        if not healthy:
            # Cannot proceed meaningfully; still run the common end checkpoint so the pool is left
            # observable, and stop here (every remaining verdict would be spurious).
            _common.standard_end(ctx, result, [_TABLE])
            return

        obs_n = _text_log_count(cl.node1, since_kill, "stale-looking mount lease")
        tmat_n = _text_log_count(cl.node1, since_kill, "materialization grace")
        sparing_n = _text_log_count(cl.node1, since_kill, "delete_pending retired entry recovered in-degree")
        result.observations["unclean_restart_log_counts"] = {
            "observation_wait": obs_n, "t_mat_wait": tmat_n, "in_degree_sparing_warning": sparing_n}
        result.add(Verdict.check(
            "observation wait line appears (unclean restart)",
            ">0 'stale-looking mount lease' rows in system.text_log since kill",
            obs_n, obs_n > 0,
            "" if obs_n > 0 else "ch1's restart did not log the mount-claim observation wait — "
                                 "either the predecessor's death looked clean, or the log is missing"))
        result.add(Verdict.check(
            "T_mat (materialization grace) wait line appears (unclean restart)",
            ">0 'materialization grace' rows in system.text_log since kill",
            tmat_n, tmat_n > 0,
            "" if tmat_n > 0 else "ch1's restart did not pay/log the T_mat wait after an unclean kill"))
        result.add(Verdict.check(
            "no in-degree sparing warning (delete_pending retired entry recovered)",
            "0 'delete_pending retired entry recovered in-degree' rows",
            sparing_n, sparing_n == 0,
            "" if sparing_n == 0 else "the unclean-handover recovery triggered a "
                                     "delete_pending/in-degree sparing warning — unexpected on a "
                                     "clean append-storm kill with no prior GC condemnation"))

        # NOTE on ordering: the clean stop/start check (below, "must pay no T_mat wait") is
        # deliberately run AFTER step 2, not here. `unclean_epoch_boundary_seen` is a flag of the
        # CURRENT live mount (CasStore.cpp:487), latched true only by THIS restart's unclean
        # predecessor observation; a further clean restart in between would mount a fresh epoch with
        # the flag false again, and the lazy per-namespace seal (gated on that flag,
        # CasStore.cpp:1245) would then never publish when the table is first touched. Step 2 must
        # therefore touch the table while still on the epoch this unclean restart just mounted.

        # =====================================================================================
        # Step 2: recovery seal, then late-PUT injection + sweep assertion (still on the epoch this
        # unclean restart just mounted — see the ordering note above).
        # =====================================================================================
        # `CasRefRecoverySealPublished` is a ProfileEvents counter -- reset to 0 whenever the server
        # process restarts (system.events is in-memory, per-process). A first dry run bracketing an
        # explicit post-healthy touch (before/after around a manual SELECT) found the counter ALREADY
        # at 1 on the "before" side: table ATTACH during ch1's own startup (loading its databases and
        # tables, which requires enumerating this CA-backed MergeTree's committed manifest set)
        # already triggers the lazy per-namespace ref recovery, BEFORE the HTTP server even opens for
        # `/ping`. So the right assertion is simply "the freshly-restarted process's counter is > 0"
        # -- there is no meaningful "before" to bracket against once the process has restarted.
        pre_inject_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
        seal_events_after = observe.events_snapshot(cl.node1).get("CasRefRecoverySealPublished", 0)
        seal_published = seal_events_after > 0
        result.observations["recovery_seal"] = {"CasRefRecoverySealPublished": seal_events_after}
        result.add(Verdict.check(
            "recovery seal published by the unclean restart",
            "CasRefRecoverySealPublished > 0 (system.events, fresh since this process's start)",
            seal_events_after, seal_published,
            "" if seal_published else "no CasRefRecoverySealPublished increment since ch1 restarted "
                                      "— either the unclean-epoch boundary was not latched, or the "
                                      "table's dead region was empty (the storm inserts never landed)"))

        if not seal_published:
            # Nothing to inject against meaningfully; record why and skip straight to the common end.
            result.add(Verdict.inconclusive(
                "RefLateLogDetected fires for an injected dead-epoch late log",
                ">0 ref_late_log_detected CA-log events after injection + driven GC",
                "no recovery seal was published — skipping the injection (it targets the sealed "
                "region and has nothing to be 'late' relative to)"))
            _common.standard_end(ctx, result, [_TABLE])
            return

        s3 = _s3_client()
        # RootNamespace is per-SERVER (server_root_id="ca_soak_ch1" for ch1, storage_conf_s38_ch1.xml)
        # with the per-table path nested underneath it — walk down to the actual ref-object leaf.
        ns = _discover_table_namespace(s3, "ca_soak_ch1")
        result.observations["discovered_namespace"] = ns
        if ns is None:
            result.add(Verdict.inconclusive(
                "namespace discovered for injection",
                "a walk from cas/refs/ca_soak_ch1/ reaches a _log/_snap/_cleanup leaf",
                "namespace walk did not resolve to an unambiguous single leaf (see cas/refs/ "
                "listing in the run log)"))
            _common.standard_end(ctx, result, [_TABLE])
            return

        manifest_prefixes = _list_common_prefixes(s3, f"{_POOL_PREFIX}/cas/manifests/{ns}/")
        epochs = []
        for mp in manifest_prefixes:
            leaf = mp.rstrip("/").rsplit("/", 1)[-1]
            if "-" in leaf:
                hi = leaf.split("-", 1)[0]
                try:
                    epochs.append(int(hi, 16))
                except ValueError:
                    pass
        dead_epoch = min(epochs) if epochs else None
        result.observations["manifest_build_prefixes"] = manifest_prefixes
        result.observations["dead_epoch"] = dead_epoch
        if dead_epoch is None:
            result.add(Verdict.inconclusive(
                "dead epoch discovered for injection", "at least one cas/manifests/<ns>/<epoch>-.../ prefix",
                f"none found under manifests for ns={ns!r}: {manifest_prefixes}"))
            _common.standard_end(ctx, result, [_TABLE])
            return

        injected_id = _render_ref_txn_id(dead_epoch, _HUGE_SEQ)
        injected_key = f"{_POOL_PREFIX}/cas/refs/{ns}/_log/{injected_id}"
        injected_body = _encode_ref_log_txn(ns, dead_epoch, _HUGE_SEQ)
        ctx.log(f"S38: injecting dead-epoch late log at s3://{_S3_BUCKET}/{injected_key} "
                f"({len(injected_body)} bytes)")
        s3.put_object(Bucket=_S3_BUCKET, Key=injected_key, Body=injected_body)
        result.observations["injected_log"] = {"ns": ns, "key": injected_key, "txn_id": injected_id}

        since_inject = cl.node1.scalar("SELECT toString(now())")
        # Attempt-2 (2026-07-14 run 20260714T115429) found this loop drove GC ONLY on node_index=0
        # (ch1) — but ch1 was NotALeader for the CA GC lease across the entire run (ch2 held it
        # throughout, confirmed via `system.content_addressed_garbage_collection_log`: 339/339 ch1
        # attempts NotALeader). Drive BOTH nodes every cycle so whichever actually holds the lease
        # makes progress, and track REAL successful-round count (not just "attempts issued") for a
        # round-completion-aware budget instead of a bare wall-clock cutoff.
        late_log_count = 0
        attempts = 0
        real_rounds_seen = 0
        deadline = time.monotonic() + 240
        min_real_rounds = 5   # keep polling until we've observed this many real Success rounds pool-wide
        while time.monotonic() < deadline:
            for idx in range(len(cl.nodes())):
                gc_mod.gc_drive_round(cl, log_fn=ctx.log, node_index=idx)
            attempts += 1
            gc_all = observe.gc_log_all(cl, since_inject)
            real_rounds_seen = gc_all.get("summary", {}).get("success", 0)
            ca_events = observe.ca_event_counts_all(cl, since_inject)
            late_log_count = sum(
                int(c.get("by_event_type", {}).get("ref_late_log_detected", 0) or 0)
                for c in ca_events.get("per_node", {}).values())
            if late_log_count > 0:
                break
            if real_rounds_seen >= min_real_rounds and attempts >= min_real_rounds:
                break   # gave the sweep a generous, confirmed number of real rounds; stop waiting
            time.sleep(6)
        result.observations["late_log_detection"] = {
            "poll_attempts": attempts, "real_success_rounds_since_inject": real_rounds_seen,
            "count": late_log_count}
        result.add(Verdict.check(
            "RefLateLogDetected fires for an injected dead-epoch late log",
            ">0 ref_late_log_detected CA-log events after injection + driven GC",
            late_log_count, late_log_count > 0,
            "" if late_log_count > 0 else
            f"no ref_late_log_detected event after {attempts} poll cycles driving both nodes "
            f"({real_rounds_seen} confirmed real Success rounds pool-wide since injection) — the "
            "manifest sweep cursor pass did not report the injected id as late within this budget; "
            "see reportLateLogsIfAny, CasOrphanManifestSweep.cpp. If this reproduces with a confirmed "
            "healthy multi-round GC leader window, treat as a product observation (a structurally "
            "narrow/slow detection window), not a card defect — do not keep re-extending the budget"))

        tmat_violation_n = _text_log_count(cl.node1, since_inject, "CAS T_mat violation")
        result.observations["t_mat_violation_log_rows"] = tmat_violation_n
        result.add(Verdict(
            "T_mat violation warning logged", "'CAS T_mat violation' row corroborates the CA-log event",
            tmat_violation_n, "pass" if tmat_violation_n > 0 else "inconclusive",
            "" if tmat_violation_n > 0 else "no corroborating text_log row (non-fatal; the CA-log "
                                            "event above is the authoritative assertion)"))

        post_inject_checksum = cl.node1.query(sql.table_checksum_query(_TABLE)).strip()
        unaffected = post_inject_checksum == pre_inject_checksum
        result.observations["checksums"] = {
            "pre_inject": pre_inject_checksum, "post_inject": post_inject_checksum}
        result.add(Verdict.check(
            "queries return only sealed truth (injection has no observable effect)",
            "table checksum unchanged by the injected dead-epoch log",
            f"pre={pre_inject_checksum!r} post={post_inject_checksum!r}", unaffected,
            "" if unaffected else "the table's queryable state CHANGED after injecting a dead-epoch "
                                  "log — it must never be applied/revived (resurrect invariant)"))

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(_TABLE),
                                      name="S38 replica agreement")

        # =====================================================================================
        # Step 1 (continued): clean stop/start pays NO T_mat wait. Run LAST (see the ordering note
        # above step 2) — the injected late log has already been swept-and-reported by now, so a
        # further mount here cannot disturb those assertions.
        # =====================================================================================
        since_clean = cl.node1.scalar("SELECT toString(now())")
        ctx.log("S38: graceful `docker restart` ch1 (clean farewell) — must pay no T_mat wait")
        apply_fault(Fault(t_offset=0, target=FaultTarget.CH1, action=FaultAction.RESTART, duration_s=0))
        healthy_clean = cluster_boot.wait_healthy(cl, timeout_s=heal_timeout_s, log_fn=ctx.log)
        clean_tmat_n = _text_log_count(cl.node1, since_clean, "materialization grace") if healthy_clean else -1
        result.observations["clean_restart"] = {"healthy": healthy_clean, "t_mat_wait_rows": clean_tmat_n}
        result.add(Verdict.check(
            "clean stop/start pays no T_mat wait", "0 'materialization grace' rows since the clean restart",
            clean_tmat_n, clean_tmat_n == 0,
            "" if clean_tmat_n == 0 else
            ("clean restart did not become healthy" if not healthy_clean else
             "a graceful docker restart still paid the T_mat wait — the drained clean-release "
             "farewell (Task 5) should make MountPriorState::Clean, skipping the wait entirely")))
        if not healthy_clean:
            _common.standard_end(ctx, result, [_TABLE])
            return

        _common.standard_end(ctx, result, [_TABLE])
