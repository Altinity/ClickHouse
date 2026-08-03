"""S42: allocation-fault soak — exception safety of the CAS write path (post-durable install window).

The suite faults nodes (kill/restart) and the object store (`s3faultproxy`), but never the
ALLOCATOR. This card makes allocation failure a first-class fault source, and it exists for ONE
window: a ref-log transaction whose PUT has already succeeded, where the in-memory apply then
throws. That window leaves the writer cache MISSING a durable transaction — a data-loss class,
because a snapshot published from a poisoned cache can hide the transaction permanently (design
`docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md`
§[Problem 2](#problem-ledger)). §A1 made the three install regions allocation-free, and §A2's lane
state machine (`RefLaneState`, `ProfileEvents::CASRefNeedsRecovery`) is what must catch anything
that still slips through.

Legs (leg B is deliberately ABSENT — see below):

- **Leg A — query-thread allocation faults.** `memory_tracker_fault_probability` (per-query `Float`
  Setting, `Core/Settings.cpp`; NOT debug-gated, `MemoryTracker.cpp`) armed through the driver's URL
  parameters over a soak-shaped insert/select workload, plus a short high-probability burst. Paired
  with `max_untracked_memory=0` so that SMALL allocations reach the tracker at all — with the 4 MiB
  default slack most of the CAS commit path's allocations never consult the tracker and the fault
  probability simply cannot bite there. The ref append lane runs on the CALLER's thread
  (`CasRefLedger::appendRefOps`), which is why this per-query knob reaches the CAS commit path.
- **Leg B — NOT HERE.** Thread-allocation faults
  (`cannot_allocate_thread_fault_injection_probability`) are scenario **S43**. They are a different
  fault CLASS with a different blast radius: they reach thread-CREATING paths (the background
  snapshot dispatcher, pools), and cannot reach the ref append lane at all. Mixing them into this
  card destroys attribution when something breaks. Do not add them back here.
- **Leg C — disarm, quiesce, GC to fixpoint, fsck, restart, compare.** The durable journal rebuild must
  reproduce the pre-restart view. Fsck derives its ref view only from catalog + exact `_ckpt` authority;
  unadopted snapshots visible to LIST are inert garbage, never an integrity oracle.

**What green means (2026-07-25 decision).** Green is A CONSISTENT STATE ON DISK AND IN MEMORY, not
proof that a fault landed in the post-durable install window. The verdict rests on the consistency
oracle: the post-restart (journal-rebuilt) view identical to the pre-restart view, every acked
block present, replicas agreeing, fsck `dangling`/`unaccounted`/`stale_edge` clean pre- and
post-restart, zero `LOGICAL_ERROR`, no wedged ref lane, GC
rounds succeeding after disarm.

**Anti-vacuity, which survives.** A run in which no allocation fault occurred at all still cannot
read green: `generic == 0` (client-visible injected failures plus the `QueryMemoryLimitExceeded`
delta) is `inconclusive`. Only the WINDOW-SPECIFIC targeting was dropped as a gate.

**The targeted signal is reported, not gating.**

  targeted = CASRefNeedsRecovery transitions + post-PUT apply failpoint hits

is structurally 0 today: the §A1 seam is the gtest-only `CasRefLedger::setInstallRegionProbeForTest`
with no `src/Common/FailPoint.cpp` registration, and `CASRefNeedsRecovery` is correctly 0 while §A1
holds. The card records it and says so. If poison ever DOES fire, the `CASRefNeedsRecovery == 0`
verdict is a real `check` and the run fails — that half is unchanged.

**Oracle (queries ARE allowed to fail; invariants are not):** zero `LOGICAL_ERROR`/abort; every ACKED
insert's rows present (S40-shaped, block-granular); replicas agree; fsck `dangling=0`,
`unaccounted=0`, `stale_edge=0` (detail mode only — the counter is not computed by a summary scan);
GC rounds succeed again after disarm; no permanently wedged ref lane; no query
hung past a bound.

**Reported, never gating:** `CASGCUnmatchedRemoveDeltas` (removal deltas reaching the in-degree
reducer without their matching activation). Its benign rate is not characterised yet, so this card
records it and says so — it does not fail on it.
"""

