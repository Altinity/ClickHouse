"""S39: mount-lease resilience under a degraded-but-alive S3 (fix #37 regression, P1).

Closes the chaos-coverage gap the #37 post-mortem identified: prior soak chaos only faulted
*nodes* (kill/restart), never a degraded-but-alive object store. Runs on the SAME compose S22
already proved out (`docker-compose-s3faultproxy.yml`: an HTTP proxy sits between ClickHouse and
RustFS, `ca` endpoint -> `s3proxy:11121`, faults armed/disarmed via the control port at
`localhost:8474`) -- `compose_variant = "s3faultproxy"` needed no new plumbing, it already
generalizes (confirmed by reading `S22`, which uses the identical mechanism and is NOT
`needs_infra` despite this file's own stale top-of-module docstring saying otherwise).

Two legs against the mount lease's `mount_lease_ttl_ms` (compiled default 30000ms; not currently
overridable via this compose's `storage_conf_faultproxy_*.xml`, so both legs size their fault
windows off that fixed constant rather than a scenario param):

- SHORT fault (`short_fault_s` < 30s): PUT/POST faulted at rate=1.0 for a window shorter than the
  lease TTL, with the background mount-lease renewer beating every `mount_renew_period_ms` (10s
  default) straight into the fault. Asserts fix #37 phase 1 directly via the exact log lines
  `SingleWriterSlot::backgroundLoop` emits (`CasServerRoot.cpp`): at least one "retrying while the
  lease is still valid" transient-retry line (proves the fault was actually exercised on the
  renewal path, or the whole leg is vacuous) and ZERO "stops advancing" fence-trip lines (the mount
  lease must survive). A post-disarm INSERT must succeed immediately -- nothing was ever fenced.
- LONG fault (`long_fault_s` > 30s + safety margin): same fault, held past the TTL. The fence
  SHOULD trip (correct fail-closed, not a bug) -- asserts at least one "stops advancing" line now
  appears, and that `system.replication_queue` (if anything queued during the outage) shows
  `last_exception` populated / a postponed entry rather than a silent, backoff-free retry loop
  (fix #37 phases 2/3: the old ABORTED mapping was invisible here and defeated
  `ReplicatedMergeTreeQueue`'s backoff). The system must then recover cleanly once the fault
  clears: a final INSERT succeeds and fsck is clean at quiescence.

Dev scale keeps both fault windows short (developer patience); ci/full widen them, still anchored
to the same fixed 30s TTL constant.
"""

import json as _json
import threading
import time
import urllib.request

from ..framework import sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s39_lease"
_CTL = "http://localhost:8474"

# Compiled-in defaults (`CasMountRuntime.h`/`CasPool.h`): not currently exposed as an overridable
# scenario param -- this compose's storage_conf does not set them, so both legs anchor to these
# fixed constants rather than a configurable knob that does not actually exist yet.
_MOUNT_LEASE_TTL_S = 30
_MOUNT_RENEW_PERIOD_S = 10


def _ctl(path, body=None, timeout=10):
    """POST/GET against the fault proxy's control port -- same shape as S22's `_ctl`."""
    url = f"{_CTL}{path}"
    if body is None:
        return _json.loads(urllib.request.urlopen(url, timeout=timeout).read().decode())
    req = urllib.request.Request(url, data=_json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    return _json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode())


def _text_log_count(node, since: str, needle: str) -> int:
    """Count `system.text_log` rows containing `needle` (case-insensitive) at/after `since`.
    Flushes logs first (they buffer in memory) -- mirrors S38's `_text_log_count`. A probe failure
    returns -1 (distinct from a genuine 0) so the caller can treat it as inconclusive rather than a
    false "never happened"."""
    try:
        node.command("SYSTEM FLUSH LOGS")
        v = node.scalar(
            f"SELECT count() FROM system.text_log WHERE event_time >= '{since}' "
            f"AND message ILIKE '%{needle}%'")
        return int(v or 0)
    except Exception:
        return -1


