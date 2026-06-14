"""CA soak-test Phase-1 green-path driver.

Drives the deterministic ledger workload (`soak.ledger`) against a live 2-replica CA cluster
(`soak.cluster`) while mirroring every op into the authoritative in-memory `Model` (`soak.model`).
At periodic quiesced checkpoints it asserts that BOTH replicas equal the model exactly and that the
content-addressed pool is clean (fsck dangling==0, GC fixpoint reached with unreachable==0, and the
GC dry-run preview is a subset of the fsck `unreachable` set).

Concurrency model:
  * INSERT and OPTIMIZE ops run CONCURRENTLY across `--workers` threads.
  * UPDATE / DELETE / TRUNCATE / DROP_PARTITION are GLOBAL BARRIERS: all in-flight inserts are drained
    before the mutation is applied to the cluster and the model, then concurrent inserts resume. This
    keeps the model's predicate (WHERE bucket=...) application exact — no insert can race a mutation.

Every op is applied to the model in op_id order under a lock. For an INSERT the row count
`n = 1 + (op.param % insert_block)` is computed ONCE and passed to BOTH the SQL emitter and the model
so the cluster and the model consume the identical effective ledger.

Cliff cap (determinism-preserving): TRUNCATE/DROP_PARTITION ops are "cliffs" that wipe the table.
Per-op cliff weights would wipe the table every ~33 ops, starving accumulation. We preprocess the
ledger into an EFFECTIVE ledger where a cliff is DEMOTED to OPTIMIZE unless (a) fewer than MAX_CLIFFS
cliffs have executed so far AND (b) at least MIN_OPS_BETWEEN_CLIFFS ops have elapsed since the previous
executed cliff. Demotion is a pure function of op_id order, so the cluster and model consume the
IDENTICAL effective ledger.
"""

import argparse
import json
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_EXCEPTION

from soak.cluster import Cluster, QueryError, retry_on_aborted
from soak.ledger import generate_ledger, Op, OpType
from soak.model import Model
from soak.workload import insert_values_sql, update_sql, delete_sql, truncate_sql
from soak.checker import (
    CheckpointFailure,
    quiesce,
    query_aggregates,
    compare_aggregates,
    drive_gc_to_fixpoint,
)
from soak.fsck import run_fsck, run_dryrun

TABLE = "ca_stress"
FSCK_CONTAINER = "ca-soak-ch1-1"

MAX_CLIFFS = 2

DDL_TEMPLATE = """
CREATE TABLE {table}
(
    op_id UInt64, writer UInt16, bucket UInt16, k UInt64, ts DateTime64(3),
    version UInt32, v Int64, payload String, row_fp UInt64
)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{table}','{{replica}}')
PARTITION BY toYYYYMMDD(ts)
ORDER BY (bucket,k,op_id)
TTL toDateTime(ts) + INTERVAL 90 MINUTE DELETE
SETTINGS storage_policy='ca', min_bytes_for_wide_part=0, min_rows_for_wide_part=0
""".strip()

CLIFF_TYPES = (OpType.TRUNCATE, OpType.DROP_PARTITION)
BARRIER_TYPES = (OpType.UPDATE, OpType.DELETE, OpType.TRUNCATE, OpType.DROP_PARTITION)


def log(msg: str):
    print(f"[soak.run] {msg}", flush=True)


def build_effective_ledger(ledger, max_cliffs: int, min_ops_between_cliffs: int):
    """Demote cliff ops (TRUNCATE/DROP_PARTITION) to OPTIMIZE beyond the cap. Pure function of op_id
    order so cluster and model consume the identical effective ledger."""
    out = []
    cliffs_done = 0
    last_cliff_op = None
    for op in ledger:
        if op.type in CLIFF_TYPES:
            far_enough = last_cliff_op is None or (op.op_id - last_cliff_op) >= min_ops_between_cliffs
            if cliffs_done < max_cliffs and far_enough:
                cliffs_done += 1
                last_cliff_op = op.op_id
                out.append(op)
            else:
                out.append(Op(op_id=op.op_id, type=OpType.OPTIMIZE, target=op.target, param=op.param))
        else:
            out.append(op)
    return out


def node_for(cluster, target: int):
    return cluster.node1 if target == 0 else cluster.node2


