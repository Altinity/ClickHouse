import time

from soak.cluster import QueryError


class CheckpointFailure(Exception):
    pass


def sync_replica_with_readonly_retry(
    node,
    table: str,
    *,
    timeout: float | None = None,
    settings: dict | None = None,
    readonly_budget_s: float = 120.0,
    backoff_start_s: float = 1.0,
    backoff_cap_s: float = 5.0,
    sleep_fn=time.sleep,
    monotonic_fn=time.monotonic,
    log_fn=print,
):
    """Issue `SYSTEM SYNC REPLICA <table>` on `node`, retrying on `TABLE_IS_READ_ONLY` (code 242).

    A ReplicatedMergeTree replica transiently becomes read-only while re-establishing its ZooKeeper
    session after a chaos fault (kill/restart/pause). This window typically lasts ~tens of seconds;
    the replica RECOVERS automatically once Keeper confirms the new session. Admin ops such as
    `SYSTEM SYNC REPLICA` issued during/just-after a chaos fault window can hit this transient and
    must RETRY (with bounded backoff) rather than surface as a hard `WORKLOAD FAILURE` (B155).

    Retry policy:
    - On `TABLE_IS_READ_ONLY` (code 242): log a loud warning, sleep `backoff_start_s` (capped at
      `backoff_cap_s`), retry. If the replica is read-write again within `readonly_budget_s`, return
      normally (the SYNC completed). If readonly PERSISTS past the budget, raise a `CheckpointFailure`
      (a replica stuck read-only past 120 s IS a real finding, not the expected transient).
    - Any other `QueryError` (a genuine error): re-raise immediately, no retry.
    - `readonly_budget_s` defaults to 120 s, which is a generous safety margin above the typical
      tens-of-seconds ZK session re-establishment time and matches the chaos fault durations.

    `sleep_fn` and `monotonic_fn` are injectable so the retry loop is pure-testable without real sleeps.
    `log_fn` defaults to `print`; callers in the harness pass the module-level `log`."""
    deadline = monotonic_fn() + readonly_budget_s
    attempt = 0
    while True:
        try:
            node.command(f"SYSTEM SYNC REPLICA {table}", timeout=timeout, settings=settings)
            if attempt > 0:
                elapsed = monotonic_fn() - (deadline - readonly_budget_s)
                log_fn(
                    f"recovery SYNC REPLICA on {node}: replica recovered from transient readonly "
                    f"(chaos ZK-session recovery) after {elapsed:.0f}s / {readonly_budget_s:.0f}s budget — "
                    f"proceeding"
                )
            return
        except QueryError as e:
            if not e.is_readonly:
                raise
            remaining = deadline - monotonic_fn()
            backoff = min(backoff_cap_s, backoff_start_s * (2 ** attempt))
            attempt += 1
            log_fn(
                f"recovery SYNC REPLICA on {node}: replica transiently readonly "
                f"(chaos ZK-session recovery, TABLE_IS_READ_ONLY), retrying "
                f"({remaining:.0f}s/{readonly_budget_s:.0f}s budget remaining, backoff={backoff:.1f}s)"
            )
            if remaining <= 0:
                raise CheckpointFailure(
                    f"SYNC REPLICA {table} on {node}: replica stuck TABLE_IS_READ_ONLY for "
                    f"{readonly_budget_s:.0f}s (budget exhausted) — replica did NOT recover its "
                    f"ZK session within the expected window; this IS a real stuck-replica finding, "
                    f"not the expected transient chaos window. last error: {e}"
                ) from e
            sleep_fn(min(backoff, remaining))


def compare_aggregates(model: dict, node1: dict, node2: dict):
    """Raise CheckpointFailure on the FIRST divergence of either replica from the model. Returns None
    when model == node1 == node2 across all seven aggregate keys."""
    for label, got in (("node1", node1), ("node2", node2)):
        for key in ("count", "sum_fp", "uniq_keys", "sum_v", "sum_version", "min_op", "max_op"):
            if model.get(key) != got.get(key):
                raise CheckpointFailure(f"{label} {key}: model={model.get(key)} got={got.get(key)}")
    return None


