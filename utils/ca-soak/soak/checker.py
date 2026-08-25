import time

from soak.cluster import QueryError, retry_on_transport, is_transport_error


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
                    f"recovery SYNC REPLICA on {node}: replica recovered from transient readonly/"
                    f"transport issue (chaos recovery) after {elapsed:.0f}s / {readonly_budget_s:.0f}s "
                    f"budget — proceeding"
                )
            return
        except QueryError as e:
            if not e.is_readonly:
                raise
            transient_desc = "transiently readonly (chaos ZK-session recovery, TABLE_IS_READ_ONLY)"
            last_err = e
        except Exception as e:   # noqa: BLE001 -- reclassified below; non-transport re-raises as-is
            # A raw transport-level error (connection reset/refused, socket timeout) can hit right
            # after a chaos fault window closes -- even once `wait_for_healthy`'s /ping check has
            # already passed, a node can still be settling its query-handling connections for a
            # moment (hits hardest after `both restart`/`both pause`, where BOTH replicas are
            # re-establishing state at once). Tolerate it with the SAME bounded budget as the
            # TABLE_IS_READ_ONLY transient above; anything else still propagates immediately.
            if not is_transport_error(e):
                raise
            transient_desc = f"hit a transient transport error ({type(e).__name__}: {e})"
            last_err = e
        remaining = deadline - monotonic_fn()
        backoff = min(backoff_cap_s, backoff_start_s * (2 ** attempt))
        attempt += 1
        log_fn(
            f"recovery SYNC REPLICA on {node}: replica {transient_desc}, retrying "
            f"({remaining:.0f}s/{readonly_budget_s:.0f}s budget remaining, backoff={backoff:.1f}s)"
        )
        if remaining <= 0:
            raise CheckpointFailure(
                f"SYNC REPLICA {table} on {node}: replica stuck ({transient_desc}) for "
                f"{readonly_budget_s:.0f}s (budget exhausted) — replica did NOT recover within the "
                f"expected chaos-recovery window; this IS a real stuck-replica finding, not the "
                f"expected transient chaos window. last error: {last_err}"
            ) from last_err
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

    There is NO core retire-grace throttle (candidates are derived statelessly per round). The ONLY
    pacing knob is the GC interval, so the bound is interval-and-backlog-based, not grace-based.

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