# INSERT-mode SETTINGS suffixes. B138: the soak MUST run with SYNC inserts. The decisive A/B
# (B138) showed that with sync inserts the count checkpoints match the model EXACTLY (the
# ABORTED-retry of a failed sync INSERT is idempotent: a failed sync insert leaves NO committed
# dedup token, so a retry truly re-inserts), whereas an async insert can commit its dedup token
# before the part publish fails, so the retry-of-async-insert is deduped against a part that was
# never published -> lost rows (B139, dedup-token-vs-part hazard). Hence "default" maps to the sync
# settings; "async" remains selectable for a future async-specific test.
INSERT_MODE_SETTINGS = {
    "default": "SETTINGS async_insert=0",
    "async": "SETTINGS async_insert=1, wait_for_async_insert=1",
    "sync": "SETTINGS async_insert=0",
}


class Driver:
    def __init__(self, cluster, model, seed, base_time, workers, insert_mode="default"):
        self.cluster = cluster
        self.model = model
        self.seed = seed
        self.base_time = base_time
        self.workers = workers
        self.insert_settings = INSERT_MODE_SETTINGS[insert_mode]
        self.model_lock = threading.Lock()
        self.executor = ThreadPoolExecutor(max_workers=workers)
        self.inflight = []          # futures for in-flight concurrent (INSERT/OPTIMIZE) ops
        self.last_op = None         # last op executed (for failure.json)
        self.aborted_retries = 0    # count of ABORTED-retried INSERT attempts (B137 race evidence)
        self._aborted_lock = threading.Lock()

    # -- concurrent (non-barrier) op execution ------------------------------------------------
    def _submit_insert(self, op):
        n = 1 + (op.param % self.model.insert_block)
        # Apply to the model immediately in op_id order (inserts touch disjoint rids, so order among
        # concurrent inserts is irrelevant; we hold the lock only to keep the dict thread-safe).
        with self.model_lock:
            self.model.apply(op)
        sql = insert_values_sql(self.seed, op.op_id, n, TABLE, self.base_time, settings=self.insert_settings)
        node = node_for(self.cluster, op.target)
        fut = self.executor.submit(self._insert_with_retry, node, sql, op.op_id)
        self.inflight.append(fut)

    def _insert_with_retry(self, node, sql, op_id):
        """Execute one INSERT, retrying ONLY on the retryable ABORTED transient (B137). Scoped to
        INSERTs: mutations/DDL are NOT routed here (they have different idempotency)."""
        def on_retry(attempt, err):
            with self._aborted_lock:
                self.aborted_retries += 1
            log(f"INSERT op_id={op_id} hit retryable ABORTED (attempt {attempt}); retrying. {err}")
        retry_on_aborted(lambda: node.command(sql), on_retry=on_retry)

    def _submit_optimize(self, op):
        # OPTIMIZE has no model effect; run it concurrently as a cluster-only op.
        node = node_for(self.cluster, op.target)
        fut = self.executor.submit(node.command, f"OPTIMIZE TABLE {TABLE}")
        self.inflight.append(fut)

    def drain(self):
        """Wait for all in-flight inserts/optimizes; re-raise the first worker exception loudly."""
        if not self.inflight:
            return
        done, _ = wait(self.inflight, return_when=FIRST_EXCEPTION)
        futures, self.inflight = self.inflight, []
        for fut in futures:
            fut.result()    # propagates any worker exception

    def drain_silent(self):
        """Wait for in-flight ops on the failure path WITHOUT re-raising worker exceptions, so the
        CheckpointFailure is the surfaced cause rather than a masking worker error."""
        futures, self.inflight = self.inflight, []
        for fut in futures:
            try:
                fut.result()
            except Exception:
                pass

    # -- barrier (mutation) op execution ------------------------------------------------------
    def apply_barrier(self, op):
        """Drain inserts, then apply the mutation to the cluster (on op.target's replica) AND the
        model. The model mutation must see exactly the rows the cluster sees, hence the drain."""
        self.drain()
        node = node_for(self.cluster, op.target)
        if op.type == OpType.UPDATE:
            node.command(update_sql(TABLE, self.model._pred_bucket(op.param)))
        elif op.type == OpType.DELETE:
            node.command(delete_sql(TABLE, self.model._pred_bucket(op.param)))
        elif op.type == OpType.TRUNCATE:
            node.command(truncate_sql(TABLE))
        elif op.type == OpType.DROP_PARTITION:
            # Single base_time day -> one partition -> drop == full clear (matches Model).
            node.command(truncate_sql(TABLE))
        else:
            raise AssertionError(f"apply_barrier on non-barrier op {op.type}")
        with self.model_lock:
            self.model.apply(op)

    def execute(self, op):
        if op.type == OpType.INSERT:
            self._submit_insert(op)
        elif op.type == OpType.OPTIMIZE:
            self._submit_optimize(op)
        elif op.type in BARRIER_TYPES:
            self.apply_barrier(op)
        else:
            raise AssertionError(f"unknown op type {op.type}")
        self.last_op = op