import threading
import time

from soak.chaos import Fault, FaultAction, FaultTarget, apply_fault
from ..framework import cluster_boot, gc as gc_mod, lifecycle, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s42_alloc"

# Events read straight from `system.events`: the shared `observe.events_snapshot` filter keeps only
# `CAS*`/`DiskS3*`/`S3*`, and the generic-vs-targeted distinction this card is built on needs
# `QueryMemoryLimitExceeded` (the injected-fault counter) next to the CAS ones.
_EVENTS_OF_INTEREST = (
    "CASRefNeedsRecovery",            # TARGETED: transitions into `RefLaneState::NeedsRecovery`
    "QueryMemoryLimitExceeded",       # generic: every injected allocation fault that threw
    "CASRefAppendWedged", "CASRefAppendUnwedged", "CASRefAppendDefiniteFailure",
    "CASRefBatchFlushes", "CASRefBatchedMutations",
    "CASRefSnapshotPublishDispatched", "CASRefSnapshotPublishBackoff",
    "CASRefRecoveryEpochSealed",
    "CASGCUnmatchedRemoveDeltas",     # reported only, never gating (benign rate uncharacterised)
)

# The transition's log line (`CasRefLedger::requireRecovery`). Corroboration only: the LOG_ERROR
# allocates and is wrapped in a swallow-everything catch precisely because it may fail under memory
# pressure, so the ProfileEvent — incremented BEFORE it — is the authoritative transition count.
_POISON_LOG_NEEDLE = "NEEDS RECOVERY at"


def _events(node) -> dict:
    """Absolute values of `_EVENTS_OF_INTEREST` on one node. `system.events` omits zero-valued rows,
    so a missing key means zero — never "unknown"."""
    names = "','".join(_EVENTS_OF_INTEREST)
    try:
        txt = node.query(
            f"SELECT event, value FROM system.events WHERE event IN ('{names}') FORMAT TabSeparated")
    except Exception:
        return {}
    out = {}
    for line in txt.splitlines():
        if "\t" in line:
            k, v = line.split("\t", 1)
            try:
                out[k] = int(v)
            except ValueError:
                pass
    return {k: out.get(k, 0) for k in _EVENTS_OF_INTEREST}


def _cluster_events(cluster) -> dict:
    return {n.container: _events(n) for n in cluster.nodes()}


def _event_total(snap: dict, name: str) -> int:
    return sum(int(per.get(name, 0)) for per in snap.values())


def _text_log_count(node, since: str, needle: str) -> int:
    """Count `system.text_log` rows containing `needle` at/after `since`; -1 when the probe itself
    failed (distinct from a genuine 0, so a caller never reads a broken probe as "never happened")."""
    try:
        node.command("SYSTEM FLUSH LOGS")
        return int(node.scalar(
            f"SELECT count() FROM system.text_log WHERE event_time >= '{since}' "
            f"AND message ILIKE '%{needle}%'") or 0)
    except Exception:
        return -1


def _post_put_failpoint_hits() -> tuple:
    """(hits, why) for the second half of the targeted signal.

    Structurally 0: the §A1 post-durable-install seam is a gtest-only C++ hook
    (`setInstallRegionProbeForTest` / `CarvePhaseForTest::PostDurableInstall`) and `FailPoint.cpp`
    registers no CAS failpoint, so a running server exposes nothing to arm. Kept as an explicit term
    of the guard rather than dropped, so the day a server-reachable failpoint lands the guard becomes
    satisfiable by construction instead of by re-reading this file."""
    return 0, ("no server-reachable post-durable-install failpoint exists: the §A1 seam is the "
               "gtest-only `CasRefLedger::setInstallRegionProbeForTest` hook and no CAS failpoint is "
               "registered in src/Common/FailPoint.cpp")


def _block_counts(node, rows_per_insert: int) -> dict:
    """{block_index: row_count} for the table. Ids are minted as `block_index * rows_per_insert +
    0..rows-1`, so `intDiv` recovers the block an id belongs to and one query gives the whole
    acked-vs-lost picture."""
    txt = node.query(
        f"SELECT intDiv(id, {rows_per_insert}) AS b, count() FROM {_TABLE} GROUP BY b FORMAT TabSeparated")
    out = {}
    for line in txt.splitlines():
        if "\t" in line:
            b, c = line.split("\t", 1)
            out[int(b)] = int(c)
    return out


