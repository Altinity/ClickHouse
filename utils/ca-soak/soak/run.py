"""CA soak-test Phase-1 green-path driver.

Drives the deterministic ledger workload (`soak.ledger`) against a live 2-replica CA cluster
(`soak.cluster`) while mirroring every op into the authoritative in-memory `Model` (`soak.model`).
At periodic quiesced checkpoints it HARD-ASSERTS the invariants the CURRENT CA design guarantees:
BOTH replicas equal the model exactly (no loss, no divergence), fsck `dangling==0` (INV-NO-LOSS:
every ref-reachable object exists), and the GC dry-run preview is a subset of the fsck `unreachable`
set (GC never plans to delete a reachable object). The residual fsck `unreachable` after the
incremental GC reaches its fixpoint is TRACKED, not failed on: it is the known M-F Full-GC debris
(B140) — per CA spec §8 the incremental GC cannot reclaim blobs orphaned by a
displaced-before-expansion tree; the Full-GC mark-sweep (milestone M-F, not yet implemented) is the
documented backstop that drains it to 0.

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
import time
import traceback
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_EXCEPTION

from soak.cluster import (
    Cluster,
    QueryError,
    retry_on_aborted,
    retry_on_transport,
    is_transport_error,
)
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
from soak.chaos import generate_chaos_schedule, apply_fault

TABLE = "ca_stress"
FSCK_CONTAINER = "ca-soak-ch1-1"

MAX_CLIFFS = 2

# search_orphaned_parts_disks='local' (B143): the harness config defines a SECOND CA disk `ca_ro`
# (a read-only alias of the SAME RustFS pool, used by the offline `clickhouse disks fsck` applet)
# which is NOT in the `ca` storage policy. On a server RESTART (Phase-2 chaos), MergeTreeData's
# orphaned-parts scan (`loadDataParts`, default `search_orphaned_parts_disks=any`) iterates EVERY
# disk in the context map -- including the remote `ca_ro` -- finds the policy parts under its
# same-pool path, and refuses to attach the table with UNKNOWN_DISK (code 479) "Part ... was found
# on disk 'ca_ro' which is not defined in the storage policy 'ca'". That is a FALSE positive (ca_ro
# is the same pool as ca, just a read-only view), not a real orphan. Limiting the scan to LOCAL
# disks skips the remote `ca_ro` alias while still catching genuinely-orphaned local parts.
# Phase-1 never restarted a server, so this only surfaced under Phase-2 restart chaos.
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
SETTINGS storage_policy='ca', min_bytes_for_wide_part=0, min_rows_for_wide_part=0,
         search_orphaned_parts_disks='local'
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


# Phase-2 transport-retry bound. A node killed/paused by chaos comes back within its fault
# duration_s (<= 60s) plus restart time; with capped-exponential backoff (0.5s..8s) this many
# attempts spans several minutes -- generously longer than any single fault window. Exhausting it
# means a node NEVER recovered, which the task spec says IS a failure (the feature must survive
# crash+restart), so the exhausted transport error is re-raised loudly.
TRANSPORT_ATTEMPTS = 40


class Driver:
    def __init__(self, cluster, model, seed, base_time, workers, insert_mode="default",
                 transport_resilient=False):
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
        # Phase 2: tolerate a node going down mid-op (transport failure) by retrying with backoff and
        # rerouting to the OTHER replica. Off in Phase 1 (no chaos -> a transport error is a real bug
        # that must surface immediately).
        self.transport_resilient = transport_resilient
        self.transport_retries = 0  # count of transport-retried op attempts (chaos evidence)
        self._transport_lock = threading.Lock()

    def _note_transport_retry(self, op_kind, op_id, attempt, node, err):
        with self._transport_lock:
            self.transport_retries += 1
        log(f"{op_kind} op_id={op_id} transport failure on {node} (attempt {attempt}); "
            f"rerouting/retrying. {type(err).__name__}: {err}")

    # -- concurrent (non-barrier) op execution ------------------------------------------------
    def _submit_insert(self, op):
        n = 1 + (op.param % self.model.insert_block)
        # Apply to the model immediately in op_id order (inserts touch disjoint rids, so order among
        # concurrent inserts is irrelevant; we hold the lock only to keep the dict thread-safe).
        with self.model_lock:
            self.model.apply(op)
        sql = insert_values_sql(self.seed, op.op_id, n, TABLE, self.base_time, settings=self.insert_settings)
        fut = self.executor.submit(self._insert_with_retry, op.target, sql, op.op_id)
        self.inflight.append(fut)

    def _nodes_starting_at(self, target):
        """Replica list to try in order, starting at the op's assigned replica then the other one --
        the rerouting order for transport retries."""
        primary = node_for(self.cluster, target)
        other = node_for(self.cluster, 1 - target)
        return [primary, other]

    def _insert_with_retry(self, target, sql, op_id):
        """Execute one INSERT. Inner: retry the retryable ABORTED transient (B137). Outer (Phase 2):
        on a TRANSPORT failure (node killed/paused mid-op), retry with bounded backoff and REROUTE to
        the other replica -- the INSERT replicates back and RMT block-dedup keeps the retry idempotent,
        and the model already applied the op exactly once."""
        def aborted_on_retry(attempt, err):
            with self._aborted_lock:
                self.aborted_retries += 1
            log(f"INSERT op_id={op_id} hit retryable ABORTED (attempt {attempt}); retrying. {err}")

        order = self._nodes_starting_at(target)

        def one_attempt(attempt_idx):
            node = order[attempt_idx % len(order)]
            retry_on_aborted(lambda: node.command(sql), on_retry=aborted_on_retry)

        self._with_transport_retry("INSERT", op_id, one_attempt)

    def _with_transport_retry(self, op_kind, op_id, one_attempt):
        """Drive `one_attempt(attempt_idx)` once if not transport-resilient (Phase 1), else with the
        bounded transport-retry loop that reroutes across replicas (Phase 2). `one_attempt` receives a
        0-based attempt index so it can pick the replica."""
        if not self.transport_resilient:
            one_attempt(0)
            return
        counter = {"i": 0}

        def attempt():
            i = counter["i"]
            counter["i"] += 1
            return one_attempt(i)

        def on_retry(attempt_no, err):
            self._note_transport_retry(op_kind, op_id, attempt_no, "assigned-replica", err)

        retry_on_transport(attempt, attempts=TRANSPORT_ATTEMPTS, on_retry=on_retry)

    def _submit_optimize(self, op):
        # OPTIMIZE has no model effect; run it concurrently as a cluster-only op. Under chaos it is
        # also transport-resilient (reroute/retry) so a node-down OPTIMIZE doesn't fail the run.
        fut = self.executor.submit(self._optimize_with_retry, op.target, op.op_id)
        self.inflight.append(fut)

    def _optimize_with_retry(self, target, op_id):
        order = self._nodes_starting_at(target)

        def one_attempt(attempt_idx):
            order[attempt_idx % len(order)].command(f"OPTIMIZE TABLE {TABLE}")

        self._with_transport_retry("OPTIMIZE", op_id, one_attempt)

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
        model. The model mutation must see exactly the rows the cluster sees, hence the drain. Under
        chaos the mutation must LAND even if its target replica is down: it is transport-resilient and
        reroutes to the other replica (replicated DDL/mutation; idempotent table-level effect). It is
        applied to the model ONLY AFTER the cluster command succeeds, so a never-landed mutation
        surfaces as a transport failure instead of silently diverging the model."""
        self.drain()
        if op.type == OpType.UPDATE:
            sql = update_sql(TABLE, self.model._pred_bucket(op.param))
        elif op.type == OpType.DELETE:
            sql = delete_sql(TABLE, self.model._pred_bucket(op.param))
        elif op.type == OpType.TRUNCATE:
            sql = truncate_sql(TABLE)
        elif op.type == OpType.DROP_PARTITION:
            # Single base_time day -> one partition -> drop == full clear (matches Model).
            sql = truncate_sql(TABLE)
        else:
            raise AssertionError(f"apply_barrier on non-barrier op {op.type}")

        order = self._nodes_starting_at(op.target)

        def one_attempt(attempt_idx):
            order[attempt_idx % len(order)].command(sql)

        self._with_transport_retry(f"BARRIER:{op.type.name}", op.op_id, one_attempt)
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

    # Drive the INCREMENTAL GC to ITS fixpoint (unreachable STOPS DECREASING). The residual is the
    # known M-F-debris (B140): per CA spec §8 the incremental GC cannot reclaim blobs orphaned by a
    # displaced-before-expansion tree; the Full-GC mark-sweep (milestone M-F, not yet implemented) is
    # the documented backstop that drains it to 0. We TRACK the residual, not fail on it.
    residual = drive_gc_to_fixpoint(cluster, lambda: run_fsck(FSCK_CONTAINER)["unreachable"])

    f = run_fsck(FSCK_CONTAINER)

    # --- HARD ASSERTS: the invariants the CURRENT CA design guarantees ------------------------
    # INV-NO-LOSS: every ref-reachable object exists. dangling>0 means a referenced object is
    # missing -> real data loss. The killer assertion.
    #
    # B144/B145: in phase 2 the `drive_gc_to_fixpoint` churn above issues many object-store
    # operations that can RE-OPEN the transient post-restart fsck-incoherence window the entry gate
    # (`wait_for_pool_consistent` in `do_checkpoint`) had already waited out -- so this post-GC fsck
    # can transiently read `dangling>0` even though the pool is intact (count==model on both
    # replicas, and a moments-later re-read clears to 0; see the B145 capture where the assert saw
    # dangling=2434 while the handler's re-fsck saw dangling=0). The entry gate only guards the
    # checkpoint ENTRY, not this second read. So in phase 2 we RE-CONFIRM a `dangling>0` reading via
    # the SAME bounded settle: re-fsck until `dangling==0` is STABLE. This is NOT papering over loss --
    # `wait_for_pool_consistent` FAILS LOUDLY (CheckpointFailure) if `dangling>0` PERSISTS past the
    # bound, which IS a real crash-recovery / durability finding. Phase 1 has no backend faults, so a
    # single read is authoritative and a `dangling>0` there fails immediately.
    if f.get("dangling") != 0:
        if phase == 2:
            f = wait_for_pool_consistent(lambda: run_fsck(FSCK_CONTAINER))
        else:
            raise CheckpointFailure(f"fsck dangling != 0: {f.get('dangling')} (INV-NO-LOSS) detail={f.get('detail')}")
    if f.get("exit_code") != 0:
        raise CheckpointFailure(f"fsck exit_code != 0: {f.get('exit_code')} stderr={f.get('stderr')}")

    # GC never plans to delete a reachable object: {dryrun delete set} subset of {fsck unreachable}.
    dr = run_dryrun(FSCK_CONTAINER)
    unreachable_keys = {row["key"] for row in f.get("detail", []) if row["class"] == "unreachable"}
    for entry in dr.get("entries", []):
        if entry["key"] not in unreachable_keys:
            raise CheckpointFailure(
                f"dryrun key {entry['key']!r} not in fsck unreachable set (dryrun must be a subset of "
                f"unreachable); dryrun_count={dr.get('count')} unreachable={sorted(unreachable_keys)}")

    # --- TRACK (do NOT hard-fail): residual unreachable is the known M-F Full-GC debris -------
    unreachable = f.get("unreachable", 0)
    reachable = f.get("reachable", 0)
    log(f"unreachable={unreachable} (M-F debris, pending Full GC / B140); reachable={reachable} "
        f"dangling=0 dryrun_subset=ok")
    # Crude regression tripwire for a NEW unbounded-leak class distinct from B140: only fire if the
    # debris grows PATHOLOGICALLY relative to the live set. Kept deliberately generous (not a tight
    # bound) so it cannot flake on normal residual; a later metrics-curve task tracks growth precisely.
    if unreachable > 50 * reachable + 100000:
        raise CheckpointFailure(
            f"unreachable={unreachable} grew pathologically vs reachable={reachable} "
            f"(> 50*reachable + 100000) — a NEW unbounded-leak class distinct from M-F debris (B140)")

    return now, exp, n1, n2, f, dr


