import time


class CheckpointFailure(Exception):
    pass


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

    NOTE: a "stable" count is NOT a GC fixpoint. End-of-run churn leaves objects in retire-GRACE; the
    unreachable count settles >0 BEFORE grace elapses, so stability would declare a false fixpoint
    (see B138, unreachable=61). The live GC-drive path uses `poll_unreachable_to_zero` instead; this
    helper is retained only for the legacy unit test of the stability predicate."""
    if len(history) <= stable:
        return False
    tail = history[-stable:]
    return len(set(tail)) == 1


def poll_unreachable_to_zero(unreachable_fn, *, timeout_s: float, interval_s: float,
                             sleep_fn=time.sleep, monotonic_fn=time.monotonic) -> int:
    """Poll `unreachable_fn()` (current fsck.unreachable, an int) until it reaches 0, returning 0.

    In-grace debris IS reclaimed once the retire grace passes, so the only correct fixpoint is
    unreachable==0. We target 0 with a grace-aware bound (`timeout_s`) rather than declaring a
    fixpoint on a "stable" non-zero count, which would mask a real non-reclaiming leak AND falsely
    fail on still-in-grace objects (B138).

    `sleep_fn`/`monotonic_fn` are injectable so the loop is pure-testable. Raises `CheckpointFailure`
    with the observed history if unreachable does not reach 0 within `timeout_s` -- a genuine
    non-reclaiming leak.

    Examples: a fake returning [61,61,40,0] -> returns 0; a stuck [61,61,61,...] -> raises after the
    bound."""
    deadline = monotonic_fn() + timeout_s
    history = []
    while True:
        n = unreachable_fn()
        history.append(n)
        if n == 0:
            return 0
        if monotonic_fn() > deadline:
            raise CheckpointFailure(
                f"GC did not reclaim to unreachable==0 within {timeout_s:.0f}s "
                f"(grace-aware bound); unreachable history={history}")
        sleep_fn(interval_s)


def quiesce(cluster, table: str, timeout_s: int = 300):
    """Caller has already paused workers. Drain replication queues + mutations + merges (bounded poll,
    loud failure on timeout), force OPTIMIZE FINAL + MATERIALIZE TTL, re-drain, then return the server
    now() captured AFTER convergence."""
    deadline = time.time() + timeout_s
    # SYSTEM SYNC REPLICA blocks server-side until the replica drains its fetch queue; under a
    # replication backlog this legitimately exceeds the default per-call socket timeout, so give the
    # blocking admin ops a socket timeout aligned with our own quiesce budget rather than letting a
    # raw socket TimeoutError escape.
    for node in cluster.nodes():
        node.command(f"SYSTEM SYNC REPLICA {table}", timeout=timeout_s)

    def drained():
        for node in cluster.nodes():
            if int(node.scalar(f"SELECT count() FROM system.replication_queue WHERE table='{table}'")):
                return False
            if int(node.scalar(f"SELECT count() FROM system.mutations WHERE table='{table}' AND NOT is_done")):
                return False
            if int(node.scalar(f"SELECT count() FROM system.merges WHERE table='{table}'")):
                return False
        return True

    while not drained():
        if time.time() > deadline:
            raise CheckpointFailure("quiescence timeout: queues/mutations/merges did not drain")
        time.sleep(1)
    for node in cluster.nodes():
        node.command(f"OPTIMIZE TABLE {table} FINAL", timeout=timeout_s)
        node.command(f"ALTER TABLE {table} MATERIALIZE TTL", timeout=timeout_s)
    while not drained():
        if time.time() > deadline:
            raise CheckpointFailure("quiescence timeout after OPTIMIZE/MATERIALIZE TTL")
        time.sleep(1)
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


def drive_gc_to_fixpoint(cluster, unreachable_fn, timeout_s: int | None = None):
    """Poll unreachable_fn() (an int: current fsck.unreachable) until it reaches 0 (the only correct
    GC fixpoint -- a "stable" non-zero count is a false fixpoint while objects sit in retire-grace,
    B138). The bound comfortably exceeds the CA-disk retire grace plus several GC intervals, so
    in-grace debris drains before we give up; if unreachable does not reach 0 within the bound a real
    non-reclaiming leak is reported via CheckpointFailure. Returns 0 on success."""
    grace = getattr(cluster, "gc_grace_sec", 5)
    interval = getattr(cluster, "gc_interval_s", 3)
    if timeout_s is None:
        # grace + several GC intervals, with a generous floor.
        timeout_s = max(180, grace * 6 + 30)
    return poll_unreachable_to_zero(unreachable_fn, timeout_s=timeout_s, interval_s=interval + 1)