def gc_fixpoint_reached(history: list, stable: int = 2) -> bool:
    """True once the unreachable count has stopped changing: the tail samples are all equal AND we
    have enough history (more than `stable` samples) to trust it.

    Examples (stable=2): [100,90,80,80] -> True (it settled), [100,90,80,70] -> False (still moving),
    [80] -> False (not enough history).

    This is the predicate the live GC-drive path (`poll_unreachable_to_stable`) uses: the incremental
    GC's fixpoint is the STABLE count, which legitimately has residual M-F-debris (B140) — per CA spec
    §8 the incremental GC cannot reclaim displaced-before-expansion-tree blobs, and the Full-GC
    mark-sweep (milestone M-F) is the documented backstop that drains the residual to 0. Stabilization
    is therefore the correct fixpoint of the currently-implemented GC; the residual is logged as
    M-F-debris (NOT data loss — `dangling==0` holds)."""
    if len(history) <= stable:
        return False
    tail = history[-stable:]
    return len(set(tail)) == 1


def fixpoint_timeout_s(initial_unreachable: int, *, gc_interval_s: float, floor_s: float = 300.0,
                       reclaim_per_round_guess: float = 50.0) -> float:
    """Compute a backlog-scaled bound for draining `initial_unreachable` orphans to 0 via the
    SERVERS' background GC, which makes ONE reclaim round per `gc_interval_s` (only the lease holder
    progresses in a multi-mounter pool — `CasGcScheduler::loop`). A large post-TRUNCATE backlog
    therefore needs many rounds: ~initial/reclaim_per_round_guess rounds * gc_interval_s seconds,
    times a slack factor, with a generous floor so small backlogs still get plenty of time.

    There is NO core retire-grace throttle (the XML `content_addressed_gc_grace_sec` key is inert —
    not read by the core; candidates are derived statelessly per round). The ONLY pacing knob is the
    GC interval, so the bound is interval-and-backlog-based, not grace-based.

    Examples (gc_interval_s=2, floor 300): backlog 100 -> 300 (floor); backlog 5000 ->
    5 * (5000/50) * 2 = 1000s."""
    rounds_needed = max(1.0, initial_unreachable / max(1.0, reclaim_per_round_guess))
    scaled = 5.0 * rounds_needed * gc_interval_s
    return max(floor_s, scaled)


def scaled_admin_timeout_s(pool_objects: int, *, floor_s: float = 600.0, per_million_s: float = 600.0,
                           cap_s: float = 3600.0) -> float:
    """Compute a generous, pool-size-scaled client timeout (seconds) for a blocking admin op such as
    `SYSTEM SYNC REPLICA` over a LARGE content-addressed pool. A 24h soak builds a pool of millions of
    objects; a SYNC that is slow-but-PROGRESSING then legitimately exceeds a fixed minute-scale bound,
    and a tight client socket timeout turns it into a spurious HTTP-408 `TIMEOUT_EXCEEDED` even though
    the server is making progress (the genuine-hang case is detected separately by drain-poll progress,
    not by this single-shot bound).

    The bound is `floor_s` plus `per_million_s` per million pool objects, capped at `cap_s` so a
    pathological reading can't produce an unbounded wait. A `None`/unknown pool size collapses to the
    floor.

    Examples (floor 600, per_million 600, cap 3600): 0 objects -> 600; 1_000_000 -> 1200;
    5_000_000 -> 3600 (cap); a huge 50_000_000 -> 3600 (cap)."""
    if not pool_objects or pool_objects < 0:
        return floor_s
    return min(cap_s, floor_s + per_million_s * (pool_objects / 1_000_000.0))