def wait_for_pool_drain(pool_bytes_fn, *, interval_s: float, band_ratio: float = 0.01, stable: int = 3,
                        safety_factor: float = 3.0, floor_s: float = 300.0, cap_s: float = 1800.0,
                        sleep_fn=time.sleep, monotonic_fn=time.monotonic, log_fn=print) -> list:
    """Poll the PHYSICAL CA-pool byte probe (`pool_bytes_fn`, e.g. `soak.pool.pool_size()[1]`, the
    B204 `du -sb`-based probe — see `soak/pool.py`) until the pool has effectively stopped SHRINKING,
    and return the sampled trajectory (a list of `int | None`, oldest first).

    Why this runs as a PRECONDITION before `poll_unreachable_to_stable`: per
    `.superpowers/sdd/task3-soak-diag-report.md` (Q1/Q2), a large pre-checkpoint garbage backlog
    leaves the server's leader GC ACTIVELY grinding down the physical pool for minutes after
    quiesce — `fsck.unreachable` oscillates every ~15-18s (candidates(N) ~= deleted(N+2), the
    report's "lag-2 alternation") for as long as real bytes are still being reclaimed, so 3
    consecutive samples of `unreachable` are structurally near-impossible to match while the drain
    is in flight, REGARDLESS of budget length (the report's diagnosed root cause: the criterion, not
    the product, failed — data was exactly consistent throughout). The physical byte count is a
    monotone, much lower-noise signal of drain progress than the fold-derived `unreachable` count, so
    waiting for IT to flatten first establishes the precondition under which
    `poll_unreachable_to_stable`'s band criterion is measuring genuine residual fold noise, not an
    active drain.

    Stability: the relative drop between EACH consecutive pair of the last `stable` samples is below
    `band_ratio` (default 1%) — i.e. the pool has stopped shrinking meaningfully, not necessarily
    that it is bit-for-bit flat.

    Budget: DERIVED from the OBSERVED drain rate (bytes_drained / elapsed, from the first and latest
    real samples) rather than a fixed constant — `remaining_bytes / rate * safety_factor`, floored at
    `floor_s` (a small/no backlog still gets a reasonable wait) and capped at `cap_s` (a pathological
    rate reading can't produce an unbounded wait). The budget is RECOMPUTED each poll as more samples
    refine the rate estimate, so an unlucky single early sample can't wedge the whole wait the way a
    single early `unreachable_fn()` read can shrink `fixpoint_timeout_s` (report Q6) — this avoids the
    same trap by design.

    Best-effort: `soak.pool.pool_size` returns `(None, None)` on ANY failure by its own contract (it
    must never block the soak). A `None` reading here carries no signal, so we log and return
    immediately rather than wait on an unrelated probe outage — the caller falls through to
    `poll_unreachable_to_stable`, which remains the real, always-authoritative gate.

    If the (rate-scaled) budget expires without the band closing, we log a loud warning and return
    the trajectory so far rather than raise: this precondition is a best-effort accelerant for the
    real gate, not a second hard-failure point — `poll_unreachable_to_stable` still has its own
    band+timeout as the backstop.

    `sleep_fn`/`monotonic_fn` are injectable so the loop is pure-testable."""
    history: list = []
    times: list = []
    start = monotonic_fn()
    timeout_s = floor_s
    while True:
        try:
            b = pool_bytes_fn()
        except Exception:
            b = None
        now = monotonic_fn()
        history.append(b)
        times.append(now)
        log_fn(f"pool drain probe: pool_bytes={b} sample={len(history)} trajectory={history}")

        if b is None:
            log_fn("pool drain probe unavailable (best-effort per soak.pool contract); "
                   "skipping drain-completion wait")
            return history

        # Refine the rate-scaled budget from the first and most-recent REAL samples.
        real = [(t, v) for t, v in zip(times, history) if v is not None]
        if len(real) >= 2:
            (t_first, b_first), (t_last, b_last) = real[0], real[-1]
            dt = t_last - t_first
            drained = b_first - b_last
            if dt > 0 and drained > 0:
                rate = drained / dt  # bytes/sec, observed
                timeout_s = min(cap_s, max(floor_s, (max(0, b_last) / rate) * safety_factor))

        if len(history) >= stable:
            tail = history[-stable:]
            stopped = all(
                prev <= 0 or (prev - cur) / prev < band_ratio
                for prev, cur in zip(tail, tail[1:])
            )
            if stopped:
                log_fn(f"pool drain complete: last {stable} samples {tail} within "
                       f"{band_ratio:.0%} relative-drop band (budget was {timeout_s:.0f}s)")
                return history

        if now - start > timeout_s:
            log_fn(f"WARNING pool drain wait exceeded its rate-scaled budget ({timeout_s:.0f}s); "
                   f"proceeding to the unreachable-fixpoint poll anyway. trajectory={history}")
            return history

        sleep_fn(interval_s)


