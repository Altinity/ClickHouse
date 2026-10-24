"""Scenario lifecycle helpers: fsck/dryrun wrappers, cluster quiescence, and the end checkpoint.

The end checkpoint implements the README §"Common run contract" quiesced fixpoint: no active inserts
(the scenario has stopped its workload), `SYSTEM SYNC REPLICA`, drained replication queue / mutations
/ merges, then forced GC rounds until the pool reaches a declared fixpoint, then a final detailed
fsck + dry-run for the structural assertions.
"""

import os
import time

from soak import fsck as fsck_mod
from soak.cluster import QueryError
from . import gc as gc_mod

# Env-overridable so a scenario can fsck an ISOLATED compose stack (distinct docker-compose project,
# e.g. S41's `ca-s41`) rather than the default `ca-soak` project. Default is the standard project.
DEFAULT_FSCK_CONTAINER = os.environ.get("CA_SOAK_FSCK_CONTAINER", "ca-soak-ch1-1")


def fsck_container() -> str:
    """Resolve the fsck container AT CALL TIME, not at import.

    `DEFAULT_FSCK_CONTAINER` above is read once when this module is imported, and was then used as a
    DEFAULT ARGUMENT in the functions below — which binds it once more, at definition. A variant that
    publishes different container names therefore could not be honoured by exporting the env var
    later: `reset_cluster` sets `CA_SOAK_FSCK_CONTAINER` for the s41 variant, and every fsck call
    still went to `ca-soak-ch1-1`, which does not exist in the `ca-s41` project. S41 then produced 43
    green verdicts and failed on `no dangling after S41 inserts`, whose fsck summary came back
    "unavailable (timeout or parse failure)" (2026-09-01)."""
    return os.environ.get("CA_SOAK_FSCK_CONTAINER", "ca-soak-ch1-1")
DEFAULT_FSCK_DISK = "ca_ro"


# ---------------------------------------------------------------------------
# fsck / dry-run wrappers
# ---------------------------------------------------------------------------

def fsck_summary(container: str | None = None, disk: str = DEFAULT_FSCK_DISK,
                 timeout_s: float = 600.0) -> dict:
    """Summary fsck (no per-object detail) — cheap enough to poll in the GC fixpoint loop."""
    container = container or fsck_container()
    return fsck_mod.run_fsck(container, disk=disk, detail=False, timeout_s=timeout_s)


def fsck_detail(container: str | None = None, disk: str = DEFAULT_FSCK_DISK,
                timeout_s: float = 900.0) -> dict:
    """Detailed fsck (per-object class rows) — used once at the final checkpoint for the structural
    and dry-run-subset assertions."""
    container = container or fsck_container()
    return fsck_mod.run_fsck(container, disk=disk, detail=True, timeout_s=timeout_s)


def dryrun(container: str | None = None, disk: str = DEFAULT_FSCK_DISK,
           timeout_s: float = 900.0) -> dict:
    container = container or fsck_container()
    return fsck_mod.run_dryrun(container, disk=disk, timeout_s=timeout_s)


def unreachable_probe(container: str | None = None, disk: str = DEFAULT_FSCK_DISK):
    """Return a 0-arg callable giving the current fsck.unreachable int (for forced_gc_to_fixpoint).
    A failed/timed-out summary fsck raises, which the GC drive treats as a skipped probe."""
    container = container or fsck_container()
    def _fn():
        s = fsck_summary(container, disk)
        return int(s.get("unreachable", 0))
    return _fn


# ---------------------------------------------------------------------------
# Quiescence
# ---------------------------------------------------------------------------

def _table_has_ttl(node, table: str) -> bool:
    try:
        v = node.scalar(
            f"SELECT count() FROM system.tables WHERE database='default' AND name='{table}' "
            f"AND create_table_query LIKE '%TTL %'")
        return int(v or 0) > 0
    except Exception:
        return False


def _cluster_counts(cluster, table_filter: str | None = None) -> dict:
    """Cluster-wide backlog counts. `table_filter` is an optional SQL predicate fragment to scope to
    scenario tables (e.g. "table LIKE 's05_%'"); None counts all tables."""
    pred = f" WHERE {table_filter}" if table_filter else ""
    pred_done = (f" WHERE ({table_filter}) AND NOT is_done" if table_filter
                 else " WHERE NOT is_done")
    repl = mut = mrg = errs = 0
    for n in cluster.nodes():
        try:
            repl += int(n.scalar(f"SELECT count() FROM system.replication_queue{pred}"))
            mut += int(n.scalar(f"SELECT count() FROM system.mutations{pred_done}"))
            mrg += int(n.scalar(f"SELECT count() FROM system.merges{pred}"))
            errs += int(n.scalar(
                "SELECT count() FROM system.replication_queue"
                + (f" WHERE ({table_filter}) AND" if table_filter else " WHERE")
                + " last_exception != ''"))
        except Exception:
            pass
    return {"repl": repl, "mut": mut, "merges": mrg, "errored": errs,
            "backlog": repl + mut + mrg}


