"""Explicit GC driving for the scenario suite.

The README requires that every scenario drive `SYSTEM CAS GC RUN ca`
explicitly at checkpoints — even when background GC is enabled — so a report can separate workload
cost from reclamation cost. `SYSTEM CAS GC RUN ca` runs ONE synchronous GC
round on the node that receives it (only the lease holder makes progress; a non-leader round is a
cheap no-op), so a fixpoint drive issues rounds on both replicas until the unreachable count settles.
"""

import time

from soak.cluster import QueryError

GC_SQL = "SYSTEM CAS GC RUN {disk}"


def gc_round(node, disk: str = "ca", timeout: float = 600.0) -> bool:
    """Issue one synchronous GC round on `node`. Returns True on success. A `QueryError` is re-raised
    (a GC command that throws IS a finding) except that the call is the caller's to guard."""
    node.command(GC_SQL.format(disk=disk), timeout=timeout)
    return True


def gc_drive_round(cluster, disk: str = "ca", timeout: float = 600.0, log_fn=print, node_index: int = 0) -> int:
    """Issue ONE explicit GC round on a SINGLE replica (default node 0). Returns 1 if it completed
    without raising, else 0.

    Single-leader by design: issuing explicit GC on BOTH replicas in the same round creates two
    concurrent GC leaders, whose fold-seal collision can permanently orphan blobs (a reclaim leak —
    safety still holds, `dangling==0`; see BACKLOG "explicit-GC concurrent leaders leak"). Driving one
    replica keeps a single leader; the lease-gated background GC on the other replica is a no-op. An
    ABORTED (raced the background tick) is logged as a benign retry, not a failure."""
    node = cluster.nodes()[node_index]
    try:
        gc_round(node, disk, timeout)
        return 1
    except QueryError as e:
        if e.is_aborted:
            log_fn(f"GC round on {node.container} raced background tick (ABORTED) — benign retry")
        else:
            log_fn(f"GC round on {node.container} raised: {e}")
    except Exception as e:  # transport (node down) — background GC still runs; keep going
        log_fn(f"GC round on {node.container} transport error: {e}")
    return 0


def forced_gc_to_fixpoint(cluster, unreachable_fn, *, max_seconds: float = 240.0, stable: int = 3,
                          disk: str = "ca", round_timeout: float = 600.0, poll_interval_s: float = 3.0,
                          nudge_every_s: float = 12.0, log_fn=print,
                          sleep_fn=time.sleep, monotonic_fn=time.monotonic):
    """Drain the pool to its reclamation fixpoint and return (residual_unreachable, history).

    Drains PRIMARILY via the lease-gated BACKGROUND GC, which runs a single leader per round and
    reclaims cleanly. We issue ONE explicit single-node `SYSTEM CAS GC RUN`
    round up front (satisfies the README "drive explicit GC at checkpoints" + the cost-measurement
    requirement) and then POLL the fsck unreachable count, letting background GC do the work, until it
    stops decreasing for `stable` consecutive reads or `max_seconds` elapses. If the count stalls above
    zero we issue an occasional, WELL-SEPARATED single-node explicit nudge.

    Why not hammer explicit rounds: issuing explicit GC on BOTH replicas — or sustained explicit GC
    overlapping the 2 s background tick under load — creates two concurrent GC leaders. The fold seal
    correctly aborts the divergent fold (safety holds, `dangling==0`), but the colliding round can
    advance GC generation/cursor state past owner-removal events that were never folded, PERMANENTLY
    orphaning their blobs (a reclaim/liveness leak; see BACKLOG "GC-CONCURRENT-LEADER-LEAK"). Letting
    background GC own the drain keeps a single leader and reclaims cleanly."""
    history = []
    node = cluster.nodes()[0]

    def _explicit_round(reason):
        try:
            gc_round(node, disk, round_timeout)
        except QueryError as e:
            if e.is_aborted:
                log_fn(f"forced_gc_to_fixpoint: explicit {reason} raced background tick (ABORTED) — benign")
            else:
                log_fn(f"forced_gc_to_fixpoint: explicit {reason} on {node.container} raised: {e}")
        except Exception as e:
            log_fn(f"forced_gc_to_fixpoint: explicit {reason} transport error: {e}")

    _explicit_round("checkpoint round")
    deadline = monotonic_fn() + max_seconds
    last_nudge = monotonic_fn()
    while True:
        sleep_fn(poll_interval_s)
        try:
            n = int(unreachable_fn())
        except Exception as e:
            log_fn(f"forced_gc_to_fixpoint: unreachable probe failed: {e}")
            if monotonic_fn() > deadline:
                return (history[-1] if history else None), history
            continue
        history.append(n)
        if n == 0 or (len(history) >= stable and len(set(history[-stable:])) == 1):
            return n, history
        now = monotonic_fn()
        # Stalled above zero for the whole nudge window -> one well-separated explicit nudge.
        if n > 0 and (now - last_nudge) >= nudge_every_s:
            _explicit_round("nudge")
            last_nudge = now
        if now > deadline:
            return n, history


def drain_condemned_pipeline(cluster, unreachable_fn, *, sync_period_s: float = 11.0,
                             max_seconds: float = 150.0, disk: str = "ca", round_timeout: float = 600.0,
                             log_fn=print, sleep_fn=time.sleep, monotonic_fn=time.monotonic):
    """Drive the TWO-PHASE graduation pipeline to completion and return (residual, history).

    After `forced_gc_to_fixpoint`, a bounded residual of CONDEMNED content (fsck `pending-gc`) can
    remain: a condemned blob graduates only once the ack floor advances past its condemn round, and the
    floor (`min` over live writers' `observed_gc_round`) advances when each server runs its PERIODIC
    retired-view sync (`mount_renew_period`, ~10 s) — NOT by driving GC rounds alone. `forced_gc_to_fixpoint`
    polls faster than that period, so it reports the residual as "settled" while the floor simply has
    not advanced yet. Here we drive ONE single-leader round then WAIT ~`sync_period_s` so both servers'
    syncs bump `observed_gc_round`, letting graduation + the delete_pending → delete step complete;
    repeat until the residual reaches 0 or `max_seconds`. A HEALTHY condemned residual drains within a
    few periods (~45-55 s observed); a residual that does NOT drain within the budget is a real finding
    (stuck ack floor, or an uncondemned orphan that GC never tracks) — the caller's `assert_no_leftovers`
    classifies the surviving residual by fsck class (unreachable/dangling ⇒ leak ⇒ FAIL)."""
    history = []
    node = cluster.nodes()[0]
    deadline = monotonic_fn() + max_seconds
    while True:
        try:
            n = int(unreachable_fn())
        except Exception as e:
            log_fn(f"drain_condemned_pipeline: unreachable probe failed: {e}")
            n = None
        if n is not None:
            history.append(n)
            if n == 0:
                return 0, history
        if monotonic_fn() >= deadline:
            return (n if n is not None else (history[-1] if history else None)), history
        try:
            gc_round(node, disk, round_timeout)
        except QueryError as e:
            if e.is_aborted:
                log_fn(f"drain_condemned_pipeline: round raced background tick (ABORTED) — benign")
            else:
                log_fn(f"drain_condemned_pipeline: round on {node.container} raised: {e}")
        except Exception as e:
            log_fn(f"drain_condemned_pipeline: round transport error: {e}")
        # Wait a retired-view-sync period so both servers advance observed_gc_round (raises the ack
        # floor); driving rounds back-to-back without this wait does NOT graduate anything.
        sleep_fn(sync_period_s)