def unreachable_stable_band(history: list, *, stable: int = 3, band_ratio: float = 0.01,
                            band_floor: float = 50.0) -> bool:
    """True once the LAST `stable` `fsck.unreachable` samples all lie within a BAND of each other,
    rather than requiring bit-for-bit equality.

    Why a band and not equality: per `.superpowers/sdd/task3-soak-diag-report.md` (Q1), the server's
    leader GC keeps running condemn/delete rounds every ~15-18s even once the physical pool has
    finished draining — candidates(N) is consistently ~= deleted(N+2) (a lag-2 alternation), and each
    round's magnitude can swing by thousands. The checker's own summary `fsck` fold costs ~20-25s per
    sample and can itself observe benign in-flight ref-cleanup churn. 3 bit-for-bit-IDENTICAL
    consecutive samples are therefore structurally near-unreachable even once the pool is fully
    quiescent — the report's diagnosed failure was exactly this: a perfectly-consistent, converging
    pool that the equality criterion could never certify. `poll_unreachable_to_stable` only reaches
    this check AFTER `wait_for_pool_drain` has confirmed the PHYSICAL pool has stopped shrinking, so
    the band here tolerates residual fold-level noise, not an active drain.

    band = max(band_floor, band_ratio * max(tail)) — an absolute floor (default 50) so a small
    residual count is not held to an unrealistically tight relative band, plus a relative 1%-of-max
    term so a large residual gets a proportionally wide one.

    Examples (stable=3, band_floor=50, band_ratio=0.01): [2837, 3177, 6203] band=max(50,62)=62,
    spread=3366 -> False (still oscillating — matches the report's real, unconverged history).
    [1622, 1600, 1610] band=max(50,16)=50, spread=22 -> True (settled within noise). [80] -> False
    (not enough history)."""
    if len(history) < stable:
        return False
    tail = history[-stable:]
    band = max(band_floor, band_ratio * max(tail))
    return (max(tail) - min(tail)) <= band


def poll_unreachable_to_stable(unreachable_fn, *, timeout_s: float, interval_s: float, stable: int = 3,
                               band_ratio: float = 0.01, band_floor: float = 50.0,
                               drain_history: list | None = None,
                               sleep_fn=time.sleep, monotonic_fn=time.monotonic) -> int:
    """Poll `unreachable_fn()` (current fsck.unreachable, an int) until the INCREMENTAL GC reaches ITS
    fixpoint — i.e. the count STOPS CHANGING MEANINGFULLY (settles into a BAND, see
    `unreachable_stable_band`) for `stable` consecutive polls — then RETURN the residual unreachable
    count.

    The incremental, journal-driven GC's fixpoint is NOT unreachable==0: per the CA spec §8 it cannot
    reclaim "debris"/"drift" — e.g. blobs orphaned by a tree that is added-and-displaced within one
    fold window, so its child-blob edges are never recorded (the gtest `CASGCLeak.
    DisplacedUnexpandedTreeBlobsLeak` documents this). The Full-GC mark-sweep (milestone M-F, NOT yet
    implemented, tracked as B140) is the documented backstop that drains this residual to 0. So the
    correct fixpoint of the CURRENTLY-IMPLEMENTED GC is the stable residual. This residual is NOT
    data loss: every ref-reachable object still exists (`dangling==0`, INV-NO-LOSS holds).

    Stability criterion (per `.superpowers/sdd/task3-soak-diag-report.md`, which diagnosed a false
    checkpoint failure here): the ORIGINAL criterion required `stable` bit-for-bit IDENTICAL
    consecutive samples, but the server's leader GC legitimately keeps running condemn/delete rounds
    every ~15-18s (candidates(N) ~= deleted(N+2), swinging by up to several thousand per round) for as
    long as a real backlog remains — so 3-in-a-row exact equality was structurally near-unreachable
    until the backlog was FULLY drained, independent of how generous the timeout was. Callers should
    therefore invoke this only AFTER the physical pool has stopped shrinking (`wait_for_pool_drain`),
    and pass its `drain_history` through here purely so a genuine timeout's failure message carries
    the full drain trajectory alongside the `unreachable` history. Stabilization is now "the last
    `stable` samples lie within a band of each other" (`unreachable_stable_band`) — a transient bump
    (a new orphan appearing mid-quiesce) that lands OUTSIDE the band still keeps the window unstable,
    so we only return once the count has truly settled within tolerance.

    `sleep_fn`/`monotonic_fn` are injectable so the loop is pure-testable. Raises `CheckpointFailure`
    ONLY on a true timeout — never reaching ANY stable band within `timeout_s` (the GC is still
    grinding a huge backlog, or genuinely oscillating without settling), which is a harness/bound
    problem, not a correctness one.

    Examples: a fake returning [1751,1200,600,61,61,61] (stable=3) -> returns 61 (exact match is
    still within any band); a fake alternating [4748,0,4748,0,...] forever -> raises after the bound
    (never settles into a band, matching the report's real per-round swing magnitude)."""
    deadline = monotonic_fn() + timeout_s
    history = []
    while True:
        n = unreachable_fn()
        history.append(n)
        if unreachable_stable_band(history, stable=stable, band_ratio=band_ratio, band_floor=band_floor):
            return n
        if monotonic_fn() > deadline:
            drain_note = f" drain_history={drain_history}" if drain_history is not None else ""
            raise CheckpointFailure(
                f"GC unreachable count never stabilized within {timeout_s:.0f}s (backlog-scaled "
                f"bound); it never reached a fixpoint (still grinding?). history={history}{drain_note}")
        sleep_fn(interval_s)