def checkpoint(driver, cluster, model, phase):
    """Pause workers (drain), quiesce, assert both replicas == model, drive GC to fixpoint, assert a
    clean CA pool. Raises CheckpointFailure on any divergence."""
    driver.drain()
    now = quiesce(cluster, TABLE)

    if model.ambiguous_band_nonempty(now, eps=10):
        raise CheckpointFailure(
            f"ambiguous TTL band non-empty at now={now} (a row sits within 10s of its TTL boundary; "
            f"scheduling issue — checkpoint cannot be asserted exactly)")

    exp = model.aggregates(now)
    n1 = query_aggregates(cluster.node1, TABLE)
    n2 = query_aggregates(cluster.node2, TABLE)
    compare_aggregates(exp, n1, n2)

    drive_gc_to_fixpoint(cluster, lambda: run_fsck(FSCK_CONTAINER)["unreachable"])

    f = run_fsck(FSCK_CONTAINER)
    if f.get("dangling") != 0:
        raise CheckpointFailure(f"fsck dangling != 0: {f.get('dangling')} (INV-NO-LOSS) detail={f.get('detail')}")
    if f.get("exit_code") != 0:
        raise CheckpointFailure(f"fsck exit_code != 0: {f.get('exit_code')} stderr={f.get('stderr')}")
    if f.get("unreachable") != 0:
        raise CheckpointFailure(f"fsck unreachable != 0 at fixpoint: {f.get('unreachable')}")

    dr = run_dryrun(FSCK_CONTAINER)
    unreachable_keys = {row["key"] for row in f.get("detail", []) if row["class"] == "unreachable"}
    for entry in dr.get("entries", []):
        if entry["key"] not in unreachable_keys:
            raise CheckpointFailure(
                f"dryrun key {entry['key']!r} not in fsck unreachable set (dryrun must be a subset of "
                f"unreachable); dryrun_count={dr.get('count')} unreachable={sorted(unreachable_keys)}")

    return now, exp, n1, n2, f, dr


def write_failure(path, *, seed, base_time, op_id, phase, model_expected, n1, n2, fsck, last_op, error=None):
    payload = {
        "seed": seed,
        "base_time": base_time,
        "op_id": op_id,
        "phase": phase,
        "error": error,
        "model_expected": model_expected,
        "node1": n1,
        "node2": n2,
        "fsck": {k: v for k, v in (fsck or {}).items() if k != "stdout"},
        "last_op": last_op,
    }
    with open(path, "w") as fh:
        json.dump(payload, fh, indent=2, default=str)
    return payload


