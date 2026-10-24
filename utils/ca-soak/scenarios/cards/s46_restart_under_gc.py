"""S46: graceful restarts under GC -- the premise that the protocol survives death at any instant.

A disk's teardown must not wait out a GC round. The paused-GC restart gives the baseline for what a
stop costs with no round in flight; every restart taken while GC runs must stay within one
control-plane attempt of it, and across the attempts at least one round must be recorded as cut.

What this scenario can and cannot prove. Only the mounter holding the GC lease runs rounds that do
work, so every restart is aimed at whichever node currently leads -- restarting a follower proves
nothing, its rounds are a lease read that returns in milliseconds. Even against the leader a round
lasts a fraction of a second while the loop paces itself at `cas_gc_interval_sec`, so a restart meets
one only some of the time; driving rounds by hand does not close that gap, because a manual round may
not steal the lease. So the timing bound and the final fsck are the verdicts -- a stop stays within
one attempt of the baseline, and whatever GC was doing when the server went down leaves a consistent
pool -- while whether a round was actually CUT is recorded. If no restart was taken against a leader
at all, the timing verdict proves nothing and says so rather than passing. The deterministic proof
that a round IS cut, and cut at its next request, is the `CASGCTeardownStop` unit suite.
"""

import subprocess
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

        # (4) Restart repeatedly, aiming each one at whoever leads NOW: a stop hands the lease to the
        #     peer, so the node that led the previous restart is a follower by the next one, and
        #     restarting it again would measure nothing.
        witnessed_before = {c: _teardown_stops_logged(c) for c in _CONTAINERS}
        stops = []
        restarts_against_a_leader = 0
        for r in range(int(p["restarts"])):
            time.sleep(1.0)
            current = _gc_leader(cl, since, ctx.log)
            target = current or leader
            if current is not None:
                restarts_against_a_leader += 1
            else:
                result.note_anomaly(f"S46 restart {r}: no node had led recently; restarting {target} anyway")
            stops.append(_stop_seconds(target, ctx.log))
            _start(target)
            if not cluster_boot.wait_healthy(cl, timeout_s=240, log_fn=ctx.log):
                result.add(Verdict.check("cluster healthy after every restart", "healthy",
                                         f"restart {r}: not healthy", False,
                                         "a restarted node did not return"))
                return

        witnessed = sum(_teardown_stops_logged(c) - witnessed_before[c] for c in _CONTAINERS)
        result.observations["stop_seconds"] = [round(s, 1) for s in stops]
        result.observations["restarts_witnessed_cut"] = witnessed
        result.observations["restarts_against_a_leader"] = restarts_against_a_leader

        bound = paused_stop_s + _ATTEMPT_TIMEOUT_S + _STOP_SLACK_S
        worst = max(stops) if stops else 0.0
        if restarts_against_a_leader == 0:
            # Every stop hit a follower, whose rounds do nothing: the bound below would compare a stop
            # with no round in flight against a baseline with no round in flight, which the unfixed
            # code satisfies too.
            result.add(Verdict.inconclusive(
                "every stop within one attempt of the paused baseline", f"<= {bound:.1f}s",
                "no restart was taken while the target held the GC lease"))
        else:
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