def is_genuine_hang(*, backlog_flat: bool, active_merges: int, errored_queue: int,
                    grace_exceeded: bool, budget_exceeded: bool, absolute_cap_exceeded: bool):
    """Pure decision: is the quiescence drain a GENUINE HANG (vs. slow-but-progressing)?

    Inputs (all booleans/counts derived by the caller from live cluster state):
      backlog_flat          - the total backlog COUNT has not decreased for the grace window.
      active_merges         - number of merges/mutations CURRENTLY EXECUTING in `system.merges`.
      errored_queue         - number of replication-queue entries with a real `last_exception`.
      grace_exceeded        - no-progress grace window has elapsed.
      budget_exceeded       - the soft `timeout_s` budget is exhausted.
      absolute_cap_exceeded - the hard absolute backstop cap is exhausted.

    Returns (is_hang: bool, reason: str). `reason` is one of:
      "errored"   - a queue entry carries a genuine `last_exception`; fail FAST (a real error,
                     distinct from slowness — handled by the caller before calling here too).
      "idle-flat" - backlog flat + grace+budget spent + NOTHING executing: a true stall.
      "capped"    - the hard absolute cap tripped while nothing is executing (wedged-run backstop).
      ""          - not a hang; keep waiting.

    A flat backlog with ≥1 active merge/mutation is NOT a hang: on CA-over-S3 a single large
    merge legitimately runs >10min with the rest of the queue postponed behind it, so the COUNT
    stays flat while real work executes. We only declare a hang when nothing is executing."""
    if errored_queue > 0:
        return True, "errored"
    if active_merges > 0:
        # Work is executing — never a hang, regardless of a flat count. The absolute cap only
        # trips when NOTHING is executing, so a long-but-progressing merge cannot be capped.
        return False, ""
    if backlog_flat and grace_exceeded and budget_exceeded:
        return True, "idle-flat"
    if absolute_cap_exceeded:
        return True, "capped"
    return False, ""