def main(argv=None):
    ap = argparse.ArgumentParser(description="CA soak Phase-1 green-path driver")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--phase", type=int, default=1)
    ap.add_argument("--ops", type=int, required=True)
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--checkpoint-every", type=int, required=True)
    ap.add_argument("--until-op", type=int, default=None, help="optional cap on the last op_id executed")
    ap.add_argument("--max-cliffs", type=int, default=MAX_CLIFFS)
    ap.add_argument("--min-ops-between-cliffs", type=int, default=None)
    ap.add_argument("--failure-json", default="failure.json")
    ap.add_argument("--insert-mode", choices=sorted(INSERT_MODE_SETTINGS), default="default",
                    help="force INSERT async/sync mode by appending a SETTINGS clause to each INSERT. "
                         "'default' and 'sync' both set async_insert=0 (B138: sync is required for an "
                         "idempotent ABORTED-retry; async loses rows via the dedup-token-vs-part hazard, "
                         "B139); 'async' sets async_insert=1, wait_for_async_insert=1.")
    args = ap.parse_args(argv)

    cluster = Cluster()

    # Capture base_time ONCE, 60s in the past, so freshly-inserted rows are NOT born expired.
    base_time = int(cluster.node1.scalar("SELECT toUnixTimestamp(now())")) - 60
    log(f"base_time={base_time} (needed for replay) seed={args.seed} phase={args.phase} "
        f"ops={args.ops} workers={args.workers} checkpoint_every={args.checkpoint_every}")

    model = Model(args.seed, base_time=base_time)

    # Fresh table on BOTH replicas.
    ddl = DDL_TEMPLATE.format(table=TABLE)
    for node in cluster.nodes():
        node.command(f"DROP TABLE IF EXISTS {TABLE} SYNC")
    for node in cluster.nodes():
        node.command(ddl)
    log(f"created {TABLE} on both replicas")

    ledger = generate_ledger(args.seed, args.ops)
    min_gap = args.min_ops_between_cliffs if args.min_ops_between_cliffs is not None else max(1, args.ops // 4)
    effective = build_effective_ledger(ledger, args.max_cliffs, min_gap)
    n_cliffs = sum(1 for op in effective if op.type in CLIFF_TYPES)
    log(f"effective ledger: {len(effective)} ops, {n_cliffs} cliffs (cap={args.max_cliffs}, min_gap={min_gap})")

    driver = Driver(cluster, model, args.seed, base_time, args.workers, insert_mode=args.insert_mode)
    log(f"insert_mode={args.insert_mode} insert_settings_suffix={INSERT_MODE_SETTINGS[args.insert_mode]!r}")

    executed = 0
    last_op = None
    try:
        for op in effective:
            if args.until_op is not None and op.op_id > args.until_op:
                break
            # A barrier op (mutation) implicitly drains inserts inside apply_barrier.
            driver.execute(op)
            last_op = op
            executed += 1

            if executed % args.checkpoint_every == 0:
                log(f"checkpoint at executed={executed} (op_id={op.op_id})")
                now, exp, n1, n2, f, dr = checkpoint(driver, cluster, model, args.phase)
                log(f"checkpoint OK: now={now} count={exp['count']} "
                    f"fsck reachable={f.get('reachable')} unreachable={f.get('unreachable')} "
                    f"dangling={f.get('dangling')} dryrun_count={dr.get('count')}")

        # Final checkpoint (if the last one didn't land exactly on a boundary).
        if executed == 0 or executed % args.checkpoint_every != 0:
            log(f"final checkpoint at executed={executed}")
            now, exp, n1, n2, f, dr = checkpoint(driver, cluster, model, args.phase)
            log(f"final checkpoint OK: now={now} count={exp['count']} "
                f"unreachable={f.get('unreachable')} dangling={f.get('dangling')}")

    except (CheckpointFailure, QueryError, OSError) as e:
        # CheckpointFailure -> a model/replica divergence or a dirty CA pool at a quiesced checkpoint.
        # QueryError -> a worker-side cluster error during op execution (e.g. a write-path race).
        # OSError (incl. socket TimeoutError) -> a transport-level failure talking to a replica (e.g. a
        # blocking admin op exceeding its socket timeout). In every case record a structured failure with
        # the reproducer (seed, base_time, op_id, error) instead of dumping a raw traceback, so the
        # failure can be replayed and triaged.
        if isinstance(e, CheckpointFailure):
            kind = "CHECKPOINT FAILURE"
        elif isinstance(e, QueryError):
            kind = "WORKLOAD FAILURE"
        else:
            kind = "TRANSPORT FAILURE"
        driver.drain_silent()
        # Best-effort: capture the current cluster + fsck state for the report.
        try:
            now = int(cluster.node1.scalar("SELECT toUnixTimestamp(now())"))
            exp = model.aggregates(now)
        except Exception:
            exp = None
        try:
            n1 = query_aggregates(cluster.node1, TABLE)
            n2 = query_aggregates(cluster.node2, TABLE)
        except Exception:
            n1 = n2 = None
        try:
            f = run_fsck(FSCK_CONTAINER)
        except Exception:
            f = None
        op_id = last_op.op_id if last_op is not None else None
        last_op_d = last_op.__dict__ if last_op is not None else None
        payload = write_failure(
            args.failure_json, seed=args.seed, base_time=base_time, op_id=op_id, phase=args.phase,
            model_expected=exp, n1=n1, n2=n2, fsck=f, last_op=last_op_d, error=f"{kind}: {e}")
        print(f"{kind}: {e}", file=sys.stderr)
        print(json.dumps(payload, indent=2, default=str), file=sys.stderr)
        driver.executor.shutdown(wait=False, cancel_futures=True)
        sys.exit(1)
    finally:
        driver.executor.shutdown(wait=True)

    log(f"ABORTED-retried INSERT attempts (B137 race fired and was handled): {driver.aborted_retries}")
    print("PHASE1 OK")
    return 0


if __name__ == "__main__":
    main()