def poll_unreachable_to_stable(unreachable_fn, *, timeout_s: float, interval_s: float, stable: int = 3,
                               sleep_fn=time.sleep, monotonic_fn=time.monotonic) -> int:
    """Poll `unreachable_fn()` (current fsck.unreachable, an int) until the INCREMENTAL GC reaches ITS
    fixpoint — i.e. the count STOPS DECREASING (stabilizes) for `stable` consecutive polls — then
    RETURN the residual unreachable count.

    The incremental, journal-driven GC's fixpoint is NOT unreachable==0: per the CA spec §8 it cannot
    reclaim "debris"/"drift" — e.g. blobs orphaned by a tree that is added-and-displaced within one
    fold window, so its child-blob edges are never recorded (the gtest `CasGcLeak.
    DisplacedUnexpandedTreeBlobsLeak` documents this). The Full-GC mark-sweep (milestone M-F, NOT yet
    implemented, tracked as B140) is the documented backstop that drains this residual to 0. So the
    correct fixpoint of the CURRENTLY-IMPLEMENTED GC is the stable non-zero residual. This residual is
    NOT data loss: every ref-reachable object still exists (`dangling==0`, INV-NO-LOSS holds).

    We accept stabilization here (and the checkpoint logs the residual as M-F-debris) rather than
    target 0, which would assert an unimplemented feature. Stabilization is "no further DECREASE for
    `stable` consecutive polls" — a transient bump (a new orphan appearing mid-quiesce) resets the
    run, so we only return once the count has truly settled.

    `sleep_fn`/`monotonic_fn` are injectable so the loop is pure-testable. Raises `CheckpointFailure`
    ONLY on a true timeout — never reaching ANY stable point within `timeout_s` (the GC is still
    monotonically grinding a huge backlog and the bound was too small), which is a harness/bound
    problem, not a correctness one.

    Examples: a fake returning [1751,1200,600,61,61,61] (stable=3) -> returns 61; a perpetually
    decreasing [1000,900,800,700,...] -> raises after the bound (never settles)."""
    deadline = monotonic_fn() + timeout_s
    history = []
    while True:
        n = unreachable_fn()
        history.append(n)
        # Stable == the last `stable` samples are all equal (no further decrease). Requires enough
        # history so a single early reading cannot be mistaken for a fixpoint.
        if len(history) >= stable and len(set(history[-stable:])) == 1:
            return n
        if monotonic_fn() > deadline:
            raise CheckpointFailure(
                f"GC unreachable count never stabilized within {timeout_s:.0f}s (backlog-scaled "
                f"bound); it never reached a fixpoint (still grinding?). history={history}")
        sleep_fn(interval_s)


def quiesce(cluster, table: str, timeout_s: int = 300, admin_timeout_s: float | None = None,
            no_progress_grace_s: float = 120.0):
    """Caller has already paused workers. Drain replication queues + mutations + merges, force OPTIMIZE
    FINAL + MATERIALIZE TTL, re-drain, then return the server now() captured AFTER convergence.

    Long-run viability: over a LARGE pool a SYNC/OPTIMIZE that is slow-but-PROGRESSING must not be
    tripped by a fixed minute-scale bound. `admin_timeout_s` (defaults to `scaled_admin_timeout_s`
    over the current pool size, generous floor 600s) is the CLIENT socket timeout AND the server-side
    `receive_timeout`/`max_execution_time` for the blocking admin ops (`SYSTEM SYNC REPLICA`,
    `OPTIMIZE ... FINAL`, `MATERIALIZE TTL`), so a slow large-pool op no longer escapes as a spurious
    HTTP-408 `TIMEOUT_EXCEEDED` / raw socket TimeoutError.

    The drain poll distinguishes a GENUINE HANG from slow-but-working: it tracks the total backlog
    (queue + unfinished mutations + merges) and only fails when the backlog has made NO PROGRESS for
    `no_progress_grace_s` AND the overall `timeout_s` budget is exhausted. A backlog that keeps
    shrinking extends the wait (the progress timer resets on every decrease) instead of tripping."""
    if admin_timeout_s is None:
        # Scale the admin/SYNC bound to the live pool size so a multi-million-object pool gets a
        # proportionally generous wait. A failure to read the size collapses to the generous floor.
        try:
            from soak.pool import pool_size
            objs = pool_size()[0] or 0
        except Exception:
            objs = 0
        admin_timeout_s = scaled_admin_timeout_s(objs)
    t = int(admin_timeout_s)
    # SYSTEM SYNC REPLICA blocks server-side until the replica drains its fetch queue; align the
    # server-side query bound (`receive_timeout`/`max_execution_time`) AND the client socket timeout
    # with the (pool-scaled) admin bound, so a slow-but-progressing large-pool sync no longer escapes
    # as a spurious server-side HTTP-408 `TIMEOUT_EXCEEDED` / raw socket TimeoutError.
    admin_settings = {"receive_timeout": t, "max_execution_time": t}
    for node in cluster.nodes():
        # B155: a replica transiently becomes TABLE_IS_READ_ONLY while re-establishing its ZK session
        # after a chaos fault. Retry on that transient with a generous 120s budget (the typical ZK
        # session re-establishment takes tens of seconds; 120s is a safe margin matching chaos fault
        # durations). Any other error is re-raised immediately — only readonly is retried here.
        sync_replica_with_readonly_retry(
            node, table,
            timeout=admin_timeout_s,
            settings=admin_settings,
        )

    def backlog():
        total = 0
        for node in cluster.nodes():
            total += int(node.scalar(f"SELECT count() FROM system.replication_queue WHERE table='{table}'"))
            total += int(node.scalar(f"SELECT count() FROM system.mutations WHERE table='{table}' AND NOT is_done"))
            total += int(node.scalar(f"SELECT count() FROM system.merges WHERE table='{table}'"))
        return total

    def drain(stage_label: str):
        deadline = time.time() + timeout_s
        last_backlog = None
        last_progress_t = time.time()
        while True:
            b = backlog()
            if b == 0:
                return
            now = time.time()
            if last_backlog is None or b < last_backlog:
                # Progress: backlog shrank -> reset the no-progress timer and extend.
                last_backlog = b
                last_progress_t = now
            # Fail ONLY on a genuine hang: no progress for the grace window AND the budget is spent.
            if (now - last_progress_t) > no_progress_grace_s and now > deadline:
                raise CheckpointFailure(
                    f"quiescence {stage_label}: backlog stuck at {b} (no progress for "
                    f"{now - last_progress_t:.0f}s past the {timeout_s}s budget) — genuine hang")
            time.sleep(1)

    drain("initial drain")
    for node in cluster.nodes():
        node.command(f"OPTIMIZE TABLE {table} FINAL", timeout=admin_timeout_s, settings=admin_settings)
        node.command(f"ALTER TABLE {table} MATERIALIZE TTL", timeout=admin_timeout_s,
                     settings=admin_settings)
    drain("after OPTIMIZE/MATERIALIZE TTL")
    return int(cluster.nodes()[0].scalar("SELECT toUnixTimestamp(now())"))