def wait_for_healthy(cluster, *, timeout_s: float = 180.0, settle_s: float = 2.0,
                     sleep_fn=time.sleep, monotonic_fn=time.monotonic):
    """Block until BOTH replicas answer `/ping` with HTTP 200, then settle briefly. Used by the
    Phase-2 recovery wait after a fault window: a killed/restarted node must be HTTP-healthy again
    BEFORE the checkpoint quiesces (else SYSTEM SYNC REPLICA / aggregates would hit a still-down node
    and the checkpoint would spuriously fail). FAILS LOUDLY (CheckpointFailure) if a node never
    returns within the bound -- a restarted node that never comes back IS a crash-recovery failure,
    not something to swallow."""
    deadline = monotonic_fn() + timeout_s
    while True:
        if all(node.ping() for node in cluster.nodes()):
            sleep_fn(settle_s)
            if all(node.ping() for node in cluster.nodes()):
                return
        if monotonic_fn() > deadline:
            states = {repr(n): n.ping() for n in cluster.nodes()}
            raise CheckpointFailure(
                f"node(s) never returned HTTP-healthy within {timeout_s:.0f}s after a fault window "
                f"(crash-recovery failure): ping states={states}")
        sleep_fn(1.0)


def wait_for_pool_consistent(fsck_fn, *, timeout_s: float = 180.0, stable: int = 2,
                             interval_s: float = 3.0, sleep_fn=time.sleep, monotonic_fn=time.monotonic):
    """Poll `fsck_fn()` (a fresh `run_fsck` result) until the CA pool is SELF-CONSISTENT -- fsck
    `dangling==0` AND `exit_code==0` for `stable` consecutive reads -- then return the last fsck dict.

    Rationale (B144): a fault that kills/restarts the RustFS OBJECT STORE leaves a transient
    post-restart window where recently-written objects are not yet visible/durable through a FRESH
    read-only fsck mount, even though the running server reads the table correctly (count==model on
    both replicas). In that window fsck transiently reports `dangling>0` (HEAD-absent reachable trees)
    and the GC dryrun/fsck views are mutually incoherent, so the dryrun-subset HARD assert can fire on
    a non-bug. `wait_for_healthy` only gates ClickHouse `/ping`, NOT the object-store backend's
    recovery, so the recovery checkpoint could assert pool invariants against a still-settling store.

    This gate waits for the pool to reach a COHERENT cut (`dangling==0`) before the checkpoint runs
    its hard dryrun-subset assert. It FAILS LOUDLY (`CheckpointFailure`) if the pool never reaches
    `dangling==0` within the bound -- a PERSISTENT post-restart `dangling>0` is then a REAL
    crash-recovery / object-store-durability finding, not a transient, and must surface.

    `sleep_fn`/`monotonic_fn` are injectable so the loop is pure-testable."""
    deadline = monotonic_fn() + timeout_s
    consecutive_clean = 0
    last = None
    while True:
        last = fsck_fn()
        clean = last.get("dangling") == 0 and last.get("exit_code") == 0
        consecutive_clean = consecutive_clean + 1 if clean else 0
        if consecutive_clean >= stable:
            return last
        if monotonic_fn() > deadline:
            raise CheckpointFailure(
                f"CA pool never reached a self-consistent state (fsck dangling==0) within {timeout_s:.0f}s "
                f"after a fault window -- PERSISTENT dangling={last.get('dangling')} "
                f"(exit_code={last.get('exit_code')}) is a REAL crash-recovery / object-store durability "
                f"finding (INV-NO-LOSS), not a transient. reachable={last.get('reachable')} "
                f"unreachable={last.get('unreachable')}")
        sleep_fn(interval_s)


