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
    [80] -> False (not enough history)."""
    if len(history) <= stable:
        return False
    tail = history[-stable:]
    return len(set(tail)) == 1


def quiesce(cluster, table: str, timeout_s: int = 300):
    """Caller has already paused workers. Drain replication queues + mutations + merges (bounded poll,
    loud failure on timeout), force OPTIMIZE FINAL + MATERIALIZE TTL, re-drain, then return the server
    now() captured AFTER convergence."""
    deadline = time.time() + timeout_s
    for node in cluster.nodes():
        node.command(f"SYSTEM SYNC REPLICA {table}")

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
        node.command(f"OPTIMIZE TABLE {table} FINAL")
        node.command(f"ALTER TABLE {table} MATERIALIZE TTL")
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


def drive_gc_to_fixpoint(cluster, unreachable_fn, timeout_s: int = 180, stable: int = 2):
    """Poll unreachable_fn() (an int: current fsck.unreachable) until it stops changing across rounds.
    Bounded; raises on timeout. Returns the final unreachable count."""
    deadline = time.time() + timeout_s
    interval = getattr(cluster, "gc_interval_s", 3)
    history = []
    while True:
        history.append(unreachable_fn())
        if gc_fixpoint_reached(history, stable=stable):
            return history[-1]
        if time.time() > deadline:
            raise CheckpointFailure(f"GC did not reach a fixpoint: unreachable history={history}")
        time.sleep(interval + 1)