def quiesce_cluster(cluster, tables, *, table_filter: str | None = None, optimize: bool = True,
                    sync_timeout_s: float = 1200.0, drain_timeout_s: float = 1800.0,
                    no_progress_grace_s: float = 180.0, log_fn=print) -> int:
    """Drain the cluster to quiescence and return the server `now()` captured after convergence.

    Steps: `SYSTEM SYNC REPLICA` each table on each node (read-only-retry tolerant), drain the
    replication queue / mutations / merges (merge-aware: a flat backlog with active merges is
    progressing, not hung), optionally `OPTIMIZE ... FINAL` + `MATERIALIZE TTL` the listed tables,
    then drain again. A queue entry carrying a real `last_exception` fails fast.

    `tables` may be a small list (optimized individually). For many-namespace scenarios pass a short
    `tables` list (only those touched) and a `table_filter` to scope the backlog counts.
    """
    settings = {"receive_timeout": int(sync_timeout_s), "max_execution_time": int(sync_timeout_s)}
    for node in cluster.nodes():
        for t in tables:
            try:
                node.command(f"SYSTEM SYNC REPLICA {t}", timeout=sync_timeout_s, settings=settings)
            except QueryError as e:
                if e.is_readonly or e.is_node_down:
                    log_fn(f"quiesce: SYNC {t} on {node.container} transient ({e.code}); retrying once")
                    time.sleep(2)
                    try:
                        node.command(f"SYSTEM SYNC REPLICA {t}", timeout=sync_timeout_s, settings=settings)
                    except Exception as e2:
                        log_fn(f"quiesce: SYNC {t} retry failed: {e2}")
                else:
                    raise

    def drain(label):
        deadline = time.time() + drain_timeout_s
        last_backlog = None
        last_progress = time.time()
        error_since = None
        while True:
            c = _cluster_counts(cluster, table_filter)
            now = time.time()
            if c["errored"] > 0:
                if error_since is None:
                    error_since = now
                elif (now - error_since) > no_progress_grace_s:
                    raise RuntimeError(f"quiesce {label}: {c['errored']} replication-queue entries carry "
                                       f"a real last_exception for over {no_progress_grace_s:.0f}s — "
                                       f"genuine error")
            else:
                error_since = None
            if c["backlog"] == 0:
                return
            if last_backlog is None or c["backlog"] < last_backlog:
                last_backlog = c["backlog"]
                last_progress = now
            grace_exceeded = (now - last_progress) > no_progress_grace_s
            if c["merges"] == 0 and grace_exceeded and now > deadline:
                raise RuntimeError(f"quiesce {label}: backlog stuck at {c['backlog']} with no active "
                                   f"merges past the {drain_timeout_s:.0f}s budget — genuine hang")
            if c["merges"] > 0 and grace_exceeded:
                log_fn(f"quiesce {label}: backlog={c['backlog']} flat but {c['merges']} active "
                       f"merge(s) — still progressing")
            time.sleep(1)

    drain("initial")
    if optimize:
        for node in cluster.nodes():
            for t in tables:
                try:
                    node.command(f"OPTIMIZE TABLE {t} FINAL", timeout=sync_timeout_s, settings=settings)
                except QueryError as e:
                    log_fn(f"quiesce: OPTIMIZE {t} on {node.container}: {e}")
                # MATERIALIZE TTL only where a TTL exists (else INCORRECT_QUERY, code 80).
                if _table_has_ttl(node, t):
                    try:
                        node.command(f"ALTER TABLE {t} MATERIALIZE TTL", timeout=sync_timeout_s,
                                     settings=settings)
                    except QueryError as e:
                        log_fn(f"quiesce: MATERIALIZE TTL {t} on {node.container}: {e}")
        drain("after OPTIMIZE/MATERIALIZE TTL")
    return int(cluster.nodes()[0].scalar("SELECT toUnixTimestamp(now())"))


def settle_fsck(container: str | None = None, disk: str = DEFAULT_FSCK_DISK,
                *, stable: int = 2, timeout_s: float = 300.0, interval_s: float = 3.0,
                log_fn=print) -> dict:
    """Poll a summary fsck until reachable+dangling are stable for `stable` reads (publishes from
    the just-drained workload have settled), then return the last summary."""
    container = container or fsck_container()
    deadline = time.time() + timeout_s
    history = []
    last = {}
    while True:
        try:
            last = fsck_summary(container, disk)
            # Stability key deliberately EXCLUDES `unreachable`: background GC churns it while
            # draining (S05 full: oscillated 1200->414->1203 for the whole 300 s budget and settle
            # never stabilized). Settle only gates "workload publishes stopped moving" —
            # `unreachable` convergence is owned by the forced_gc_to_fixpoint step that follows.
            key = (last.get("reachable"), last.get("dangling"))
        except fsck_mod.FsckTimeout:
            log_fn("settle_fsck: summary fsck timed out; returning last")
            return last
        history.append(key)
        if len(history) >= stable and len(set(history[-stable:])) == 1:
            return last
        if time.time() > deadline:
            log_fn(f"settle_fsck: did not stabilize in {timeout_s:.0f}s; returning last (history={history[-4:]})")
            return last
        time.sleep(interval_s)