class ChaosRunner(threading.Thread):
    """Background thread that fires a deterministic fault schedule WHILE the workload runs. For each
    `Fault` it sleeps until its `t_offset` (relative to run start), runs `apply_fault` (which blocks
    for the fault's `duration_s` and brings the node back), then records that a RECOVERY CHECKPOINT is
    due. The main thread, between ops, drains the workload and runs the recovery checkpoint -- so the
    checkpoint never races a fault and the workload is paused only for the checkpoint itself.

    `apply_fault` is run HERE (not on a workload thread): it blocks for `duration_s`, and a worker
    blocking that long would stall the whole workload."""

    def __init__(self, schedule, *, on_fault_done, stop_event, checkpoint_active):
        super().__init__(daemon=True, name="chaos")
        self.schedule = schedule
        self.on_fault_done = on_fault_done    # callback(fault) -> request a recovery checkpoint
        self.stop_event = stop_event
        # Set by the main thread WHILE a checkpoint is in progress. The chaos thread will not start a
        # new fault while this is set, so a fault never races a quiescing checkpoint (the checkpoint's
        # raw queries assume reachable nodes; mean_interval comfortably exceeds checkpoint time).
        self.checkpoint_active = checkpoint_active
        self.start_monotonic = None
        self.last_fault = None
        self.faults_fired = 0
        self.error = None

    def run(self):
        self.start_monotonic = time.monotonic()
        try:
            for fault in self.schedule:
                # Sleep (interruptibly) until this fault's scheduled offset.
                while not self.stop_event.is_set():
                    elapsed = time.monotonic() - self.start_monotonic
                    if elapsed >= fault.t_offset:
                        break
                    self.stop_event.wait(min(0.5, fault.t_offset - elapsed))
                if self.stop_event.is_set():
                    return
                # Do not fire while a checkpoint is quiescing the cluster -- wait it out.
                while self.checkpoint_active.is_set() and not self.stop_event.is_set():
                    self.stop_event.wait(0.5)
                if self.stop_event.is_set():
                    return
                log(f"CHAOS firing fault #{self.faults_fired + 1} at t+{fault.t_offset}s: "
                    f"{fault.target.value} {fault.action.value} dur={fault.duration_s}s")
                apply_fault(fault)   # blocks for duration_s; brings node(s) back
                self.last_fault = fault
                self.faults_fired += 1
                log(f"CHAOS fault window complete: {fault.target.value} {fault.action.value} "
                    f"-> requesting recovery checkpoint")
                self.on_fault_done(fault)
        except Exception as e:   # noqa: BLE001 -- surface chaos-thread errors to the main thread
            self.error = e
            log(f"CHAOS thread error: {type(e).__name__}: {e}\n{traceback.format_exc()}")