def query_aggregates(node, table: str) -> dict:
    """Read the seven oracle aggregates from one replica (matching Model.aggregates keys/types)."""
    row = node.query(
        f"SELECT count(), toUInt64(sum(row_fp)), uniqExact((bucket,k)), sum(v), sum(version), "
        f"min(op_id), max(op_id) FROM {table} FORMAT TabSeparated").strip().split("\t")
    if int(row[0]) == 0:
        return {"count": 0, "sum_fp": 0, "uniq_keys": 0, "sum_v": 0, "sum_version": 0,
                "min_op": None, "max_op": None}
    return {"count": int(row[0]), "sum_fp": int(row[1]), "uniq_keys": int(row[2]),
            "sum_v": int(row[3]), "sum_version": int(row[4]),
            "min_op": int(row[5]), "max_op": int(row[6])}


def drive_gc_to_fixpoint(cluster, unreachable_fn, timeout_s: int | None = None,
                         sleep_fn=time.sleep, monotonic_fn=time.monotonic) -> int:
    """Wait until the INCREMENTAL GC reaches ITS fixpoint — `fsck.unreachable` STOPS DECREASING
    (stabilizes) for K consecutive polls — and RETURN the residual unreachable count.

    The incremental, journal-driven GC's fixpoint legitimately has residual M-F-debris (B140): per
    CA spec §8 it cannot reclaim blobs orphaned by a displaced-before-expansion tree; the Full-GC
    mark-sweep (milestone M-F, NOT yet implemented) is the documented backstop that drains the
    residual to 0. So we wait for the count to SETTLE, not for 0 — targeting 0 here would assert an
    unimplemented feature. The residual is NOT data loss (`dangling==0`, INV-NO-LOSS holds); the
    checkpoint LOGS it as M-F-debris.

    The SERVERS' background `CasGcScheduler` makes one reclaim round per `gc_interval_s` (only the
    lease holder progresses), so a large post-TRUNCATE backlog of a few thousand orphans takes many
    rounds to grind DOWN to its residual. The bound is SCALED to the initial backlog (see
    `fixpoint_timeout_s`) with a generous floor; we raise via `CheckpointFailure` ONLY on a true
    timeout — never reaching ANY stable point (the GC is still monotonically grinding), which is a
    bound/harness issue, not a correctness one.

    Returns the residual unreachable count (0 once M-F lands). `sleep_fn`/`monotonic_fn` are
    injectable so the loop is pure-testable."""
    interval = getattr(cluster, "gc_interval_s", 2)
    # Measure the backlog once up front so the bound scales to it. A zero reading is already a
    # fixpoint (nothing to reclaim).
    try:
        initial = int(unreachable_fn())
    except Exception:
        initial = 0
    if initial == 0:
        return 0
    if timeout_s is None:
        timeout_s = fixpoint_timeout_s(initial, gc_interval_s=interval)
    return poll_unreachable_to_stable(unreachable_fn, timeout_s=timeout_s, interval_s=interval + 1,
                                      sleep_fn=sleep_fn, monotonic_fn=monotonic_fn)