def quiesce(cluster, table: str, timeout_s: int = 300, admin_timeout_s: float | None = None,
            no_progress_grace_s: float = 120.0, absolute_cap_s: float = 1800.0,
            log_fn=print):
    """Caller has already paused workers. Drain replication queues + mutations + merges, force OPTIMIZE
    FINAL + MATERIALIZE TTL, re-drain, then return the server now() captured AFTER convergence.

    Long-run viability: over a LARGE pool a SYNC/OPTIMIZE that is slow-but-PROGRESSING must not be
    tripped by a fixed minute-scale bound. `admin_timeout_s` (defaults to `scaled_admin_timeout_s`
    over the current pool size, generous floor 600s) is the CLIENT socket timeout AND the server-side
    `receive_timeout`/`max_execution_time` for the blocking admin ops (`SYSTEM SYNC REPLICA`,
    `OPTIMIZE ... FINAL`, `MATERIALIZE TTL`), so a slow large-pool op no longer escapes as a spurious
    HTTP-408 `TIMEOUT_EXCEEDED` / raw socket TimeoutError.

    The drain poll is MERGE-AWARE: it distinguishes a GENUINE HANG from slow-but-working. A flat
    backlog COUNT is NOT treated as a hang while real work is actively executing — on CA-over-S3 a
    single large merge legitimately runs >10min (one active merge on a huge high-level part, with a
    `MUTATE_PART`/`MATERIALIZE TTL` mutation postponed behind it as "not disjoint"), so the COUNT
    stays flat while the merge progresses with zero exceptions. The drain treats the system as
    PROGRESSING if there is ≥1 active merge/mutation in `system.merges` OR the backlog count
    decreased since the last poll. It declares a hang ONLY when the backlog is flat AND there are NO
    active merges/mutations AND the grace+budget windows are spent (a true stall: queue stuck with
    nothing executing). A generous `absolute_cap_s` (30min) is a backstop, but it too only trips
    when nothing is executing. A queue entry carrying a real `last_exception` still fails FAST."""
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

    def _scalar_resilient(node, sql):
        """A backlog/merge-activity probe read, tolerant of a TRANSIENT transport error. A node can
        still be settling its connection handling for a moment even after `wait_for_healthy`'s `/ping`
        check has already passed -- especially right after a `both restart`/`both pause` chaos fault,
        where BOTH replicas are re-establishing state at once. Without this, a single connection
        reset during the drain POLL LOOP (not the workload itself) propagated straight out of
        `quiesce()` uncaught, aborting the whole run with zero retries -- unlike every workload op,
        which gets the same bounded transport-retry via `Driver._with_transport_retry`. A node that
        stays genuinely unreachable past this budget still fails loudly (`retry_on_transport`
        re-raises after its attempts are exhausted)."""
        def on_retry(attempt_no, err):
            log_fn(f"quiesce probe on {node} transiently failed (attempt {attempt_no}); "
                   f"retrying: {type(err).__name__}: {err}")
        return retry_on_transport(lambda: node.scalar(sql), attempts=10, on_retry=on_retry)

    def backlog():
        total = 0
        for node in cluster.nodes():
            total += int(_scalar_resilient(node, f"SELECT count() FROM system.replication_queue WHERE table='{table}'"))
            total += int(_scalar_resilient(node, f"SELECT count() FROM system.mutations WHERE table='{table}' AND NOT is_done"))
            total += int(_scalar_resilient(node, f"SELECT count() FROM system.merges WHERE table='{table}'"))
        return total

    def merge_activity():
        """Return (active_merges, max_elapsed_s) across the cluster: how many merges/mutations are
        CURRENTLY EXECUTING in `system.merges`, and the largest elapsed of any of them. ≥1 active
        means real work is in flight (so a flat backlog is slow-but-progressing, not a hang)."""
        active = 0
        max_elapsed = 0.0
        for node in cluster.nodes():
            active += int(_scalar_resilient(node, f"SELECT count() FROM system.merges WHERE table='{table}'"))
            e = _scalar_resilient(node, f"SELECT max(elapsed) FROM system.merges WHERE table='{table}'")
            try:
                max_elapsed = max(max_elapsed, float(e))
            except (TypeError, ValueError):
                pass  # NULL/empty when there are no active merges
        return active, max_elapsed

    def errored_queue():
        """Count replication-queue entries carrying a genuine `last_exception` (a real error,
        distinct from slowness). Any such entry fails the checkpoint FAST."""
        total = 0
        for node in cluster.nodes():
            total += int(_scalar_resilient(node,
                f"SELECT count() FROM system.replication_queue "
                f"WHERE table='{table}' AND last_exception != ''"))
        return total

    def drain(stage_label: str):
        deadline = time.time() + timeout_s
        absolute_deadline = time.time() + absolute_cap_s
        last_backlog = None
        last_progress_t = time.time()
        while True:
            b = backlog()
            if b == 0:
                return
            now = time.time()
            backlog_flat = True
            if last_backlog is None or b < last_backlog:
                # Progress: backlog shrank -> reset the no-progress timer and extend.
                last_backlog = b
                last_progress_t = now
                backlog_flat = False

            errs = errored_queue()
            active, max_elapsed = merge_activity()
            grace_exceeded = (now - last_progress_t) > no_progress_grace_s
            budget_exceeded = now > deadline
            absolute_cap_exceeded = now > absolute_deadline

            is_hang, reason = is_genuine_hang(
                backlog_flat=backlog_flat,
                active_merges=active,
                errored_queue=errs,
                grace_exceeded=grace_exceeded,
                budget_exceeded=budget_exceeded,
                absolute_cap_exceeded=absolute_cap_exceeded,
            )
            if is_hang:
                if reason == "errored":
                    raise CheckpointFailure(
                        f"quiescence {stage_label}: {errs} replication-queue entr(ies) carry a real "
                        f"last_exception — genuine error (failing fast, not slowness)")
                if reason == "capped":
                    raise CheckpointFailure(
                        f"quiescence {stage_label}: backlog stuck at {b} and nothing executing in "
                        f"system.merges — absolute cap of {absolute_cap_s:.0f}s exhausted — wedged run")
                raise CheckpointFailure(
                    f"quiescence {stage_label}: backlog stuck at {b} with NO active merges (no "
                    f"progress for {now - last_progress_t:.0f}s past the {timeout_s}s budget) — "
                    f"genuine hang")
            if backlog_flat and grace_exceeded and budget_exceeded and active > 0:
                # Slow-but-progressing: flat COUNT but real work is executing. Log once per poll
                # that we are extending the wait so the operator can see why we are not failing.
                log_fn(
                    f"quiesce {stage_label}: backlog={b} flat but {active} active merge(s) "
                    f"(max elapsed {max_elapsed:.0f}s) — still progressing, extending wait")
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
                         pool_bytes_fn=None, drain_interval_s: float = 60.0,
                         sleep_fn=time.sleep, monotonic_fn=time.monotonic, log_fn=print) -> int:
    """Wait until the INCREMENTAL GC reaches ITS fixpoint — `fsck.unreachable` STOPS CHANGING
    MEANINGFULLY (settles into a band) for K consecutive polls — and RETURN the residual unreachable
    count.

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
    timeout — never reaching ANY stable band (the GC is still grinding, or genuinely oscillating
    without settling), which is a bound/harness issue, not a correctness one.

    Two-part fix for the false checkpoint failure diagnosed in
    `.superpowers/sdd/task3-soak-diag-report.md` (a real backlog whose per-round condemn/delete
    oscillation made 3-in-a-row EXACT equality of `unreachable` structurally near-unreachable while
    the pool was still actively, correctly draining):

    1. If `pool_bytes_fn` is given (e.g. `lambda: soak.pool.pool_size()[1]`), we first WAIT for the
       PHYSICAL pool to stop shrinking (`wait_for_pool_drain`) before even starting to sample
       `unreachable` for stability — sampling a fold-derived count while the pool is still draining
       is chasing a moving target, independent of how generous the `unreachable`-poll budget is. This
       step is a best-effort accelerant with its own rate-scaled budget; it never raises, and a
       `None` pool-bytes reading (probe outage) or an omitted `pool_bytes_fn` (default `None`, the
       original behavior) both fall straight through to step 2.
    2. `poll_unreachable_to_stable` now accepts a BAND (see `unreachable_stable_band`) instead of
       requiring bit-for-bit equality, tolerating the residual fold-level noise that persists even
       after the physical drain is complete.

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

    drain_history = None
    if pool_bytes_fn is not None:
        drain_history = wait_for_pool_drain(
            pool_bytes_fn, interval_s=drain_interval_s, sleep_fn=sleep_fn, monotonic_fn=monotonic_fn,
            log_fn=log_fn)

    if timeout_s is None:
        timeout_s = fixpoint_timeout_s(initial, gc_interval_s=interval)
    return poll_unreachable_to_stable(unreachable_fn, timeout_s=timeout_s, interval_s=interval + 1,
                                      drain_history=drain_history, sleep_fn=sleep_fn,
                                      monotonic_fn=monotonic_fn)