def write_failure(path, *, seed, base_time, op_id, phase, model_expected, n1, n2, fsck, last_op,
                  error=None, chaos_seed=None, last_fault=None):
    payload = {
        "seed": seed,
        "chaos_seed": chaos_seed,
        "base_time": base_time,
        "op_id": op_id,
        "phase": phase,
        "error": error,
        "last_fault": last_fault,
        "model_expected": model_expected,
        "node1": n1,
        "node2": n2,
        "fsck": {k: v for k, v in (fsck or {}).items() if k != "stdout"},
        "last_op": last_op,
    }
    with open(path, "w") as fh:
        json.dump(payload, fh, indent=2, default=str)
    return payload


def _last_fault_dict(chaos):
    if chaos is None or chaos.last_fault is None:
        return None
    f = chaos.last_fault
    return {"t_offset": f.t_offset, "target": f.target.value, "action": f.action.value,
            "duration_s": f.duration_s}


def main(argv=None):
    ap = argparse.ArgumentParser(description="CA soak green-path / chaos driver")
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
    # --- Phase-2 chaos args ------------------------------------------------------------------
    ap.add_argument("--chaos-seed", type=int, default=None,
                    help="(phase 2) seed for the deterministic fault schedule (generate_chaos_schedule).")
    ap.add_argument("--chaos-interval", type=int, default=90,
                    help="(phase 2) mean inter-fault interval (s). MUST comfortably exceed the "
                         "checkpoint+recovery time so quiescence is reachable between faults.")
    ap.add_argument("--duration", type=int, default=None,
                    help="(phase 2) wall-clock seconds to generate faults over. Defaults to a generous "
                         "estimate from --ops; the chaos thread is stopped as soon as the workload finishes.")
    args = ap.parse_args(argv)

    phase2 = args.phase == 2
    if phase2 and args.chaos_seed is None:
        args.chaos_seed = args.seed

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

    driver = Driver(cluster, model, args.seed, base_time, args.workers, insert_mode=args.insert_mode,
                    transport_resilient=phase2)
    log(f"insert_mode={args.insert_mode} insert_settings_suffix={INSERT_MODE_SETTINGS[args.insert_mode]!r} "
        f"transport_resilient={phase2}")

    # --- Phase-2 chaos wiring ----------------------------------------------------------------
    chaos = None
    chaos_stop = threading.Event()
    checkpoint_active = threading.Event()
    recovery_lock = threading.Lock()
    recovery_pending = []   # list of Faults whose windows completed; main thread drains it at checkpoints

    if phase2:
        # Generous wall-clock estimate: the chaos thread is stopped the moment the workload finishes,
        # so this only needs to outlast the workload. Tie it to ops with slack.
        duration_s = args.duration if args.duration is not None else max(600, args.ops * 2)
        schedule = generate_chaos_schedule(args.chaos_seed, duration_s, args.chaos_interval)
        log(f"chaos schedule: {len(schedule)} faults over {duration_s}s "
            f"(chaos_seed={args.chaos_seed} mean_interval={args.chaos_interval}s)")

        def on_fault_done(fault):
            with recovery_lock:
                recovery_pending.append(fault)

        chaos = ChaosRunner(schedule, on_fault_done=on_fault_done, stop_event=chaos_stop,
                            checkpoint_active=checkpoint_active)
        chaos.start()

    def do_checkpoint(label, executed_count, op_id):
        log(f"{label} at executed={executed_count} (op_id={op_id})")
        # Hold off chaos while the checkpoint quiesces (its raw queries assume reachable nodes). In
        # phase 2 a fault may already be mid-window when we reach a periodic checkpoint, so wait for
        # both nodes HTTP-healthy first (loud fail if a node never returns).
        checkpoint_active.set()
        try:
            if phase2:
                wait_for_healthy(cluster)
                # Gate on object-store/pool recovery too (B144): a fault that hit RustFS leaves a
                # transient post-restart window where a fresh fsck mount sees recently-written trees
                # as HEAD-absent (dangling) and the GC/fsck views are incoherent. Wait for the pool to
                # reach a coherent cut (fsck dangling==0) BEFORE the hard dryrun-subset assert; a
                # PERSISTENT dangling>0 past the bound is escalated as a REAL durability finding.
                wait_for_pool_consistent(lambda: run_fsck(FSCK_CONTAINER))
            now, exp, n1, n2, f, dr = checkpoint(driver, cluster, model, args.phase)
        finally:
            checkpoint_active.clear()
        log(f"{label} OK: now={now} count={exp['count']} "
            f"fsck reachable={f.get('reachable')} unreachable={f.get('unreachable')} "
            f"(M-F debris, B140) dangling={f.get('dangling')} dryrun_count={dr.get('count')}")
        return now, exp, n1, n2, f, dr

    def drain_recovery_checkpoints(executed_count, op_id):
        """If chaos completed any fault windows, run a RECOVERY checkpoint for them: wait for both
        nodes HTTP-healthy (loud fail if a restarted node never returns), then the full scoped
        checkpoint (workload already drained inside `checkpoint`)."""
        with recovery_lock:
            pending = list(recovery_pending)
            recovery_pending.clear()
        if not pending:
            return
        # Surface any chaos-thread error immediately.
        if chaos is not None and chaos.error is not None:
            raise chaos.error
        last = pending[-1]
        log(f"RECOVERY: {len(pending)} fault window(s) completed; last="
            f"{last.target.value}/{last.action.value}/{last.duration_s}s — recovery checkpoint")
        # do_checkpoint pauses the workload (drain inside `checkpoint`) and, in phase 2, waits for
        # both nodes HTTP-healthy before quiescing (loud fail if a restarted node never returns).
        do_checkpoint("recovery checkpoint", executed_count, op_id)

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

            # Phase 2: run a recovery checkpoint after any completed fault window (pauses workload).
            if phase2:
                drain_recovery_checkpoints(executed, op.op_id)

            if executed % args.checkpoint_every == 0:
                do_checkpoint("checkpoint", executed, op.op_id)

        # Phase 2: stop chaos and drain any final recovery checkpoints before the final checkpoint.
        if phase2:
            chaos_stop.set()
            chaos.join(timeout=30)
            drain_recovery_checkpoints(executed, last_op.op_id if last_op else None)
            if chaos is not None and chaos.error is not None:
                raise chaos.error

        # Final checkpoint (if the last one didn't land exactly on a boundary). do_checkpoint waits
        # for HTTP-healthy in phase 2 and drains the workload inside `checkpoint`.
        if executed == 0 or executed % args.checkpoint_every != 0:
            do_checkpoint("final checkpoint", executed, last_op.op_id if last_op else None)

    except (CheckpointFailure, QueryError, OSError) as e:
        # CheckpointFailure -> a model/replica divergence or a dirty CA pool at a quiesced checkpoint
        #   (in phase 2, this is post-recovery -> a crash-recovery invariant violation).
        # QueryError -> a worker-side cluster error during op execution (e.g. a write-path race).
        # OSError (incl. socket TimeoutError) -> a transport-level failure talking to a replica that
        #   the bounded retry/reroute never recovered from (in phase 2: a node that never came back).
        # In every case record a structured failure with the reproducer instead of a raw traceback.
        chaos_stop.set()
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
            model_expected=exp, n1=n1, n2=n2, fsck=f, last_op=last_op_d, error=f"{kind}: {e}",
            chaos_seed=args.chaos_seed, last_fault=_last_fault_dict(chaos))
        print(f"{kind}: {e}", file=sys.stderr)
        print(json.dumps(payload, indent=2, default=str), file=sys.stderr)
        driver.executor.shutdown(wait=False, cancel_futures=True)
        sys.exit(1)
    finally:
        chaos_stop.set()
        if chaos is not None:
            chaos.join(timeout=30)
        driver.executor.shutdown(wait=True)

    log(f"ABORTED-retried INSERT attempts (B137 race fired and was handled): {driver.aborted_retries}")
    if phase2:
        log(f"transport-retried op attempts (chaos node-down fired and was handled): "
            f"{driver.transport_retries}; faults fired: {chaos.faults_fired if chaos else 0}")
        print("PHASE2 OK")
    else:
        print("PHASE1 OK")
    return 0


if __name__ == "__main__":
    main()
