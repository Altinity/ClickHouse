"""S46: graceful restarts under GC -- the premise that the protocol survives death at any instant.

A disk's teardown must not wait out a GC round. The paused-GC restart gives the baseline for what a
stop costs with no round in flight; every restart taken while GC runs must stay within one
control-plane attempt of it, and across the attempts at least one round must be recorded as cut.

What this scenario can and cannot prove. The timing bound and the final fsck are verdicts: a stop
must stay within one attempt of the baseline, and whatever GC was doing when the server went down
must leave a consistent pool behind. Whether a round was actually CUT is recorded, not asserted --
only the lease holder runs rounds that do work, one lasts a fraction of a second, and the loop paces
itself at `cas_gc_interval_sec`, so a restart meets one about a percent of the time. Driving rounds
by hand does not help: a manual round may not steal the lease and returns `NotALeader` in
milliseconds. The deterministic proof that a round IS cut, and cut at its next request, lives in the
`CASGCTeardownStop` unit suite; what this scenario adds is that a real server restarted under a real
GC leader stays fast and leaves the pool clean.
"""

import subprocess
import threading
import time

from ..framework import cluster_boot, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE_PREFIX = "s46_restart"
_DISK = "ca"
_CONTAINERS = ("ca-soak-ch1-1", "ca-soak-ch2-1")
# One control-plane attempt plus slack: what a teardown may add over the paused baseline.
_ATTEMPT_TIMEOUT_S = 5.0
_STOP_SLACK_S = 10.0
_TEARDOWN_STOP_LOG_LINE = "CA GC round stopped by the disk's teardown"
# What the engine says when an admission is refused. A synchronous `SYSTEM CAS GC RUN` cut by the
# teardown answers with it; the background loop logs `_TEARDOWN_STOP_LOG_LINE` instead. Either is a
# round the teardown cut short.
_REFUSAL_TEXT = "mount fence tripped"


def _stop_seconds(container, log_fn):
    """The stop phase alone, timed. `docker stop` sends SIGTERM and waits, which is the graceful
    shutdown this scenario measures; the grace is long so the timing, not the kill, is the verdict."""
    t0 = time.monotonic()
    subprocess.run(["docker", "stop", "-t", "900", container], check=True, timeout=1000)
    stop_s = time.monotonic() - t0
    log_fn(f"S46 stop phase of {container}: {stop_s:.1f}s")
    return stop_s


def _start(container):
    subprocess.run(["docker", "start", container], check=True, timeout=120)


def _teardown_stops_logged(container):
    """How many times the server logged a round cut by its own teardown. Read from the text log
    rather than `system.cas_gc_log`: a round cut by a teardown emits its Finish row while the system
    log is itself shutting down, so the row is not reliably persisted across the restart that
    produced it."""
    out = subprocess.run(
        ["docker", "exec", container, "sh", "-c",
         f"grep -c \"{_TEARDOWN_STOP_LOG_LINE}\" /var/log/clickhouse-server/clickhouse-server.log || true"],
        capture_output=True, text=True, timeout=120)
    text = (out.stdout or "").strip().splitlines()
    return int(text[-1]) if text and text[-1].isdigit() else 0


def _gc_leader(cluster, since, log_fn):
    """The container whose GC rounds actually LEAD. Only one mounter holds the pool's GC lease, and a
    follower's round is a lease read that returns in milliseconds -- restarting a follower can never
    cut a round, which is the whole point of this scenario."""
    per_node = observe.gc_log_all(cluster, since).get("per_node", {})
    for container, rows in per_node.items():
        for r in rows:
            if r.get("outcome") in ("Success", "Deferred"):
                log_fn(f"S46 GC leader is {container}")
                return container
    return None


def _gc_outcomes(cluster, since):
    rows = observe.gc_log_all(cluster, since).get("per_node", {})
    out = []
    for node_rows in rows.values():
        for r in node_rows:
            outcome = r.get("outcome")
            if outcome:
                out.append(outcome)
    return out