@register
class S39(Scenario):
    name = "S39"
    title = "mount-lease resilience under a degraded-but-alive S3 (fix #37)"
    priority = "P1"
    compose_variant = "s3faultproxy"
    # INVARIANT (every scale row must keep this, see `_MOUNT_LEASE_TTL_S`/`_MOUNT_RENEW_PERIOD_S`
    # above and the leg-A/leg-B asserts below): short_fault_s < _MOUNT_RENEW_PERIOD_S (10) and
    # << _MOUNT_LEASE_TTL_S (30), so the short leg overlaps AT MOST one renewal beat and can never
    # fence; long_fault_s > _MOUNT_LEASE_TTL_S (30) + a safety margin, so the long leg reliably
    # fences. The `ci` row previously set short_fault_s=15 (>= the renew period), which violated
    # this invariant and made leg A's own soundness assert raise.
    param_table = {
        "dev": {"short_fault_s": 8, "long_fault_s": 40, "settle_s": 20, "rows": 2000, "payload_bytes": 512},
        "ci": {"short_fault_s": 9, "long_fault_s": 50, "settle_s": 40, "rows": 20000, "payload_bytes": 1024},
        "full": {"short_fault_s": 20, "long_fault_s": 60, "settle_s": 60, "rows": 100000, "payload_bytes": 2048},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        node = cl.nodes()[0]
        rows = int(p["rows"])
        payload = int(p["payload_bytes"])

        # Disarm first: bring-up must be clean regardless of a prior run's state.
        try:
            hz = _ctl("/healthz")
        except Exception as e:
            result.add(Verdict.inconclusive("fault proxy reachable", "control :8474 up",
                                            f"unreachable: {e}"))
            return
        result.observations["proxy"] = {"healthz": hz}
        _ctl("/config", {"rate": 0.0})

        # Create the ReplicatedMergeTree on EVERY node: a replicated table materializes per-replica
        # (each replica runs its own CREATE against the shared zk path), so creating it only on node1
        # would leave node2 without the table entirely and the end-of-run replica-agreement check would
        # see UNKNOWN_TABLE on node2. All the fault/write legs below still drive node1 (`node`) only --
        # this is a single-writer mount-lease test -- but node2 must exist as a real replica so the
        # standard replica-agreement / fsck end-checks are meaningful.
        for n in cl.nodes():
            sql.create_ca_table(n, _TABLE, columns="id UInt64, payload String", order_by="id", wide=True)
        sql.insert_random(node, _TABLE, rows=rows // 4, payload_bytes=payload, op_id=0)

        # --- Leg A: SHORT fault (< lease TTL) -- the mount lease must survive ---
        since_a = node.scalar("SELECT toString(now())")
        short_s = int(p["short_fault_s"])
        assert short_s < _MOUNT_LEASE_TTL_S, "leg A's fault window must stay under the lease TTL"
        assert short_s < _MOUNT_RENEW_PERIOD_S, (
            "leg A's fault window must be shorter than the renew period so it can overlap AT MOST "
            "one renewal beat -- a window >= the renew period can fault two consecutive beats and "
            "(correctly) near the lease deadline, which is leg B's job, not leg A's")
        # The best-effort write MUST run in a background thread, NOT inline: a blocking insert keeps
        # retrying under the fault for its whole CAS budget (~20s), which would keep the fault armed
        # for insert-duration + short_s -- far past the lease TTL -- and the renewer would then
        # (correctly) fence, defeating the "short fault must NOT fence" assertion. This leg asserts
        # the RENEWER rides out the window, not that the INSERT succeeds; the write is only here to
        # put load on the write path while armed. Decoupling it keeps the armed window EXACTLY
        # short_s, and since short_s < renew_period the window can fault at most one beat -> one
        # transient retry -> no deadline breach -> no fence, by construction.
        errs_a: list[str] = []
        def _bg_write_a():
            try:
                sql.insert_random(node, _TABLE, rows=rows // 4, payload_bytes=payload, op_id=rows,
                                  timeout=short_s + 15)
            except Exception as e:
                errs_a.append(str(e))
        _ctl("/config", {"rate": 1.0, "modes": ["503"], "methods": ["PUT", "POST"], "seed": 39})
        writer_a = threading.Thread(target=_bg_write_a, daemon=True)
        writer_a.start()
        time.sleep(short_s)                     # armed window is EXACTLY short_s, write-independent
        _ctl("/config", {"rate": 0.0})
        writer_a.join(timeout=30)               # reap the background writer (faulted or completed)
        if errs_a:
            ctx.log(f"S39 leg A background INSERT under fault (expected to possibly fail/retry): {errs_a[0]}")
        time.sleep(_MOUNT_RENEW_PERIOD_S / 2)   # let the post-clear renewal beat land

        transient_a = _text_log_count(node, since_a,
                                      "background renewal failed transiently, retrying while the lease is still valid")
        fenced_a = _text_log_count(node, since_a, "background renewal failed, the mount-lease stops advancing")
        result.observations["leg_a"] = {"transient_retry_lines": transient_a, "fence_trip_lines": fenced_a}
        result.add(Verdict.check(
            "leg A (short fault): the renewer actually hit the fault (not vacuous)",
            "> 0 transient-retry log lines", f"{transient_a}",
            transient_a > 0,
            "" if transient_a > 0 else "0 transient-retry lines -- the fault window may not have "
                                       "overlapped a renewal beat; widen short_fault_s or shorten "
                                       "the renew period"))
        result.add(Verdict.check(
            "leg A (short fault): mount lease NEVER fenced",
            "0 fence-trip log lines", f"{fenced_a}", fenced_a == 0,
            "" if fenced_a == 0 else "the mount lease fenced during a SHORT fault -- fix #37 phase 1 regression"))

        # Post-disarm write must succeed immediately: nothing was ever fenced.
        sql.insert_random(node, _TABLE, rows=rows // 4, payload_bytes=payload, op_id=2 * rows)

        # --- Leg B: LONG fault (> lease TTL) -- the fence SHOULD trip, then recover cleanly ---
        since_b = node.scalar("SELECT toString(now())")
        _ctl("/config", {"rate": 1.0, "modes": ["503"], "methods": ["PUT", "POST"], "seed": 40})
        long_s = int(p["long_fault_s"])
        assert long_s > _MOUNT_LEASE_TTL_S, "leg B's fault window must exceed the lease TTL"
        try:
            sql.insert_random(node, _TABLE, rows=rows // 4, payload_bytes=payload, op_id=3 * rows,
                              timeout=min(long_s, 30))
            node.command(f"OPTIMIZE TABLE {_TABLE} FINAL", timeout=min(long_s, 30))
        except Exception as e:
            ctx.log(f"S39 leg B write/merge under sustained fault (expected to fail/retry): {e}")
        time.sleep(long_s)
        _ctl("/config", {"rate": 0.0})

        # Give the queue's backoff + self-remount time to recover.
        time.sleep(int(p["settle_s"]))

        fenced_b = _text_log_count(node, since_b, "background renewal failed, the mount-lease stops advancing")
        result.observations["leg_b"] = {"fence_trip_lines": fenced_b}
        result.add(Verdict.check(
            "leg B (long fault): the mount lease fenced (correct fail-closed)",
            "> 0 fence-trip log lines", f"{fenced_b}", fenced_b > 0,
            "" if fenced_b > 0 else "no fence trip recorded during a fault held past the TTL -- "
                                    "either the fault window was too short or phase 1's retry rode "
                                    "out longer than the lease deadline should have allowed"))

        try:
            queue_rows = node.query(
                f"SELECT num_postponed, num_tries, last_exception FROM system.replication_queue "
                f"WHERE table = '{_TABLE}' ORDER BY num_tries DESC LIMIT 5 FORMAT TabSeparated").strip()
            rows_list = [r.split("\t") for r in queue_rows.splitlines() if r]
        except Exception:
            rows_list = None
        if rows_list is None:
            result.add(Verdict.inconclusive(
                "long fault: replication_queue backoff visibility", "populated or empty",
                "system.replication_queue query failed"))
        elif not rows_list:
            # Nothing queued at all during the outage is a legitimate outcome (no merge happened to
            # collide with the fault window) -- not a failure, just nothing to check here.
            result.observations["leg_b"]["queue_rows_at_check"] = 0
        else:
            any_last_exception_populated = any(r[2].strip() for r in rows_list if len(r) > 2)
            result.observations["leg_b"]["queue_sample"] = rows_list[:5]
            result.add(Verdict.check(
                "long fault: system.replication_queue.last_exception is populated (fixes #37 phases 2/3)",
                "at least one non-empty last_exception among queued/recent entries", "see observations",
                any_last_exception_populated,
                "" if any_last_exception_populated else
                "queue entries exist but last_exception is empty everywhere -- the OLD ABORTED "
                "no-visibility defect may have resurfaced"))

        # Post-recovery: a fresh write must succeed once the fault clears and the self-remount lands.
        # Recovery is ASYNCHRONOUS -- self-remount (~16s per the #37 spec) plus the replication-queue
        # backoff -- and under load it routinely exceeds `settle_s`, so a single bare INSERT here is
        # flaky: it throws the (correct, expected) retry-later NETWORK_ERROR while recovery is still in
        # flight and crashes the leg. POLL instead: retry the write until it lands within a generous
        # budget. A retry-later NETWORK_ERROR means "not recovered yet", NOT a failure; the verdict is
        # the whole point of #37 -- writes RESUME after the fault clears, they are not permanently wedged.
        recovered = False
        last_err = ""
        recover_deadline = time.monotonic() + 90
        while time.monotonic() < recover_deadline:
            try:
                sql.insert_random(node, _TABLE, rows=rows // 4, payload_bytes=payload, op_id=4 * rows,
                                  timeout=30)
                recovered = True
                break
            except Exception as e:
                last_err = str(e)
                time.sleep(3)
        final_count = node.scalar(f"SELECT count() FROM {_TABLE}") if recovered else "0"
        result.add(Verdict.check(
            "post-recovery INSERT succeeds within the recovery budget (writes resume, not wedged)",
            "an INSERT lands within 90s of the fault clearing", f"recovered={recovered} count={final_count}",
            recovered and int(final_count or 0) > 0,
            "" if recovered else f"no write succeeded within 90s after the fault cleared -- self-remount "
                                 f"recovery did not resume writes (last error: {last_err[:200]})"))

        # node2 replicates node1's writes through the same faulted S3; after leg B it may still be
        # fetching. Sync it deterministically before the agreement check so we compare converged state,
        # not a mid-catch-up snapshot (the check itself only polls ~8s, too short after a long fault).
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM SYNC REPLICA {_TABLE}", timeout=120)
            except Exception as e:
                ctx.log(f"S39 SYNC REPLICA on a node before agreement check (best-effort): {e}")
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(_TABLE),
                                      name="S39 replica agreement")
        _common.standard_end(ctx, result, [_TABLE])