def _view(node, rows_per_insert: int) -> dict:
    """The comparable view of one replica: content checksum, active part names, per-block row counts.
    This is what must be IDENTICAL across the restart — the post-restart side is rebuilt from the
    durable journal, so a divergence is a diverged writer cache."""
    checksum = node.query(sql.table_checksum_query(_TABLE)).strip()
    parts = sorted(node.query(
        f"SELECT name FROM system.parts WHERE table = '{_TABLE}' AND active "
        f"ORDER BY name FORMAT TabSeparated").split())
    return {"checksum": checksum, "active_parts": parts,
            "block_counts": _block_counts(node, rows_per_insert)}


@register
class S42(Scenario):
    name = "S42"
    title = "allocation-fault soak (query-thread): exception safety of the CAS post-durable window"
    priority = "P0"
    # Injected `MEMORY_LIMIT_EXCEEDED` throws land inside CAS publishes, so `exception` rows in
    # `system.cas_log` are EXPECTED here. Every other bad event type still gates.
    expect_exception = True

    # `fault_probability` is per TRACKED ALLOCATION, and `max_untracked_memory=0` makes every
    # allocation tracked — so the per-query failure odds are roughly `p * allocations_per_query`.
    # MEASURED on the 2026-07-25 dev smoke: a 200-row × 512 B INSERT costs ~1.7e3 tracked
    # allocations (6 injected failures over 1695 acked inserts at p=2e-6), so the rows below aim at
    # a few percent of statements failing in the steady window (the acked-vs-lost oracle needs acked
    # volume) and a large fraction failing in the burst (to sweep the rarer code paths).
    param_table = {
        "dev": {"fault_probability": 2e-5, "burst_probability": 2e-4,
                "workload_s": 90, "burst_s": 15, "writers": 3, "readers": 2,
                "rows_per_insert": 400, "payload_bytes": 512,
                "query_timeout_s": 120, "join_bound_s": 180, "settle_s": 15,
                "restart_timeout_s": 240, "min_acked_blocks": 20},
        "ci": {"fault_probability": 5e-5, "burst_probability": 5e-4,
               "workload_s": 420, "burst_s": 45, "writers": 5, "readers": 3,
               "rows_per_insert": 800, "payload_bytes": 1024,
               "query_timeout_s": 180, "join_bound_s": 300, "settle_s": 30,
               "restart_timeout_s": 300, "min_acked_blocks": 100},
        "full": {"fault_probability": 1e-4, "burst_probability": 1e-3,
                 "workload_s": 1800, "burst_s": 120, "writers": 8, "readers": 4,
                 "rows_per_insert": 2000, "payload_bytes": 2048,
                 "query_timeout_s": 300, "join_bound_s": 600, "settle_s": 60,
                 "restart_timeout_s": 420, "min_acked_blocks": 400},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        node = cl.node1
        since = ctx.extra.get("since_event_time") or ""
        rows_per_insert = int(p["rows_per_insert"])
        payload = int(p["payload_bytes"])
        query_timeout = float(p["query_timeout_s"])

        for n in cl.nodes():
            sql.create_ca_table(n, _TABLE, columns="id UInt64, payload String", order_by="id", wide=True)

        # ---- arming self-check ------------------------------------------------------------------
        # Distinguishes "the knob does not reach the server" (harness fault) from "faults fired but
        # never inside the target window" (the interesting outcome). Without it, a zero fault count
        # is unattributable.
        armed_ok, armed_obs = False, ""
        try:
            node.query("SELECT sum(number) FROM numbers(20000000)", timeout=60,
                       settings={"memory_tracker_fault_probability": 0.001,
                                 "max_untracked_memory": 0})
            armed_obs = "probe query SUCCEEDED with p=1e-3 over 2e7 numbers"
        except Exception as e:
            body = str(e)
            armed_ok = "fault injected" in body
            armed_obs = ("MEMORY_LIMIT_EXCEEDED 'fault injected'" if armed_ok
                         else f"unexpected error: {body[:200]}")
        result.add(Verdict.check(
            "allocation-fault injection is armable through the driver's URL parameters",
            "a probe query throws MEMORY_LIMIT_EXCEEDED ... fault injected", armed_obs, armed_ok,
            "" if armed_ok else "the per-query knob never threw — memory_tracker_fault_probability "
                                "did not reach the query's tracker, so leg A tests nothing"))

        ev_before = _cluster_events(cl)
        result.observations["events_before"] = ev_before

        # ---- Leg A: soak-shaped insert/select workload under armed allocation faults --------------
        acked_blocks: set = set()
        acked_lock = threading.Lock()
        next_block = [0]
        block_lock = threading.Lock()
        injected_client_failures = [0]     # client-visible MEMORY_LIMIT_EXCEEDED (the generic signal)
        other_failures: list = []          # anything else a statement returned — triage material
        select_faults = [0]
        max_query_s = [0.0]
        stop_at = [time.time() + float(p["workload_s"])]
        probability = [float(p["fault_probability"])]

        def _settings():
            return {"memory_tracker_fault_probability": probability[0],
                    "max_untracked_memory": 0,
                    "async_insert": 0,
                    "max_insert_threads": 1}

        def _record_failure(exc):
            body = str(exc)
            if "fault injected" in body or "MEMORY_LIMIT_EXCEEDED" in body or "Code: 241" in body:
                injected_client_failures[0] += 1
            else:
                other_failures.append(body[:300])

        def writer():
            while time.time() < stop_at[0]:
                with block_lock:
                    b = next_block[0]
                    next_block[0] += 1
                base = b * rows_per_insert
                t0 = time.monotonic()
                try:
                    # One block per statement (rows_per_insert stays well under
                    # max_insert_block_size), so an INSERT either commits the whole block or nothing
                    # — which is what makes the acked-vs-lost oracle block-granular and exact.
                    node.query(
                        f"INSERT INTO {_TABLE} SELECT {base} + number AS id, "
                        f"randomString({payload}) AS payload FROM numbers({rows_per_insert})",
                        timeout=query_timeout, settings=_settings())
                    with acked_lock:
                        acked_blocks.add(b)
                except Exception as e:
                    _record_failure(e)
                finally:
                    max_query_s[0] = max(max_query_s[0], time.monotonic() - t0)

        def reader():
            while time.time() < stop_at[0]:
                t0 = time.monotonic()
                try:
                    node.query(f"SELECT count(), sum(sipHash64(id)), max(length(payload)) "
                               f"FROM {_TABLE} WHERE id % 7 = 0",
                               timeout=query_timeout, settings=_settings())
                except Exception:
                    select_faults[0] += 1
                finally:
                    max_query_s[0] = max(max_query_s[0], time.monotonic() - t0)
                time.sleep(0.2)

        threads = ([threading.Thread(target=writer, daemon=True) for _ in range(int(p["writers"]))]
                   + [threading.Thread(target=reader, daemon=True) for _ in range(int(p["readers"]))])
        ctx.log(f"S42 leg A: armed workload for {p['workload_s']}s at p={probability[0]}")
        for t in threads:
            t.start()

        # A merge is issued from an armed query thread, but the merge itself runs on a background
        # pool that this per-query knob does NOT reach — recorded here so nobody later reads the
        # OPTIMIZE as evidence that merges were fault-tested (they were not; that is S43's class).
        time.sleep(min(float(p["workload_s"]) / 2, 30))
        try:
            node.query(f"OPTIMIZE TABLE {_TABLE}", timeout=query_timeout, settings=_settings())
        except Exception as e:
            _record_failure(e)

        for t in threads:
            t.join(timeout=float(p["join_bound_s"]))
        hung = [t for t in threads if t.is_alive()]

        # ---- Leg A burst: short high-probability window ------------------------------------------
        probability[0] = float(p["burst_probability"])
        stop_at[0] = time.time() + float(p["burst_s"])
        ctx.log(f"S42 leg A burst: {p['burst_s']}s at p={probability[0]}")
        burst = [threading.Thread(target=writer, daemon=True) for _ in range(int(p["writers"]))]
        for t in burst:
            t.start()
        for t in burst:
            t.join(timeout=float(p["join_bound_s"]))
        hung += [t for t in burst if t.is_alive()]

        ev_armed = _cluster_events(cl)
        armed_delta = {k: _event_total(ev_armed, k) - _event_total(ev_before, k)
                       for k in _EVENTS_OF_INTEREST}
        result.observations["events_delta_armed_window"] = armed_delta
        result.observations["leg_a"] = {
            "acked_blocks": len(acked_blocks), "attempted_blocks": next_block[0],
            "injected_client_failures": injected_client_failures[0],
            "select_failures": select_faults[0],
            "other_failures": other_failures[:10], "other_failure_count": len(other_failures),
            "max_query_seconds": round(max_query_s[0], 1),
        }
        ctx.write_json("s42_leg_a.json", {
            "acked_blocks": sorted(acked_blocks), "armed_delta": armed_delta,
            "other_failures": other_failures[:50]})

        result.add(Verdict.check(
            "no query hung past its bound",
            f"every workload thread joins within {p['join_bound_s']}s", f"{len(hung)} still alive",
            not hung,
            "" if not hung else "a statement never returned under allocation faults — a hang, not a "
                                "failure; the fault contract is 'queries may fail', not 'queries may block'"))
        result.add(Verdict.check(
            "workload still made progress under faults (acked volume for the loss oracle)",
            f"acked blocks >= {int(p['min_acked_blocks'])}", f"{len(acked_blocks)}",
            len(acked_blocks) >= int(p["min_acked_blocks"]),
            "" if len(acked_blocks) >= int(p["min_acked_blocks"]) else
            "too few acked inserts — the acked-vs-lost oracle below is near-vacuous; lower "
            "fault_probability for this scale (never the guard)"))

        # ---- Leg C: disarm -> quiesce -> GC to fixpoint -> fsck -> restart -> compare -------------
        # Disarm is implicit and total: the knob is per-query and was only ever sent as a URL
        # parameter, so no statement issued from here on carries it. Proven, not assumed, by the
        # unarmed statements below succeeding.
        disarm_since = node.scalar("SELECT toString(now())")
        time.sleep(float(p["settle_s"]))

        ctx.log("S42 leg C: quiescing")
        lifecycle.quiesce_cluster(cl, [_TABLE], log_fn=ctx.log)

        ctx.log("S42 leg C: driving GC to fixpoint")
        residual, gc_history = gc_mod.forced_gc_to_fixpoint(
            cl, lifecycle.unreachable_probe(), log_fn=ctx.log)
        result.observations["leg_c_gc"] = {"residual_unreachable": residual,
                                           "rounds": len(gc_history)}

        # fsck in DETAIL mode: `stale_edge` and the per-object classes are detail-only, and a clean
        # SUMMARY says nothing about stale edges (`FsckReport::clean`'s own caveat).
        fsck_pre = lifecycle.fsck_detail()
        result.observations["fsck_pre_restart"] = {
            k: fsck_pre.get(k) for k in
            ("reachable", "dangling", "unreachable", "pending_gc", "awaiting_gc", "unaccounted",
             "stale_edge", "partial")}

        view_pre = {n.container: _view(n, rows_per_insert) for n in cl.nodes()}

        ctx.log("S42 leg C: restarting both servers (rebuild the ref view from the durable journal)")
        apply_fault(Fault(t_offset=0, target=FaultTarget.BOTH, action=FaultAction.RESTART, duration_s=0))
        healthy = cluster_boot.wait_healthy(cl, timeout_s=int(p["restart_timeout_s"]), log_fn=ctx.log)
        result.add(Verdict.check(
            "cluster comes back after the restart", "both replicas healthy within the timeout",
            f"healthy={healthy}", healthy,
            "" if healthy else "a server did not come back — check for an abort in the container log"))
        if not healthy:
            _common.standard_end(ctx, result, [_TABLE], expect_exception=True)
            return

        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {_TABLE}", timeout=300)
            except Exception as e:
                ctx.log(f"S42: SYNC REPLICA after restart (best-effort): {e}")
        view_post = {n.container: _view(n, rows_per_insert) for n in cl.nodes()}
        ctx.write_json("s42_views.json", {"pre": view_pre, "post": view_post})

        same = view_pre == view_post
        result.add(Verdict.check(
            "the post-restart view (rebuilt from the durable journal) equals the pre-restart view",
            "identical checksum, active part names and per-block row counts on every replica",
            "identical" if same else
            {c: {"checksum": (view_pre[c]["checksum"], view_post[c]["checksum"]),
                 "parts": (len(view_pre[c]["active_parts"]), len(view_post[c]["active_parts"]))}
             for c in view_pre},
            same,
            "" if same else "the live writer cache and the journal-rebuilt view disagree — the "
                            "diverged-ledger-cache class this card exists for"))

        # Acked-vs-lost, block-granular, evaluated on the POST-restart (journal-rebuilt) view: an
        # acked INSERT whose rows are gone is data loss regardless of which side is wrong.
        post_blocks = view_post[node.container]["block_counts"]
        lost = sorted(b for b in acked_blocks if post_blocks.get(b, 0) != rows_per_insert)
        result.add(Verdict.check(
            "every ACKED insert's rows are present after the restart",
            "0 acked blocks missing rows",
            f"acked={len(acked_blocks)} lost={len(lost)} (first: {lost[:10]})",
            not lost,
            "" if not lost else "an acked-then-lost INSERT under allocation faults"))

        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(_TABLE),
                                      name="S42 replica agreement")

        # fsck again after the restart, and compare the structural picture with the pre-restart one.
        fsck_post = lifecycle.fsck_detail()
        result.observations["fsck_post_restart"] = {
            k: fsck_post.get(k) for k in
            ("reachable", "dangling", "unreachable", "pending_gc", "awaiting_gc", "unaccounted",
             "stale_edge", "partial")}

        for label, rep in (("pre-restart", fsck_pre), ("post-restart", fsck_post)):
            if "stale_edge" not in rep:
                result.add(Verdict.inconclusive(
                    f"fsck {label}: stale_edge == 0", "0",
                    "the fsck summary carries no `stale_edge` field — the server binary predates the "
                    "stale-edge class; the run cannot claim the pool is free of permanently "
                    "unreclaimable blobs"))
            else:
                result.add(Verdict.check(
                    f"fsck {label}: stale_edge == 0 (detail mode)", "0", rep.get("stale_edge"),
                    int(rep.get("stale_edge") or 0) == 0,
                    "" if int(rep.get("stale_edge") or 0) == 0 else
                    "blobs whose every source edge names a manifest that no longer exists: their "
                    "in-degree can never reach zero, so the incremental GC will never reclaim them"))
            result.add(Verdict.check(
                f"fsck {label}: dangling == 0 and unaccounted == 0", "0 / 0",
                f"dangling={rep.get('dangling')} unaccounted={rep.get('unaccounted')}",
                int(rep.get("dangling", -1)) == 0 and int(rep.get("unaccounted", -1)) == 0))

        # ---- Poison: the targeted signal ----------------------------------------------------------
        ev_post = _cluster_events(cl)
        result.observations["events_after_restart_absolute"] = ev_post
        # `system.events` is per-process, so the restart resets it: the armed-window delta and the
        # post-restart absolutes are two disjoint measurements, summed rather than differenced.
        poison_armed = int(armed_delta.get("CASRefNeedsRecovery", 0))
        poison_post = _event_total(ev_post, "CASRefNeedsRecovery")
        poison_total = poison_armed + poison_post
        poison_log_lines = _text_log_count(node, since, _POISON_LOG_NEEDLE)
        failpoint_hits, failpoint_why = _post_put_failpoint_hits()

        result.add(Verdict.check(
            "CASRefNeedsRecovery == 0 (no durable transaction lost from a writer cache)",
            "0 poison transitions",
            f"armed_window={poison_armed} post_restart={poison_post} log_lines={poison_log_lines}",
            poison_total == 0,
            "" if poison_total == 0 else
            "an install failed although its ref-log object may already be durable — §A1's "
            "allocation-free install regions did not hold"))

        # ---- Soundness guard (step 5) -------------------------------------------------------------
        generic = int(injected_client_failures[0]) + int(armed_delta.get("QueryMemoryLimitExceeded", 0))
        targeted = poison_total + failpoint_hits
        guard = {"targeted_total": targeted, "poison_transitions": poison_total,
                 "post_put_failpoint_hits": failpoint_hits, "post_put_failpoint_why": failpoint_why,
                 "generic_injected_failures": generic,
                 "client_visible_memory_limit_exceeded": injected_client_failures[0],
                 "QueryMemoryLimitExceeded_delta": int(armed_delta.get("QueryMemoryLimitExceeded", 0))}
        result.observations["soundness_guard"] = guard
        ctx.write_json("s42_soundness_guard.json", guard)

        if generic == 0:
            result.add(Verdict.inconclusive(
                "allocation faults were actually injected (generic anti-vacuity)",
                "> 0 injected allocation failures during the armed window",
                "0 injected failures — the arming path itself did not work (check that the driver "
                "sends memory_tracker_fault_probability AND max_untracked_memory=0 as URL "
                "parameters); nothing about the write path was exercised"))
        else:
            result.add(Verdict.check(
                "allocation faults were actually injected (generic anti-vacuity)",
                "> 0 injected allocation failures during the armed window",
                f"{generic} (client-visible {injected_client_failures[0]})", True,
                "generic only — proves SOME allocation failed, NOT that the post-durable window was "
                "hit. This guard STAYS gating (2026-07-25 decision): a run in which no allocation "
                "fault occurred at all must never read green."))

        if targeted == 0:
            result.add(Verdict.reported(
                "post-durable install window traversal (reported, not gating)",
                "> 0 targeted signals (CASRefNeedsRecovery transitions or post-PUT failpoint hits)",
                f"targeted=0 with {generic} generic allocation failures",
                f"the window was NOT proven traversed: a nonzero MEMORY_LIMIT_EXCEEDED count proves "
                f"only that SOME allocation failed, never that the few-instruction post-durable "
                f"install region was entered while armed. {failpoint_why}. With §A1 landed the region "
                f"allocates nothing, so this counter is EXPECTED to stay 0. Per the 2026-07-25 "
                f"decision this no longer gates: green means the consistency oracle held, not that "
                f"the target window was hit."))
        else:
            result.add(Verdict.reported(
                "post-durable install window traversal (reported, not gating)",
                "> 0 targeted signals (CASRefNeedsRecovery transitions or post-PUT failpoint hits)",
                f"targeted={targeted} (poison={poison_total}, failpoint={failpoint_hits})",
                "the window WAS reached — the poison verdict above is a conclusive statement about "
                "§A1 for this run, not a vacuous zero"))

        # ---- Remaining oracle terms ----------------------------------------------------------------
        # The needle is deliberately narrow. A bare `%Logical error%` also matches the harmless
        # startup line "Sending logical errors is enabled" (Information level) and turns every run
        # red: match the thrown-exception forms at Error level or worse instead.
        logical_error_where = (
            "level <= 'Error' AND (message ILIKE '%LOGICAL_ERROR%' OR message ILIKE '%Logical error:%')")
        logical_errors = 0
        probe_failed = False
        code49 = 0
        samples = []
        for n in cl.nodes():
            try:
                n.command("SYSTEM FLUSH LOGS")
                logical_errors += int(n.scalar(
                    f"SELECT count() FROM system.text_log WHERE event_time >= '{since}' "
                    f"AND {logical_error_where}") or 0)
            except Exception:
                probe_failed = True   # a broken probe must never read as "never happened"
            try:
                code49 += int(n.scalar(
                    "SELECT sum(value) FROM system.errors WHERE name = 'LOGICAL_ERROR'") or 0)
            except Exception:
                pass
            try:
                samples += [r for r in n.query(
                    f"SELECT message FROM system.text_log WHERE event_time >= '{since}' "
                    f"AND {logical_error_where} LIMIT 3 FORMAT TabSeparated").splitlines() if r]
            except Exception:
                pass
        result.observations["logical_error_samples"] = samples[:6]
        if probe_failed:
            result.add(Verdict.inconclusive(
                "zero LOGICAL_ERROR", "0",
                f"the text_log probe failed on at least one node; cannot claim zero "
                f"(counted {logical_errors} on the nodes that answered)"))
        else:
            # `system.errors` is post-restart-only (per-process), so it corroborates rather than
            # replaces the since-scoped text_log count that spans the whole run.
            result.add(Verdict.check(
                "zero LOGICAL_ERROR (queries may fail; invariants may not)", "0",
                f"text_log={logical_errors} system.errors(post-restart)={code49}",
                logical_errors == 0 and code49 == 0,
                "" if logical_errors == 0 and code49 == 0 else f"samples: {samples[:2]}"))

        if other_failures:
            result.observations["unexpected_statement_errors"] = other_failures[:10]
        result.add(Verdict.check(
            "statements failed only with the injected allocation error",
            "0 statement failures of any other kind during the armed window",
            f"{len(other_failures)} other failures", not other_failures,
            "" if not other_failures else
            f"an armed statement failed with something other than the injected "
            f"MEMORY_LIMIT_EXCEEDED: {other_failures[:2]}"))

        # No permanently wedged ref lane: an UNARMED write must land after the restart. Polled, not
        # single-shot — post-restart remount/recovery is asynchronous.
        recovered, last_err = False, ""
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                sql.insert_random(node, _TABLE, rows=rows_per_insert, payload_bytes=payload,
                                  op_id=(next_block[0] + 10) * rows_per_insert, timeout=60)
                recovered = True
                break
            except Exception as e:
                last_err = str(e)
                time.sleep(3)
        result.add(Verdict.check(
            "no permanently wedged ref lane (an unarmed write lands after the run)",
            "an INSERT succeeds within 120s of the restart", f"recovered={recovered}", recovered,
            "" if recovered else f"no write landed after the faults cleared: {last_err[:200]}"))

        # GC recovers after disarm: rounds ran and finished successfully in the post-disarm window.
        gc_after = observe.gc_log_all(cl, disarm_since)
        gc_summary = gc_after.get("summary", {})
        result.observations["gc_after_disarm"] = gc_summary
        succeeded = int(gc_summary.get("success", 0) or 0)
        failed = int(gc_summary.get("failed", 0) or 0)
        rounds_seen = sum(int(v or 0) for k, v in gc_summary.items()
                          if k in ("success", "failed", "failed_benign", "not_a_leader"))
        if rounds_seen == 0:
            # No finish rows at all is NOT "GC recovered" and not "GC broke" — it is a blind probe.
            # (The suite has been burned twice by a stale column list silently emptying this query;
            # see `observe.gc_log_rows`.)
            result.add(Verdict.inconclusive(
                "GC rounds succeed again after disarm", "> 0 successful and 0 failed rounds",
                "no GC finish rows visible after disarm — the GC log probe returned nothing, so "
                "GC recovery cannot be claimed either way"))
        else:
            result.add(Verdict.check(
                "GC rounds succeed again after disarm", "> 0 successful and 0 failed rounds",
                f"success={succeeded} failed={failed} rounds_seen={rounds_seen}",
                succeeded > 0 and failed == 0,
                "" if succeeded > 0 and failed == 0 else
                "GC did not recover after the allocation faults cleared"))

        # Reported, never gating (see the module docstring).
        unmatched = (int(armed_delta.get("CASGCUnmatchedRemoveDeltas", 0))
                     + _event_total(ev_post, "CASGCUnmatchedRemoveDeltas"))
        result.add(Verdict.reported(
            "CASGCUnmatchedRemoveDeltas (reported, not gating)",
            "(recorded; benign rate not yet characterised)",
            f"{unmatched} (armed window {armed_delta.get('CASGCUnmatchedRemoveDeltas', 0)}, "
            f"post-restart {_event_total(ev_post, 'CASGCUnmatchedRemoveDeltas')})",
            "removal deltas that matched no existing source edge; a per-key no-op by design, but a "
            "persistent rate means deltas reach the reducer without their activation"))
        result.add(Verdict.reported(
            "ref-lane wedge counters (reported)", "(recorded)",
            f"wedged={armed_delta.get('CASRefAppendWedged', 0)} "
            f"unwedged={armed_delta.get('CASRefAppendUnwedged', 0)} "
            f"definite_failure={armed_delta.get('CASRefAppendDefiniteFailure', 0)}"))

        _common.standard_end(ctx, result, [_TABLE], expect_exception=True)