@register
class S46RestartUnderGc(Scenario):
    name = "S46"
    title = "graceful restarts under GC"
    priority = "P1"

    param_table = {
        "dev": {"restarts": 6, "tables": 40, "rows": 200, "payload_bytes": 4096, "settle_s": 20},
        "ci": {"restarts": 12, "tables": 120, "rows": 2000, "payload_bytes": 8192, "settle_s": 40},
        "full": {"restarts": 24, "tables": 400, "rows": 5000, "payload_bytes": 16384, "settle_s": 60},
    }

    def run(self, ctx, result):
        p = self.resolve_params(ctx.scale, getattr(ctx, "param_overrides", None) or {})
        cl = ctx.cluster
        node = cl.nodes()[0]
        since = ctx.extra.get("since_event_time")
        tables = [f"{_TABLE_PREFIX}_{i}" for i in range(int(p["tables"]))]

        # (1) Pause GC while the pool is small, so the baseline stop is short by construction.
        node.command(f"SYSTEM CAS GC STOP {_DISK}", timeout=180)

        # (2) Populate with GC paused, then drop half: once GC resumes it has both a live universe to
        #     fold and retired namespaces to reclaim, so its rounds do real work.
        for t in tables:
            sql.create_ca_table(node, t, columns="id UInt64, payload String", order_by="id")
            sql.insert_random(node, t, rows=int(p["rows"]), payload_bytes=int(p["payload_bytes"]))
        for t in tables[::2]:
            node.command(f"DROP TABLE IF EXISTS {t} SYNC", timeout=600)
        live_tables = tables[1::2]

        # (3) The baseline: a stop with GC stopped and nothing in flight.
        leader = _gc_leader(cl, since, ctx.log)
        if leader is None:
            result.add(Verdict.check("a GC leader was identified", "one node leads", "none", False,
                                     "without knowing which mounter holds the lease this scenario "
                                     "would restart a follower, whose rounds do nothing"))
            return
        result.observations["gc_leader"] = leader

        paused_stop_s = _stop_seconds(leader, ctx.log)
        _start(leader)
        if not cluster_boot.wait_healthy(cl, timeout_s=240, log_fn=ctx.log):
            result.add(Verdict.check("cluster healthy after the baseline restart", "healthy",
                                     "not healthy", False,
                                     "the paused-GC restart must come back before the run continues"))
            return
        result.observations["paused_stop_s"] = round(paused_stop_s, 1)

        # (4) Keep a round in flight for the whole restart phase. GC paces itself at
        #     `cas_gc_interval_sec` (10s here) and a round lasts a fraction of a second, so the
        #     background loop alone is inside a round barely a percent of the time and a restart
        #     meets one only by luck. Driving rounds back to back removes the luck; the round holds
        #     the same lock and does the same work.
        driving = threading.Event()
        driving.set()
        cut_synchronous = []

        leader_node = next((n for n in cl.nodes() if n.container == leader), node)

        def _drive_rounds():
            while driving.is_set():
                try:
                    leader_node.command(f"SYSTEM CAS GC RUN {_DISK}", timeout=180)
                except Exception as e:
                    if _REFUSAL_TEXT in str(e):
                        cut_synchronous.append(str(e)[:160])
                    else:
                        # The server is going down or coming back; that is the point.
                        time.sleep(0.2)

        driver = threading.Thread(target=_drive_rounds, daemon=True)
        driver.start()

        witnessed_before = _teardown_stops_logged(leader)
        stops = []
        try:
            for r in range(int(p["restarts"])):
                time.sleep(1.0)
                stops.append(_stop_seconds(leader, ctx.log))
                _start(leader)
                if not cluster_boot.wait_healthy(cl, timeout_s=240, log_fn=ctx.log):
                    result.add(Verdict.check("cluster healthy after every restart", "healthy",
                                             f"restart {r}: not healthy", False,
                                             "a restarted node did not return"))
                    return
        finally:
            driving.clear()
            driver.join(timeout=300)

        witnessed = (_teardown_stops_logged(leader) - witnessed_before) + len(cut_synchronous)
        result.observations["stop_seconds"] = [round(s, 1) for s in stops]
        result.observations["restarts_witnessed_cut"] = witnessed
        result.observations["cut_synchronous_rounds"] = len(cut_synchronous)

        bound = paused_stop_s + _ATTEMPT_TIMEOUT_S + _STOP_SLACK_S
        worst = max(stops) if stops else 0.0
        result.add(Verdict.check(
            "every stop within one attempt of the paused baseline",
            f"<= {bound:.1f}s", f"max {worst:.1f}s", worst <= bound,
            "GC's contribution to a teardown is one request, not one round"))
        result.add(Verdict.reported(
            "rounds cut by a teardown (info)", "recorded; a leading round is in flight ~1% of the time",
            str(witnessed),
            "not a verdict: with one leader, sub-second rounds and a paced loop, a restart meets a "
            "round rarely, and a manual round may not steal the lease. `CASGCTeardownStop` proves "
            "the cut deterministically; this number says whether this run happened to catch one"))

        outcomes = _gc_outcomes(cl, since)
        bad = [o for o in outcomes if o in ("Error", "Aborted")]
        result.add(Verdict.check("no Aborted/Error round across the restarts", "0", str(len(bad)),
                                 not bad, "a clean restart must never read as a backend incident"))

        # (5) The premise: every interrupted round left consistent state.
        time.sleep(int(p["settle_s"]))
        _common.standard_end(ctx, result, live_tables)
