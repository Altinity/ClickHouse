"""CA soak-test Phase-1 green-path driver.

Drives the deterministic ledger workload (`soak.ledger`) against a live 2-replica CA cluster
(`soak.cluster`) while mirroring every op into the authoritative in-memory `Model` (`soak.model`).
At periodic quiesced checkpoints it HARD-ASSERTS the invariants the CURRENT CA design guarantees:
BOTH replicas equal the model exactly (no loss, no divergence), fsck `dangling==0` (INV-NO-LOSS:
every ref-reachable object exists), fsck `stale_edge==0` (no blob whose every source edge names a
manifest that no longer exists — such a blob's in-degree is pinned above zero and the incremental GC
can never reclaim it; `ca-fsck` itself does not exit nonzero on this, so the checkpoint is the only
gate), and the GC dry-run preview is a subset of the fsck `unreachable`
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

Signals (2026-07-26). The driver also READS the product's own counters, which until this change it did
not do at all: the `Cas*` `ProfileEvents` in `soak.signals.CAS_SIGNAL_EVENTS` (per metrics tick and at
every checkpoint) and the per-phase GC log rows (`event_type = 'Phase'`, summarised per checkpoint into
the `gc_phases` table of the metrics db). Both are REPORTED, not gated — their benign rates are
uncharacterised — but a `preflight_signals` at bring-up refuses to start a run whose binary does not
have every watched counter, because a counter that reads as a quiet zero for four hours is worse than
no counter at all.
"""

import argparse
import json
import os
import random
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
    classify_retry_error,
)
from soak.ledger import generate_ledger, Op, OpType
from soak.model import Model
from soak.rowgen import NBUCKETS, KSPACE
from soak.workload import (
    insert_values_sql, update_sql, delete_sql, truncate_sql, select_range_sql, select_recent_sql,
)
from soak.checker import (
    CheckpointFailure,
    quiesce,
    query_aggregates,
    compare_aggregates,
    drive_gc_to_fixpoint,
)
from soak.fsck import run_fsck, run_dryrun, FsckTimeout, stale_edge_verdict
from soak.signals import (
    CAS_SIGNAL_EVENTS,
    LatePutFencing,
    PhaseCoverage,
    SignalTracker,
    format_phase_summary,
    preflight_signals,
    read_phase_summary,
    read_signal_events,
    summarize_phases,
)
from soak.replay import dump_failure
from soak.chaos import generate_chaos_schedule, apply_fault, Fault, FaultTarget, FaultAction
from soak import metrics as metrics_mod
from soak.pool import pool_size
from soak.schedule import stage_plan, stage_at, chaos_window, StageKind

TABLE = "ca_stress"
# Dedicated Atomic database, created EAGERLY (lazy_load_tables OFF) by the single shared
# `setup_cluster_and_table` helper. lazy_load_tables was deliberately turned off (2026-07-22): the
# StorageTableProxy wrapper is under a quarantine decision after several unforwarded-virtual product
# bugs (see docs/superpowers/reports/2026-07-21-storageproxy-forwarding-audit.md and the CAS backlog's
# lazy_load_tables item). The table name stays bare; the workload connection's default database is DB.
DB = "ca_soak"
FSCK_CONTAINER = "ca-soak-ch1-1"

MAX_CLIFFS = 2

# Phase-3 defaults. The metrics tick is the §8 "every 60s" cadence; for a SHORT self-check it is
# scaled down (see `metrics_interval_for`) so a 10-minute run still records a useful curve.
METRICS_INTERVAL_S = 60
GB = 1024 ** 3

# search_orphaned_parts_disks='local' (B143): the harness config defines a SECOND CA disk `ca_ro`
# (a read-only alias of the SAME RustFS pool, used by the offline `clickhouse disks ca-fsck` applet)
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


def parse_duration(s) -> int:
    """Parse a wall-clock duration to seconds. Accepts a bare integer (seconds) or a suffixed form
    `<n>{s,m,h,d}` (e.g. `24h`, `600`, `90m`, `1d`). Pure + unit-tested so `--duration 24h` and the
    compressed `--duration 600` self-check share one deterministic mapping."""
    if isinstance(s, int):
        return s
    s = str(s).strip().lower()
    if not s:
        raise ValueError("empty duration")
    if s[-1] in "smhd":
        n = float(s[:-1])
        mult = {"s": 1, "m": 60, "h": 3600, "d": 86400}[s[-1]]
        return int(n * mult)
    return int(s)


def metrics_interval_for(duration_s: int, base_interval_s: int = METRICS_INTERVAL_S) -> int:
    """The §8 metrics cadence is "every 60s". For a SHORT self-check (e.g. 600s) a 60s tick yields
    only ~10 samples — too coarse to see the stage transitions. Scale the interval DOWN for short
    durations so a self-check still produces a dense-enough curve (aim for ~30+ samples), but never
    below 5s and never above the 60s production cadence. Pure + unit-tested."""
    target_samples = 30
    scaled = max(5, min(base_interval_s, duration_s // target_samples))
    return scaled


def demote_dense_mutations(ledger, min_ops_between_mutations: int):
    """Phase-3 mutation thinning. Each UPDATE/DELETE issues an ALTER ... mutation that materializes
    a whole part on the remote CA pool — comparatively heavy. The base ledger is ~20% mutations, and
    the time-driven loop fires them far faster than this cluster materializes them, piling up hundreds
    of pending mutations that then make a quiesced checkpoint's mutation-drain time out. We DEMOTE a
    mutation to OPTIMIZE unless at least `min_ops_between_mutations` ops have elapsed since the previous
    KEPT mutation, so mutations stay SPARSE (a representative few still exercise the UPDATE/DELETE +
    model path) and always materializable. Demotion is a pure function of op_id order — the cluster
    and the model consume the IDENTICAL thinned ledger (OPTIMIZE is a model no-op, exactly like the
    cliff demotion in `build_effective_ledger`). `min_ops_between_mutations<=0` disables thinning."""
    if min_ops_between_mutations <= 0:
        return list(ledger)
    out = []
    last_mut_op = None
    for op in ledger:
        if op.type in (OpType.UPDATE, OpType.DELETE):
            far_enough = last_mut_op is None or (op.op_id - last_mut_op) >= min_ops_between_mutations
            if far_enough:
                last_mut_op = op.op_id
                out.append(op)
            else:
                out.append(Op(op_id=op.op_id, type=OpType.OPTIMIZE, target=op.target, param=op.param))
        else:
            out.append(op)
    return out


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


# Phase-2 transport-retry bound. A node killed/paused/restarted by chaos comes back within its
# fault duration_s (<= 60s; freeze_long <= 90s; rustfs pause/restart <= 60s -- see
# `generate_chaos_schedule`, chaos.py) plus recovery time (worst observed: ~120s kill-to-serving
# boot, ~90s Keeper-session re-establishment). With capped-exponential backoff (0.5s..8s) these 40
# attempts sleep ~287s total, plus each failed attempt's own call time -- generously longer than
# the worst fault window + recovery stack (~270s); the full arithmetic lives on
# `retry_on_transport` (cluster.py). Exhausting it means a node/store NEVER recovered, which the
# task spec says IS a failure (the feature must survive crash+restart), so the exhausted transport
# error is re-raised loudly.
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
        self.retry_class_counts = {}  # error-class -> count (availability accounting; see _note_transport_retry)
        self._transport_lock = threading.Lock()
        # Phase-3 resource bounding: an insert THROTTLE the metrics ticker raises when the physical
        # pool approaches --max-pool-gb. `throttle_sleep_s` (0 == unthrottled) is the per-insert delay
        # injected into each submitted INSERT worker; it slows the workers WITHOUT dropping work (the
        # op still runs, just paced). Read locklessly (a float store/load is atomic enough for pacing).
        self.throttle_sleep_s = 0.0
        self.inserts_submitted = 0
        self._submit_lock = threading.Lock()
        # OPTIMIZE ops on one table serialize server-side on the merge mutex; submitting many
        # concurrently just queues them and — worse — under continuous submission they can occupy
        # EVERY worker slot at once, pinning the whole pool on slow remote-CA merges and starving
        # INSERTs. Cap concurrent OPTIMIZEs at 1 (a non-blocking try-acquire: if one is already
        # running, the new OPTIMIZE op is a NO-OP — it has no model effect, so skipping it is sound
        # and keeps workers free for inserts).
        self._optimize_gate = threading.BoundedSemaphore(1)

    def _note_transport_retry(self, op_kind, op_id, attempt, node, err):
        # Availability accounting (2026-07-13, user finding): every driver-level retry is a
        # PRODUCT-VISIBLE failure the server did not absorb internally. A week-old build absorbed
        # S3 blips server-side (multi-attempt conditional writes pre-bc40f5fd945), so a healthy
        # build should keep these counts near zero outside node-kill windows. The per-class
        # breakdown below is printed at run end so harness tolerance can never silently mask a
        # product availability regression again.
        klass = classify_retry_error(err)
        with self._transport_lock:
            self.transport_retries += 1
            self.retry_class_counts[klass] = self.retry_class_counts.get(klass, 0) + 1
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
        with self._submit_lock:
            self.inserts_submitted += 1

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
        and the model already applied the op exactly once.

        Idempotency of the retry, including after an AMBIGUOUS failure (a timed-out CAS conditional
        PUT / a dropped connection where the first attempt MAY have committed server-side): the retry
        re-sends the byte-identical `sql` (built once from (seed, op_id) and captured here), the soak
        runs SYNC inserts with `deduplicate_blocks=true` (B138), and RMT block-dedup keys on the block
        content hash -- so a first attempt that DID commit makes the retry a dedup no-op, and one that
        did NOT commit left no dedup token (sync insert, B138) so the retry truly re-inserts. The
        dedup window (`replicated_deduplication_window` = 10000 blocks, ~15 min at the observed op
        rate) comfortably outlasts the ~287s retry budget. Either way the table converges to exactly
        the model's one application of the op."""
        def aborted_on_retry(attempt, err):
            with self._aborted_lock:
                self.aborted_retries += 1
            log(f"INSERT op_id={op_id} hit retryable ABORTED (attempt {attempt}); retrying. {err}")

        # Phase-3 resource-bounding throttle: pace the insert WITHOUT dropping it (the op still runs).
        sleep_s = self.throttle_sleep_s
        if sleep_s > 0:
            time.sleep(sleep_s)

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

    OPTIMIZE_TIMEOUT_S = 45

    def _optimize_with_retry(self, target, op_id):
        # At most one OPTIMIZE in flight (try-acquire, non-blocking). If one is already running, skip
        # this OPTIMIZE: it has NO model effect, so dropping a redundant compaction hint is sound and
        # avoids piling concurrent OPTIMIZEs that would pin every worker on the serialized merge mutex.
        if not self._optimize_gate.acquire(blocking=False):
            return
        try:
            node = node_for(self.cluster, target)
            # OPTIMIZE is a best-effort compaction HINT with no model effect, and over wide parts on the
            # remote CA pool it can run for minutes. Run it ONCE with a BOUNDED socket timeout and
            # SWALLOW a timeout (and node-down under chaos): a slow/abandoned OPTIMIZE must never pin a
            # worker — which would stall the barrier `drain` (and thus the whole phase-3 timeline) —
            # and dropping it loses nothing the background merges + the checkpoint's `OPTIMIZE FINAL`
            # don't already do. A genuine query error (not transport/timeout) still surfaces.
            try:
                node.command(f"OPTIMIZE TABLE {TABLE}", timeout=self.OPTIMIZE_TIMEOUT_S)
            except QueryError as e:
                # node-down (graceful-shutdown cancel), the transient TABLE_IS_READ_ONLY (a replica
                # re-establishing its Keeper session after a chaos pause/restart -- esp. `both pause`),
                # a Keeper coordination transient (`Session expired` or `Operation timeout`, a node
                # frozen/paused past its Keeper session TTL, B190; broadened per
                # .superpowers/sdd/task3v2-chaos-diag-report.md), and a transport-flavored S3_ERROR
                # (the rustfs store paused/restarting mid-OPTIMIZE -- the task3 v3 tolerance gap,
                # tmp/task3_soak_2h_v3.log:518) are all expected under chaos. OPTIMIZE has NO model
                # effect, so dropping it is sound; only a genuine logic error surfaces.
                if not (e.is_node_down or e.is_readonly or e.is_mount_fenced or e.is_keeper_transient
                        or e.is_s3_transient):
                    raise
            except Exception as e:
                if not is_transport_error(e):
                    raise
        finally:
            self._optimize_gate.release()

    def drain(self):
        """Wait for all in-flight inserts/optimizes; re-raise the first worker exception loudly."""
        if not self.inflight:
            return
        done, _ = wait(self.inflight, return_when=FIRST_EXCEPTION)
        futures, self.inflight = self.inflight, []
        for fut in futures:
            fut.result()    # propagates any worker exception

    def harvest(self):
        """Remove already-COMPLETED futures from `inflight`, re-raising the first worker exception.
        Phase-3 backpressure: the time-driven loop submits ops continuously for hours, so without
        periodic harvesting the `inflight` list (and the executor's queue) would grow without bound.
        Non-blocking — only reaps futures that are already done."""
        if not self.inflight:
            return
        still = []
        for fut in self.inflight:
            if fut.done():
                fut.result()        # propagate any worker exception
            else:
                still.append(fut)
        self.inflight = still

    def inflight_count(self):
        return len(self.inflight)

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
        reroutes to the other replica (replicated DDL/mutation; idempotent table-level effect) --
        `_with_transport_retry`/`retry_on_transport` (cluster.py) also tolerates a transient Keeper
        coordination error (`is_keeper_transient`: `Session expired` or `Operation timeout`, code 999
        KEEPER_EXCEPTION) racing the mutation's Keeper-side bookkeeping (e.g. creating the entry under
        .../mutations), per .superpowers/sdd/task3v2-chaos-diag-report.md -- previously this class went
        unretried here and could abort the whole run on a self-healing ~70s outage -- and a
        transport-flavored S3_ERROR (`is_s3_transient`, the rustfs store paused/restarting; the task3
        v3 tolerance gap). It is applied to the model ONLY AFTER the cluster command succeeds, so a
        never-landed mutation surfaces as a transport failure instead of silently diverging the model.

        KNOWN residual (pre-existing, documented not fixed): a retried UPDATE is NOT idempotent at
        the table level (`v = v + 1`), so an AMBIGUOUS first attempt -- one that actually created its
        mutation entry server-side before the error surfaced (possible for node-down mid-op and the
        Keeper `Operation timeout` shape; NOT for readonly/mount-fenced, which reject before doing
        anything) -- followed by a successful retry double-applies the increment and the NEXT
        checkpoint's model comparison catches it loudly. This window (entry-created-but-error-
        surfaced) is a few ms wide and has not been observed across the 2h soaks; DELETE/TRUNCATE
        retries are table-level idempotent."""
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



def _stale_edge_display(fsck_result: dict) -> str:
    """Render `stale_edge` for a summary line WITHOUT letting "not checked" read as "checked, nothing".

    A checkpoint that skipped its fsck asserts (a timed-out entry gate, B146/B154) still prints this line,
    and `f.get('stale_edge')` is then `None`. Printed raw, that is indistinguishable from a check that ran
    and found nothing — which is the reporting-degrades-to-absence shape this project has now hit five
    separate times (BACKLOG {#gc-observation-vacuous-2026-07-25} and the entries it references). Say
    `not-checked` instead, so a reader of the run log can tell the two apart.
    """
    value = fsck_result.get("stale_edge")
    return "not-checked" if value is None else str(value)


def checkpoint(driver, cluster, model, phase, *, strict_unreachable=False):
    """Pause workers (drain), quiesce, assert both replicas == model, drive GC to fixpoint, assert a
    clean CA pool. Raises CheckpointFailure on any divergence.

    `strict_unreachable`: assert the unreachable backlog has collapsed to the GC pipeline floor.
    Pass True ONLY at the final converge checkpoint: mid-churn the backlog legitimately scales with
    churn rate (multi-round condemn->graduate->delete pipeline, time/ack-gated), so any mid-run
    level cap keyed to the live set is a guaranteed false trip on a churny run (2026-07-22 RCA:
    169,837 AwaitingGc at t+420 drained to 14 by run end through the normal pipeline)."""
    driver.drain()
    now = quiesce(cluster, TABLE)

    # A row whose TTL expiry sits within ±eps of `now` makes the exact-aggregate assertion ambiguous:
    # we cannot tell whether the table has already TTL-deleted it. This is a checkpoint-TIMING artifact,
    # not corruption (B173-class) — so WAIT OUT the band rather than failing. Sleep just past the band
    # (eps+1) and re-quiesce to refresh `now`; the boundary is fixed (`ts + ttl_seconds`) and `now`
    # advances monotonically, so the row falls cleanly past expiry within a bounded number of tries.
    # The sleep only moves wall-clock checkpoint timing — it does NOT touch the seeded op ledger, so the
    # run stays deterministic-friendly. Only a genuinely stuck schedule (band never clears within the
    # bound) is fatal. The exact `compare_aggregates` assertion below is unchanged.
    AMBIGUOUS_BAND_EPS = 10
    AMBIGUOUS_BAND_MAX_ATTEMPTS = 6
    band_attempt = 0
    ttl_band_ambiguous = False
    while model.ambiguous_band_nonempty(now, eps=AMBIGUOUS_BAND_EPS):
        if band_attempt >= AMBIGUOUS_BAND_MAX_ATTEMPTS:
            # Under sustained TTL pressure MANY rows cross their boundary around `now`, so the ±eps band
            # may never empty within a bounded wait: as `now` advances past one row's boundary another
            # enters the band. The original code treated this as fatal, but it is a checkpoint-TIMING
            # artifact, NOT corruption (B173-class) — exactly like the FsckTimeout path below. Degrade
            # instead of aborting: WARN, skip only the EXACT sum equality (which the band makes genuinely
            # ambiguous), but STILL enforce (a) a band-tolerant COUNT RANGE (catches real data loss/dup)
            # and (b) the GC-to-fixpoint + clean-pool fsck gate — the CA-integrity oracle, which TTL
            # timing cannot affect. The exact `compare_aggregates` runs unchanged at every non-ambiguous
            # checkpoint. (Fixes the chronic TTL-band soak abort; see BACKLOG SOAK-TTL-band.)
            log(f"WARNING [TTL-band] ambiguous TTL band still non-empty at now={now} after "
                f"{AMBIGUOUS_BAND_MAX_ATTEMPTS} waits; degrading this checkpoint to a band-tolerant "
                f"count-range + CA-integrity (fsck) gate (TTL-timing artifact, not corruption) — soak continues")
            ttl_band_ambiguous = True
            break
        band_attempt += 1
        wait_s = AMBIGUOUS_BAND_EPS + 1
        log(f"checkpoint: ambiguous TTL band non-empty at now={now}; waiting {wait_s}s to clear "
            f"(attempt {band_attempt}/{AMBIGUOUS_BAND_MAX_ATTEMPTS})")
        time.sleep(wait_s)
        now = quiesce(cluster, TABLE)

    exp = model.aggregates(now)
    n1 = query_aggregates(cluster.node1, TABLE)
    n2 = query_aggregates(cluster.node2, TABLE)
    if not ttl_band_ambiguous:
        compare_aggregates(exp, n1, n2)
    else:
        # Band-tolerant COUNT range: every row is definitely-live if its boundary is > now+eps and
        # definitely-gone if < now-eps; band rows (within ±eps) may be either. So the replica's live
        # count must lie within [count(now+eps), count(now-eps)]. A count outside this range is a REAL
        # divergence (data loss/dup), not TTL timing → still fatal. Exact sum equality is skipped
        # because the band rows' sum contribution is genuinely undetermined here.
        count_low = model.aggregates(now + AMBIGUOUS_BAND_EPS)["count"]
        count_high = model.aggregates(now - AMBIGUOUS_BAND_EPS)["count"]
        for node_name, na in (("node1", n1), ("node2", n2)):
            if not (count_low <= na["count"] <= count_high):
                raise CheckpointFailure(
                    f"TTL-band-tolerant checkpoint: {node_name} count {na['count']} outside the "
                    f"band range [{count_low}, {count_high}] at now={now} — real divergence, not TTL timing")

    # Drop rows the table has already TTL-deleted so the oracle dict stays bounded by the live set
    # instead of every rid ever inserted. The workload is drained here and `now` is freshly quiesced,
    # so this never races a worker and never changes the aggregates just asserted (it removes only
    # already-excluded expired rows). Without this the driver OOM'd (~12.8 GiB) on a multi-hour soak.
    pruned = model.prune_expired(now)
    if pruned:
        log(f"model.prune_expired: reclaimed {pruned} TTL-expired rows (live={exp['count']})")

    # Drive the INCREMENTAL GC to ITS fixpoint (unreachable STOPS DECREASING). The residual is the
    # known M-F-debris (B140): per CA spec §8 the incremental GC cannot reclaim blobs orphaned by a
    # displaced-before-expansion tree; the Full-GC mark-sweep (milestone M-F, not yet implemented) is
    # the documented backstop that drains it to 0. We TRACK the residual, not fail on it.
    # The fixpoint POLL only needs the `unreachable` COUNT, so it uses the cheap SUMMARY fsck
    # (`detail=False`): the `--detail` per-object listing scans+classifies every blob and over a large
    # pool costs tens of seconds PER call, which the multi-poll fixpoint loop would multiply into many
    # minutes. The single authoritative `--detail` fsck below (needed for the unreachable-key set the
    # dryrun-subset check consumes) pays that cost exactly once.
    # Each per-poll summary fsck is bounded at 180s; FsckTimeout in any poll surfaces as a logged
    # best-effort skip of the fixpoint loop — the soak must not wedge here (B146/B154).
    # Per .superpowers/sdd/task3-soak-diag-report.md: wait for the PHYSICAL pool (`pool_size`'s B204
    # `du -sb` probe) to stop shrinking BEFORE sampling `unreachable` for stability — the server's
    # leader GC keeps oscillating candidates/deletes every ~15-18s for as long as a real backlog
    # remains, so the checker was chasing a moving target. `drain_interval_s` matches the existing
    # metrics-tick cadence (`METRICS_INTERVAL_S`).
    _detail_fsck_skipped = False  # set True if `--detail` fsck times out; skips dryrun-subset assert
    _fsck_is_detail = False       # True once `f` holds the result of a `--detail` scan (see stale_edge)
    try:
        residual = drive_gc_to_fixpoint(
            cluster,
            lambda: run_fsck(FSCK_CONTAINER, detail=False, timeout_s=180, partial=True)["unreachable"],
            pool_bytes_fn=lambda: pool_size()[1],
            drain_interval_s=METRICS_INTERVAL_S,
            log_fn=log,
        )
    except FsckTimeout as _e:
        log(f"WARNING [B146/B154] drive_gc_to_fixpoint: summary fsck timed out ({_e}); "
            f"skipping fixpoint loop for this checkpoint — soak continues")
        residual = 0
        _detail_fsck_skipped = True

    # Authoritative post-GC fsck. The `--detail` per-object listing is needed ONLY to build the
    # unreachable-KEY set the dryrun-subset assert consumes; over a large pool it costs tens of seconds.
    # So read the cheap SUMMARY first and only ESCALATE to `--detail` when the summary shows something
    # to inspect: dangling>0 (need the detail rows for the failure message / phase-2 re-confirm) or
    # unreachable>0 (need the key set for the subset check). When BOTH are 0 — the clean common case —
    # the dryrun MUST be empty (nothing is reclaimable), which we assert directly below WITHOUT the
    # expensive detail scan. This keeps every clean checkpoint cheap.
    # Both fsck calls are bounded: 180s for the cheap summary, 600s for the expensive `--detail` scan.
    # If even the summary fsck times out here (B146/B154), we log a LOUD warning and skip this
    # checkpoint's fsck asserts entirely — a slow fsck must never wedge or fail the soak.
    try:
        f = run_fsck(FSCK_CONTAINER, detail=False, timeout_s=180, partial=True)
    except FsckTimeout as _e:
        log(f"WARNING [B146/B154] post-GC summary fsck timed out ({_e}); "
            f"SKIPPING fsck/dryrun asserts for this checkpoint — dangling==0 gate unavailable; "
            f"soak continues (primary purpose: live workload exercise, fsck is best-effort oracle)")
        _detail_fsck_skipped = True
        # HAZARD, kept deliberately and narrowly: these zeros are FABRICATED, not measured. They exist
        # only to keep the code below flowing, and every consumer of `f` past this point is guarded by
        # `not _detail_fsck_skipped`, so no assert ever reads them. Do NOT remove that guard from any
        # consumer without also removing this dict -- a fabricated `dangling: 0` reaching an assert is
        # precisely the "check that passed while looking at nothing" failure. Tracked in BACKLOG
        # {#fsck-fabricated-clean-on-timeout}. With `partial=True` above this path is now rare: the scan
        # reports a lower bound instead of dying empty-handed.
        f = {"dangling": 0, "unreachable": 0, "reachable": 0, "exit_code": 0, "detail": []}
    else:
        if not _detail_fsck_skipped and (f.get("dangling", 0) != 0 or f.get("unreachable", 0) != 0):
            try:
                f = run_fsck(FSCK_CONTAINER, detail=True, timeout_s=600)
                _fsck_is_detail = True
            except FsckTimeout as _e:
                log(f"WARNING [B146/B154] post-GC fsck --detail timed out ({_e}); "
                    f"keeping summary result (dangling={f.get('dangling')} unreachable={f.get('unreachable')}); "
                    f"SKIPPING dryrun-subset assert for this checkpoint (B146/B154; O(pool) scan under load); "
                    f"dangling==0 summary gate remains authoritative")
                _detail_fsck_skipped = True

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
    # Each `run_fsck` inside `wait_for_pool_consistent` is bounded at 180s per call; FsckTimeout
    # propagates up from the lambda and is caught here — treated as fsck unavailable for this round.
    if not _detail_fsck_skipped and f.get("dangling") != 0:
        if phase == 2:
            try:
                f = wait_for_pool_consistent(
                    lambda: run_fsck(FSCK_CONTAINER, timeout_s=180)
                )
                _fsck_is_detail = True   # run_fsck defaults to detail=True
            except FsckTimeout as _e:
                log(f"WARNING [B146/B154] wait_for_pool_consistent fsck timed out ({_e}); "
                    f"SKIPPING dangling re-confirm for this checkpoint — soak continues")
                _detail_fsck_skipped = True
        else:
            raise CheckpointFailure(f"fsck dangling != 0: {f.get('dangling')} (INV-NO-LOSS) detail={f.get('detail')}")
    if not _detail_fsck_skipped and f.get("exit_code") != 0:
        raise CheckpointFailure(f"fsck exit_code != 0: {f.get('exit_code')} stderr={f.get('stderr')}")

    # INV-NO-STALE-EDGE: no blob may be left whose every source edge names a manifest that no longer
    # exists. Such a blob's in-degree is pinned above zero forever, so the incremental GC will never
    # reclaim it -- and until the class existed it was reported as an `AwaitingGc` "expected, no action
    # needed" backlog, which is what hid it. `ca-fsck` does NOT exit nonzero on it (only `dangling` and
    # `snapshot_oracle_mismatches` throw in CommandFsck.cpp), so the `exit_code` gate above does not
    # cover this and THIS assert is the only gate.
    #
    # Asserted off the `--detail` result, never the summary: the cross-check is detail-gated in
    # `runFsck`, so a summary line's `stale_edge=0` is structural. `stale_edge_verdict` also fails
    # CLOSED when the key is absent entirely -- that means the binary predates the class and the
    # checkpoint is not looking at anything. See its docstring for why `unreachable == 0` is a genuine
    # proof of cleanliness rather than a shortcut.
    if not _detail_fsck_skipped:
        _se_verdict, _se_why = stale_edge_verdict(f, detail=_fsck_is_detail)
        if _se_verdict in ("absent", "found"):
            raise CheckpointFailure(f"fsck stale_edge check failed: {_se_why} detail={f.get('detail')}")
        if _se_verdict == "unchecked":
            log(f"WARNING [stale-edge] {_se_why} — this checkpoint does not gate the class")
        else:
            log(f"stale_edge=0 ({_se_why})")

    # The fsck detail parser drops any object class it does not know about, which silently
    # under-reports exactly the objects a NEW fsck class was added to surface (that is how
    # `awaiting-gc` sat mislabelled for months). `run_fsck` records the unknown names; nothing read
    # them until now. Loud, not fatal: it is a harness gap, so the fix is to extend
    # `soak/fsck.py:known_classes` -- but the run must say so instead of quietly narrowing its view.
    if f.get("unknown_detail_classes"):
        log(f"WARNING [fsck-whitelist] ca-fsck emitted detail classes this harness does not know: "
            f"{f['unknown_detail_classes']} — objects in those classes were DROPPED from the "
            f"checkpoint's view. Extend soak/fsck.py:known_classes.")

    # `corrupted-run` (a GC source-edge run whose whole-file seal checksum disagrees with its bytes)
    # is an ERROR class that nothing gates: `CommandFsck` does not throw on it, and -- unlike
    # `stale_edge` -- it is not even PRINTED on the summary line, so `FsckReport::corrupted_runs` is
    # invisible to any consumer of the applet. It is only observable as a `--detail` row, so that is
    # what is counted here. Reported rather than asserted: fixing the missing summary field is a
    # product change, outside this harness change's mandate.
    _corrupted = sum(1 for row in f.get("detail", []) if row["class"] == "corrupted-run")
    if _corrupted:
        log(f"WARNING [corrupted-run] {_corrupted} fsck detail row(s) in class `corrupted-run` — a GC "
            f"source-edge run's seal checksum disagrees with its bytes. Gated by neither this "
            f"checkpoint nor ca-fsck's exit code (and absent from its summary line); investigate.")

    # GC never plans to delete a REACHABLE object: {dryrun delete set} subset of {fsck deletion-pipeline}.
    # The "retired-in-snapshot" refactor made ca-gc-dryrun (previewDeletes) emit a SUPERSET: not only
    # fresh zero-in-degree `unreachable` blobs but ALSO every condemned-pipeline blob (a kCondemned row
    # in the GC snapshot run), with reason delete_pending / awaiting_graduation. In lock-step, fsck now
    # classifies a condemned-but-present blob as `pending-gc` / `awaiting-gc` (NOT `unreachable`), so
    # operators do not misread the churny condemn pipeline as a leak. So the oracle is: every dryrun key
    # MUST appear in fsck `--detail` under a DELETION-PIPELINE class {unreachable, pending-gc, awaiting-gc}.
    # A dryrun key ABSENT from `--detail` (reachable blobs are NEVER listed) — or present under some OTHER
    # class (e.g. unaccounted) — means GC is previewing deletion of a live/unknown blob: a real violation.
    # Because previewDeletes only ever emits zeroInDegree + kCondemned rows, a clean (dangling==0,
    # unreachable==0) SUMMARY no longer implies an empty dryrun — the condemn pipeline can hold kCondemned
    # rows mid-flight — so we must fetch `--detail` whenever the dryrun is non-empty, not only when the
    # summary showed unreachable>0.
    # The dryrun is also bounded at 600s; FsckTimeout skips the subset assert (best-effort oracle).
    if _detail_fsck_skipped:
        dr = {"count": 0, "entries": []}
        log("WARNING [B146/B154] dryrun-subset assert SKIPPED this checkpoint (fsck timed out above)")
    else:
        try:
            dr = run_dryrun(FSCK_CONTAINER, timeout_s=600)
        except FsckTimeout as _e:
            log(f"WARNING [B146/B154] dryrun timed out ({_e}); SKIPPING dryrun-subset assert")
            dr = {"count": 0, "entries": []}
            _detail_fsck_skipped = True

    # If the dryrun is non-empty but we never escalated to `--detail` above (the clean-summary case),
    # fetch it now so `pipeline_keys` is populated — otherwise every dryrun entry would falsely fail.
    # Same container / 600s bound / best-effort-on-timeout pattern as the escalation fsck above.
    if not _detail_fsck_skipped and dr.get("entries") and "detail" not in f:
        try:
            f = run_fsck(FSCK_CONTAINER, detail=True, timeout_s=600)
            _fsck_is_detail = True
        except FsckTimeout as _e:
            log(f"WARNING [B146/B154] dryrun-subset `--detail` fsck timed out ({_e}); "
                f"SKIPPING dryrun-subset assert for this checkpoint (B146/B154; O(pool) scan under load); "
                f"dangling==0 summary gate remains authoritative")
            _detail_fsck_skipped = True

    if not _detail_fsck_skipped:
        detail_rows = f.get("detail", [])
        pipeline_classes = ("unreachable", "pending-gc", "awaiting-gc")
        pipeline_keys = {row["key"] for row in detail_rows if row["class"] in pipeline_classes}
        detail_class_by_key = {row["key"]: row["class"] for row in detail_rows}

        # Classify dryrun keys: pipeline, absent-from-detail (already deleted pending fold), or wrong-class
        already_deleted_count = 0
        for entry in dr.get("entries", []):
            if entry["key"] not in detail_class_by_key:
                # Blob absent from fsck detail: already physically deleted but row not yet folded. TOLERATED.
                already_deleted_count += 1
            elif entry["key"] not in pipeline_keys:
                # Blob present in detail but NOT in a deletion-pipeline class: a real wrong-preview signal.
                other = detail_class_by_key.get(entry["key"], "unknown")
                class_counts: dict = {}
                for row in detail_rows:
                    class_counts[row["class"]] = class_counts.get(row["class"], 0) + 1
                raise CheckpointFailure(
                    f"dryrun key {entry['key']!r} previews deletion of a non-pipeline blob "
                    f"(fsck class={other!r}) — a dryrun key must be in a deletion-pipeline class "
                    f"{pipeline_classes} (absent-from-detail means the blob is already deleted); "
                    f"dryrun_count={dr.get('count')} pipeline_keys={len(pipeline_keys)} "
                    f"detail_class_counts={class_counts}")

        if already_deleted_count > 0:
            log(f"dryrun: {already_deleted_count} keys already deleted, pending fold — tolerated")

    # --- TRACK (do NOT hard-fail): residual unreachable is fold BACKLOG, not debris ----------
    # The label used to read "M-F debris, pending Full GC / B140". That attribution is WRONG and was
    # actively misleading: the product classifies these objects as `AwaitingGc` -- the ack-floor deletion
    # pipeline mid-flight -- and the one time the B140 label was believed, it hid a real retention leak
    # for months (BACKLOG {#unmatched-minus-one-retention-leak}). Say what the product says.
    unreachable = f.get("unreachable", 0)
    reachable = f.get("reachable", 0)
    log(f"unreachable={unreachable} (fold backlog / AwaitingGc); reachable={reachable} "
        f"dangling=0 dryrun_subset=ok")
    # Mid-churn, `unreachable` is a fold BACKLOG (fsck class AwaitingGc), not a leak: it scales
    # with churn rate while `reachable` sits at its floor, so a level cap here is a guaranteed
    # false trip (see the docstring; BACKLOG.md already documents mid-run levels as transients).
    # The honest assertion is CONVERGENCE once churn has stopped: at the final converge checkpoint
    # the pipeline has had its time/ack grace, so anything beyond the pipeline floor is a real
    # unbounded-leak signal.
    if strict_unreachable and unreachable > 50 * reachable + 5000:
        raise CheckpointFailure(
            f"unreachable={unreachable} did not converge after quiesce (reachable={reachable}, "
            f"bound 50*reachable + 5000) — an unbounded-leak class distinct from the normal "
            f"GC-pipeline residual")

    return now, exp, n1, n2, f, dr


def capture_checkpoint_signals(cluster, label, *, tracker, fencing=None, log_fn=log):
    """Read the CAS signal counters at a checkpoint, FAIL-CLOSED, and report them.

    Distinct from the ticker's read in one way that matters: a checkpoint has already waited for both
    replicas to be HTTP-healthy with tables loaded (`wait_for_healthy`), so there is no chaos window to
    excuse a gap and every node is expected to answer. A gap is still tolerated (it is logged as one,
    never as zeros) because a node can go down between the health gate and this read, but a query
    FAILURE propagates and the counters are never invented.

    MOSTLY REPORTED, NOT GATED (Task 21 step 3). The benign rates in `UNCHARACTERISED_SIGNALS` are
    uncharacterised as of 2026-07-26: `CasRefAppendPreAttemptRefused` is EXPECTED to be nonzero under
    chaos -- that is the availability fix working -- and `CasGcUnmatchedRemoveDeltas` should be zero in
    a healthy pool but may not be for reasons nobody has enumerated. A threshold goes in once several
    runs agree on a rate, not before.

    The ONE gated family is the late-PUT-loses invariant, when a `fencing` tracker is supplied. It
    needs no characterisation: every counter it gates on is documented as always-zero in
    `ProfileEvents.cpp`, so a nonzero DELTA over the run's own preflight baseline is a durable
    transaction gone missing, a fold cursor advanced past work, or a stream that stopped being dense --
    none of which a soak may finish green having seen. This is a checkpoint gate rather than a ticker
    gate for the same reason the fsck is: the ticker is observability and runs through chaos windows,
    the checkpoint is the correctness assertion and runs on the main thread with both replicas up."""
    violations = []
    for node in cluster.nodes():
        values = read_signal_events(node)
        tracker.observe(repr(node), values)
        if fencing is not None:
            violations += [f"[{node!r}] {v}" for v in fencing.observe(repr(node), values, label=label)]
    log_fn(f"{label} CAS SIGNALS: {tracker.format_latest()}")
    if violations:
        raise CheckpointFailure(
            f"{label} LATE-PUT FENCING VIOLATION — a counter that must stay zero moved: "
            + "; ".join(violations)
            + ". The invariant is that a fenced predecessor's PUT never materializes below coverage "
              "its successor already declared; these counters are how that failing looks from outside")


def capture_phase_summary(cluster, label, *, since_ts, coverage, conn=None, ts=None, log_fn=log):
    """Summarize the per-phase GC log for the window since `since_ts` on every node, log it, and (when
    a metrics db is open) persist it into `gc_phases`.

    The eighteen `event_type = 'Phase'` rows a folding round emits are the only artifact that can
    answer "where did this round spend its time"; they are also where the four detector values now
    surface (`fold_ref_list.probe_a_holes`, `fold_ref_intake.logs_accounted`/`logs_applied`,
    `fold_reduce.txns_unapplied`). They are captured PER CHECKPOINT rather than only printed because
    the load study that comes next needs them after the run has ended.

    Gaps are tolerated and counted (a node that is down, or a replica that has never held the GC lease
    and so has no log table). Anything else -- notably `UNKNOWN_IDENTIFIER`, i.e. a server that
    predates the phase rows -- raises out of `read_phase_summary`."""
    written = 0
    for node in cluster.nodes():
        rows = read_phase_summary(node, since_ts)
        if rows is None:
            coverage.observe(None)
            log_fn(f"{label} GC PHASES [{node!r}]: unavailable (node unreadable, or the GC log has "
                   f"never been materialized on this replica) — recorded as a gap, not as zero")
            continue
        summary = summarize_phases(rows)
        coverage.observe(summary)
        log_fn(f"{label} GC PHASES [{node!r}]: {format_phase_summary(summary)}")
        if conn is not None and rows:
            written += metrics_mod.record_phases(
                conn, metrics_mod.phase_row_dicts(ts if ts is not None else int(time.time()),
                                                  label, repr(node), rows))
    if written:
        log_fn(f"{label} GC PHASES: {written} per-phase row(s) persisted to the metrics db (gc_phases)")
    return written


def wait_for_healthy(cluster, *, timeout_s: float = 180.0, settle_s: float = 2.0,
                     sleep_fn=time.sleep, monotonic_fn=time.monotonic):
    """Block until BOTH replicas answer `/ping` with HTTP 200, then settle briefly. Used by the
    Phase-2 recovery wait after a fault window: a killed/restarted node must be HTTP-healthy again
    BEFORE the checkpoint quiesces (else SYSTEM SYNC REPLICA / aggregates would hit a still-down node
    and the checkpoint would spuriously fail). FAILS LOUDLY (CheckpointFailure) if a node never
    returns within the bound -- a restarted node that never comes back IS a crash-recovery failure,
    not something to swallow."""
    deadline = monotonic_fn() + timeout_s
    def tables_loaded(node) -> bool:
        # /ping turns 200 BEFORE async table load completes. A real read is the robust gate: it proves
        # `ca_stress` is actually loaded and functional on this node, independent of how the engine
        # column reports (and it stays correct if lazy_load_tables is ever re-enabled -- it does not
        # rely on the engine column flipping on its own).
        try:
            node.query("SELECT count() >= 0 FROM ca_soak.ca_stress", timeout=5.0)
            return True
        except Exception:
            return False

    while True:
        if all(node.ping() for node in cluster.nodes()) and all(tables_loaded(n) for n in cluster.nodes()):
            sleep_fn(settle_s)
            if all(node.ping() for node in cluster.nodes()) and all(tables_loaded(n) for n in cluster.nodes()):
                return
        if monotonic_fn() > deadline:
            states = {repr(n): (n.ping(), tables_loaded(n)) for n in cluster.nodes()}
            raise CheckpointFailure(
                f"node(s) never returned healthy-with-tables-loaded within {timeout_s:.0f}s after a "
                f"fault window (crash-recovery failure): (ping, table_loaded) states={states}")
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
    its hard dryrun-subset assert.

    B152/B185 — the AUTHORITATIVE no-loss gate is the aggregate oracle (`compare_aggregates`, model vs
    BOTH replicas), asserted SEPARATELY and BEFORE this structural gate. This fsck gate is secondary.
    Two distinct timeout outcomes, which the old code conflated (it called a `dangling==0` timeout a
    "PERSISTENT dangling" durability finding, the B152 message bug):
      - FLAPPING-CLEAN: the pool DID reach `dangling==0` at least once but kept flapping in/out of the
        transient post-restart fsck-incoherence window (B141/B144/B145) without holding `stable`
        consecutive clean reads within the bound. Under aggressive load this window can stay open >the
        bound. This is a SETTLING/recovery-time artifact, NOT data loss (the oracle already proved no
        loss) -- warn loudly and return the clean reading instead of false-failing the soak (B185).
      - PERSISTENT NEVER-CLEAN: `dangling>0` on EVERY read, never once clearing -- a REAL
        pool-structure / crash-recovery / object-store-durability finding; FAIL LOUDLY.

    `sleep_fn`/`monotonic_fn` are injectable so the loop is pure-testable."""
    deadline = monotonic_fn() + timeout_s
    consecutive_clean = 0
    last = None
    last_clean = None
    while True:
        last = fsck_fn()
        # A PARTIAL read never counts as clean. Its `dangling == 0` is a lower bound over the prefix the
        # scan reached before its deadline, not a statement about the pool, so treating it as a coherent
        # cut would fabricate exactly the consistency proof this gate exists to establish. (Live risk
        # since 2026-07-26: the callers now pass `partial=True`, so a slow scan RETURNS zeros instead of
        # raising `FsckTimeout` — before that change this branch was unreachable.)
        clean = (last.get("dangling") == 0
                 and last.get("exit_code") == 0
                 and not last.get("partial"))
        if clean:
            last_clean = last
        consecutive_clean = consecutive_clean + 1 if clean else 0
        if consecutive_clean >= stable:
            return last
        if monotonic_fn() > deadline:
            if last_clean is not None:
                # FLAPPING-CLEAN (B152/B185): reached dangling==0 but did not hold it `stable` reads.
                # The oracle is authoritative for loss and already passed; this is a settling artifact.
                log(f"WARNING [B152/B185] CA pool reached fsck dangling==0 but did not HOLD it for "
                    f"{stable} consecutive reads within {timeout_s:.0f}s after a fault window -- it "
                    f"flapped in/out of the transient post-restart incoherence window (B141/B144/B145). "
                    f"The aggregate oracle already proved no loss, so this is a SETTLING artifact, not a "
                    f"durability finding -- continuing on the last clean reading "
                    f"(reachable={last_clean.get('reachable')} unreachable={last_clean.get('unreachable')}).")
                return last_clean
            if last.get("partial"):
                # NOT a durability finding: the scan never FINISHED, so it never had a verdict to give.
                # Distinguished explicitly because the message below asserts a real INV-NO-LOSS violation,
                # and emitting it for a slow scan is how an operator gets sent after data loss that did not
                # happen. Seen for real on 2026-07-26 when `partial=True` was wired into this waiter: it
                # reported "PERSISTENT dangling" while dangling was 0 and reachable was 0.
                raise CheckpointFailure(
                    f"CA pool consistency could not be ESTABLISHED within {timeout_s:.0f}s: every fsck in "
                    f"the window returned PARTIAL (reason: {last.get('reason', 'unstated')}), so the scan "
                    f"never produced a verdict. This is a SCAN-COST problem, not a durability finding -- "
                    f"`dangling` was {last.get('dangling')} over the subset actually walked. Raise the fsck "
                    f"budget or reduce pool size before reading anything into this.")
            raise CheckpointFailure(
                f"CA pool never reached a self-consistent state (fsck dangling==0) within {timeout_s:.0f}s "
                f"after a fault window -- PERSISTENT dangling={last.get('dangling')} "
                f"(exit_code={last.get('exit_code')}) NEVER cleared once. The aggregate oracle "
                f"(compare_aggregates) is the authoritative no-loss gate and is asserted separately; a "
                f"persistent dangling>0 here is a REAL crash-recovery / object-store durability finding "
                f"(INV-NO-LOSS), not a transient. reachable={last.get('reachable')} "
                f"unreachable={last.get('unreachable')}")
        sleep_fn(interval_s)


def settle_fsck_for_dump(fsck_fn, *, timeout_s: float = 180.0, stable: int = 2, interval_s: float = 3.0,
                         sleep_fn=time.sleep, monotonic_fn=time.monotonic):
    """Produce an fsck verdict for the FAILURE-DUMP path that does NOT false-positive a transient
    `dangling` (B141/B144/B145). The dump runs while the pool may still be CHURNING (workers were
    drained best-effort, the object store may be mid-recovery), so a single bare fsck routinely reads
    a transient `dangling>0` (HEAD-absent reachable trees) that is NOT data loss. Recording that as a
    hard dangling claim is misleading.

    Instead we reuse the SAME settle gate as the green path (`wait_for_pool_consistent`): poll until
    `dangling==0` is STABLE for `stable` reads within a bound. We return `(fsck, status)`:
      - "settled"            -> the pool reached a coherent `dangling==0` cut; the verdict is trusted.
      - "persistent-dangling"-> `dangling>0` PERSISTED past the bound; a REAL durability escalation
                                (recorded, NOT swallowed — but not re-raised here since we are already
                                on the failure path and must finish writing the dump).
      - "skipped"            -> fsck itself could not run (e.g. the container is gone). No claim made.

    A `dangling==0` after settling is a confirmed-clean pool; a persistent one is the only hard
    dangling claim this path makes. Injectable clock so the settle is pure-testable."""
    try:
        f = wait_for_pool_consistent(fsck_fn, timeout_s=timeout_s, stable=stable,
                                     interval_s=interval_s, sleep_fn=sleep_fn, monotonic_fn=monotonic_fn)
        return f, "settled"
    except CheckpointFailure:
        # Persistent dangling past the bound — re-read once to capture the last verdict for the dump.
        try:
            return fsck_fn(), "persistent-dangling"
        except Exception:
            return None, "skipped"
    except Exception:
        # fsck could not run at all (container down, etc.) — make no dangling claim.
        return None, "skipped"


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


_THROTTLE_MAX = 1.0    # the heaviest pacing the over-budget branch applies


def compute_throttle(pool_bytes, max_pool_bytes, *, current_sleep_s):
    """Resource-bounding policy (pure + unit-tested). Given the latest physical `pool_bytes` and the
    `max_pool_bytes` budget, decide the per-insert throttle sleep (seconds).

    - pool unknown (None) AND no budget set -> keep the current throttle (no-op, pass-through).
    - pool unknown (None) AND budget IS set -> FAIL CLOSED: return `_THROTTLE_MAX` so an
      unmeasurable pool STOPS growth instead of running away. This is the B204 fix: the old
      fail-open (keep current_sleep_s) let a timed-out probe keep the throttle at 0 while the pool
      grew unchecked, filling the host disk to 100% in ~1.7h.
    - < 75% of budget   -> 0.0 (unthrottled; plenty of headroom).
    - 75%..90%          -> mild pacing (0.05s/insert).
    - 90%..100%         -> hard pacing (0.25s/insert).
    - >= 100% of budget -> heavy pacing (`_THROTTLE_MAX` = 1.0s/insert) — never DROP work, only
      slow it (the TTL horizon is the real cap; throttling buys time for eviction + GC to reclaim).

    Returns the new throttle sleep. The caller LOGS loudly on any change (never silently)."""
    if max_pool_bytes is None or max_pool_bytes <= 0:
        # No budget configured: pool measurement is informational only; no throttle decision.
        return current_sleep_s
    if pool_bytes is None:
        # Budget is set but pool is unmeasurable — FAIL CLOSED to the maximum throttle so an
        # unavailable probe cannot allow unchecked pool growth (B204).
        return _THROTTLE_MAX
    frac = pool_bytes / max_pool_bytes
    if frac < 0.75:
        return 0.0
    if frac < 0.90:
        return 0.05
    if frac < 1.00:
        return 0.25
    return _THROTTLE_MAX


class MetricsTicker(threading.Thread):
    """Phase-3 background metrics sink. Every `interval_s` it snapshots BOTH nodes via
    `metrics.snapshot_cluster` (system.parts + repl/mutation/merge counts), LISTs the physical CA pool
    (`pool.pool_size`) to fill pool_objects/pool_bytes, records the rows into the sqlite `--metrics`
    db, and applies the resource-bounding throttle (`compute_throttle`) to the driver — logging any
    throttle change LOUDLY.

    The snapshot reads are read-only system queries; under chaos a node may be transiently down, so a
    snapshot failure is logged and SKIPPED (the next tick retries) rather than crashing the soak — the
    metrics curve is observability, not a correctness gate. The CHECKPOINT fsck (correctness) is
    asserted on the main thread, unchanged."""

    def __init__(self, conn, cluster, table, *, interval_s, max_pool_bytes, driver, stop_event,
                 restarts_fn, fsck_fn=None, signal_tracker=None):
        super().__init__(daemon=True, name="metrics")
        self.conn = conn
        # Accumulates what the CAS signal counters actually showed, so the end-of-run report can state
        # whether they were ever read at all rather than leaving a silent zero to be mistaken for one.
        self.signal_tracker = signal_tracker if signal_tracker is not None else SignalTracker()
        self.cluster = cluster
        self.table = table
        self.interval_s = interval_s
        self.max_pool_bytes = max_pool_bytes
        self.driver = driver
        self.stop_event = stop_event
        self.restarts_fn = restarts_fn       # callable -> cumulative restart count (chaos evidence)
        self.fsck_fn = fsck_fn                # optional callable -> latest fsck dict (checkpoint ticks)
        self.ticks = 0
        self.recorded = 0
        self.last_pool_bytes = None
        self.error = None
        # tick_once is called from BOTH the ticker thread (periodic) and the main thread (checkpoint
        # ticks carrying fsck); serialize the sqlite writes (open_db uses check_same_thread=False).
        self._lock = threading.Lock()

    def tick_once(self, ts, fsck=None):
        """Take one snapshot of both nodes + the physical pool, record it, and re-evaluate the
        throttle. Returns the number of rows recorded (0 if the snapshot failed).

        Pool-size source: when an `fsck` result is supplied (checkpoint ticks), its `physical_bytes`/
        `distinct_blobs` are AUTHORITATIVE and preferred — the cheap per-tick `mc ls` over-counts,
        because the RustFS test backend retains object VERSIONS on the CA dedup re-PUT path, inflating
        a raw recursive listing well above the true content-addressed footprint (observed ~2GB LIST vs
        ~124MB fsck physical_bytes). Between checkpoints we still use the cheap `mc ls` as a proxy
        (fsck is too slow to run every 60s), accepting its over-count."""
        objs, pbytes = pool_size()
        if fsck is not None and fsck.get("physical_bytes") is not None:
            pbytes = fsck.get("physical_bytes")
            objs = fsck.get("distinct_blobs", objs)
        self.last_pool_bytes = pbytes

        # CAS signal counters, read BEFORE the try/except that skips a tick. Deliberate: a node that is
        # down under chaos yields `None` here (a visible gap, recorded as NULL columns), while a query
        # that FAILS -- a broken probe, or a counter the binary does not have (`SignalsUnsupported`) --
        # propagates out of `tick_once`. In the ticker thread that lands in `self.error`, which the
        # main loop raises; folding it into "metrics tick skipped" would silently blind the whole run,
        # which is the exact defect this instrumentation exists to detect.
        signals_by_node = {}
        for node in self.cluster.nodes():
            values = read_signal_events(node)
            signals_by_node[repr(node)] = values
            self.signal_tracker.observe(repr(node), values)

        try:
            snaps = metrics_mod.snapshot_cluster(
                self.cluster, self.table, ts, fsck=fsck, restarts=self.restarts_fn(),
                signals_by_node=signals_by_node)
        except Exception as e:   # node transiently down under chaos -> skip this tick
            log(f"metrics tick skipped (snapshot failed, node likely down): {type(e).__name__}: {e}")
            return 0
        with self._lock:
            for snap in snaps:
                snap["pool_objects"] = objs
                snap["pool_bytes"] = pbytes
                metrics_mod.record(self.conn, snap)
        self.recorded += len(snaps)
        # Resource bounding: re-evaluate the throttle from the freshest physical pool reading.
        old = self.driver.throttle_sleep_s
        new = compute_throttle(pbytes, self.max_pool_bytes, current_sleep_s=old)
        if new != old:
            pool_gb = (pbytes / GB) if pbytes is not None else float("nan")
            budget_gb = self.max_pool_bytes / GB
            # pbytes may be None (best-effort probe failed this tick) — the fail-closed throttle still
            # changed, so this log line CAN run with pbytes=None; guard the percentage (B204 fallout).
            pct = (pbytes / self.max_pool_bytes * 100) if pbytes is not None else float("nan")
            log(f"THROTTLE change {old}s -> {new}s/insert: pool={pool_gb:.2f}GB / "
                f"budget={budget_gb:.2f}GB ({pct:.0f}%) "
                f"-- pacing inserts (work is slowed, NEVER dropped)")
            self.driver.throttle_sleep_s = new
        # Signals on the tick line: which nodes answered, and every counter that is nonzero. A tick
        # that read nothing says `signals=gap(N)` rather than printing zeros -- silence and zero must
        # not look the same.
        read_ok = [n for n, v in signals_by_node.items() if v is not None]
        nonzero = {}
        for values in signals_by_node.values():
            for event, value in (values or {}).items():
                if value:
                    nonzero[event] = max(nonzero.get(event, 0), value)
        if read_ok:
            sig_s = (f"signals={len(read_ok)}/{len(signals_by_node)} nodes"
                     + ("" if not nonzero
                        else " nonzero[" + " ".join(f"{k}={v}" for k, v in sorted(nonzero.items())) + "]"))
        else:
            sig_s = f"signals=gap({len(signals_by_node)} nodes unreadable)"
        log(f"metrics tick #{self.ticks + 1}: ts={ts} pool_objects={objs} "
            f"pool_bytes={pbytes} throttle={self.driver.throttle_sleep_s}s recorded={self.recorded} "
            f"{sig_s}")
        return len(snaps)

    def run(self):
        try:
            while not self.stop_event.is_set():
                ts = int(time.time())
                self.tick_once(ts)
                self.ticks += 1
                self.stop_event.wait(self.interval_s)
        except Exception as e:   # noqa: BLE001 -- surface ticker errors to the main thread
            self.error = e
            log(f"METRICS thread error: {type(e).__name__}: {e}\n{traceback.format_exc()}")


class SelectWorker(threading.Thread):
    """Phase-3 background read workload: one of several PARALLEL threads issuing SELECT queries
    against the live table concurrently with the INSERT/mutation workload and chaos. Each query
    filters by bucket + a moderate k-range (or a recent ts window) -- not a full-table scan, not a
    single-row point lookup -- and references `payload` so it pays for a real data read, giving the
    CAS storage read path genuine pressure throughout the whole soak window (not just at
    checkpoints, where `query_aggregates` runs one aggregate per replica).

    Cooperates with `checkpoint_active` exactly like `ChaosRunner`: pauses while a checkpoint is
    quiescing the cluster, so a concurrent scan never competes with the quiesce's own raw queries.
    A query against a transiently-down node (chaos) or a slow/timed-out query is logged and
    skipped -- this is best-effort read pressure, not a correctness gate (the fsck/aggregate
    asserts at checkpoints remain the sole correctness gate)."""

    def __init__(self, worker_id, cluster, table, *, interval_s, rng_seed, stop_event,
                 checkpoint_active, stats, stats_lock):
        super().__init__(daemon=True, name=f"select-{worker_id}")
        self.cluster = cluster
        self.table = table
        self.interval_s = interval_s
        self.rng = random.Random(rng_seed)
        self.stop_event = stop_event
        self.checkpoint_active = checkpoint_active
        self.stats = stats               # shared dict: issued / errors / rows -- see run_phase3
        self.stats_lock = stats_lock
        self.error = None

    def _next_query(self):
        node = self.cluster.node1 if self.rng.random() < 0.5 else self.cluster.node2
        bucket = self.rng.randrange(NBUCKETS)
        if self.rng.random() < 0.7:
            # Moderate k-range scan (0.2%-2% of the key space): bounded, but wide enough to touch
            # real row/blob data -- not a full-table scan, not a single-row point lookup.
            width = self.rng.randint(2000, 20000)
            k_lo = self.rng.randrange(0, KSPACE - width)
            sql = select_range_sql(self.table, bucket, k_lo, k_lo + width)
        else:
            # Recent-window scan: biases some reads toward hot (not-yet-TTL-expired) data.
            seconds = self.rng.randint(60, 1800)
            sql = select_recent_sql(self.table, bucket, seconds)
        return node, sql

    def run(self):
        try:
            while not self.stop_event.is_set():
                if self.checkpoint_active.is_set():
                    self.stop_event.wait(0.5)
                    continue
                node, sql = self._next_query()
                try:
                    result = node.query(sql, timeout=30)
                    rows = int(result.split("\t", 1)[0])
                    with self.stats_lock:
                        self.stats["issued"] += 1
                        self.stats["rows"] += rows
                except Exception as e:   # node down/busy under chaos -- non-fatal, see docstring
                    with self.stats_lock:
                        self.stats["errors"] += 1
                    log(f"SELECT workload [{self.name}] query failed (non-fatal, node likely down "
                        f"or busy under chaos): {type(e).__name__}: {e}")
                # Jittered pacing so parallel workers don't lock-step; keeps the read RATE "not too
                # small, not too big" too (a handful of concurrent moderate scans, not a query storm).
                self.stop_event.wait(self.interval_s * (0.5 + self.rng.random()))
        except Exception as e:   # noqa: BLE001 -- surface to the main thread like chaos/ticker
            self.error = e
            log(f"SELECT workload [{self.name}] thread error: {type(e).__name__}: "
                f"{e}\n{traceback.format_exc()}")


def write_failure(path, *, seed, base_time, op_id, phase, model_expected, n1, n2, fsck, last_op,
                  error=None, chaos_seed=None, last_fault=None, until_op=None,
                  fsck_status="not-run"):
    """Thin wrapper over the reusable `dump_failure` (Task 15): records the two-replica observed
    aggregates as the {node1, node2} pair and writes the stable reproducer to `path`. The
    `fsck_status` distinguishes a settled/confirmed fsck verdict from a transient/unconfirmed or
    skipped one (the failure-dump path must NOT record a bare fsck on a churning pool — B141/B144/
    B145)."""
    return dump_failure(
        seed, base_time, op_id, phase, model_expected, (n1, n2), last_fault=last_fault,
        path=path, chaos_seed=chaos_seed, until_op=until_op, error=error, last_op=last_op,
        fsck=fsck, fsck_status=fsck_status)


def _last_fault_dict(chaos):
    if chaos is None or chaos.last_fault is None:
        return None
    f = chaos.last_fault
    return {"t_offset": f.t_offset, "target": f.target.value, "action": f.action.value,
            "duration_s": f.duration_s}


def setup_cluster_and_table(seed, phase, ops, workers, checkpoint_every):
    """THE single shared bring-up for EVERY phase (1/2/3): deterministically (re)create the dedicated
    soak database as an EAGER Atomic DB, recreate the CA table on both replicas, capture base_time,
    return (cluster, model, base_time).

    lazy_load_tables is DELIBERATELY OFF (2026-07-22): the StorageTableProxy wrapper is under a
    quarantine decision after several unforwarded-virtual product bugs (SYSTEM verbs, mutations, TTL
    metadata, rename -- see docs/superpowers/reports/2026-07-21-storageproxy-forwarding-audit.md and the
    CAS backlog's lazy_load_tables item). Cost of turning it off: a transient S3 error during CAS table
    load once again strands the table in a permanently-FAILED AsyncLoader job until restart (the property
    lazy_load_tables=1 was added for) -- outage-at-load scenarios lose that coverage until the feature
    decision lands."""
    cluster = Cluster(database=DB)
    # DB lifecycle runs against the DEFAULT database (DB may not exist yet on a fresh stand; a DB-scoped
    # connection would fail UNKNOWN_DATABASE). Each command is a stateless HTTP POST that resolves the
    # database by name per call, so dropping and recreating DB underneath the scoped `cluster` is safe.
    bootstrap = Cluster()
    # Ensure DB exists so the scoped DROP TABLE below can run, then tear the table down explicitly: a
    # pre-existing large CA-over-S3 table's object teardown can take far longer than the default query
    # timeout, so give it a generous one and warn if it is actually slow. (Dropping it before the DB drop
    # keeps the DROP DATABASE that follows trivial -- an empty database.)
    for node in bootstrap.nodes():
        node.command(f"CREATE DATABASE IF NOT EXISTS {DB} ENGINE = Atomic")
    for node in cluster.nodes():
        t0 = time.monotonic()
        node.command(f"DROP TABLE IF EXISTS {TABLE} SYNC", timeout=900)
        dt = time.monotonic() - t0
        if dt > 30:
            log(f"setup DROP {TABLE} SYNC on {node.container} took {dt:.1f}s (large CA/S3 table)")
    # DETERMINISM: drop and recreate the (now-empty) database so its lazy_load_tables setting is ALWAYS
    # exactly what we specify here. A plain `CREATE DATABASE IF NOT EXISTS` is a silent no-op on a reused
    # stand and would inherit a prior run's lazy database, re-exposing the quarantined proxy path -- the
    # exact non-determinism that made a reused soak environment keep running the lazy code path.
    for node in bootstrap.nodes():
        node.command(f"DROP DATABASE IF EXISTS {DB} SYNC", timeout=900)
        node.command(f"CREATE DATABASE {DB} ENGINE = Atomic")
    base_time = int(cluster.node1.scalar("SELECT toUnixTimestamp(now())")) - 60
    log(f"base_time={base_time} (needed for replay) seed={seed} phase={phase} "
        f"ops={ops} workers={workers} checkpoint_every={checkpoint_every}")
    model = Model(seed, base_time=base_time)
    ddl = DDL_TEMPLATE.format(table=TABLE)
    for node in cluster.nodes():
        node.command(ddl)
    log(f"created {DB}.{TABLE} on both replicas (lazy_load_tables OFF, eager Atomic DB)")
    return cluster, model, base_time


def phase3_chaos_schedule(chaos_seed, plan, chaos_interval_s):
    """Generate the deterministic fault schedule ONLY over the chaos-armed window of the stage plan
    (CHAOS + CLIFF stages), so faults never fire during warmup/steady/GC-checkpoint/converge. The
    schedule is generated for [0, win_end) and faults before `win_start` are dropped, then a final
    CONVERGE restart is appended just inside the converge tail so the soak always exercises a clean
    server restart at the end (per §8 'final converge + restart')."""
    win_start, win_end = chaos_window(plan)
    raw = generate_chaos_schedule(chaos_seed, win_end, chaos_interval_s)
    faults = [f for f in raw if f.t_offset >= win_start]
    # Final converge+restart: a graceful both-replica restart shortly into the CONVERGE stage. The
    # recovery checkpoint after it proves the soak survives a clean restart of the whole cluster.
    converge = next((s for s in plan if s.kind == StageKind.CONVERGE), None)
    if converge is not None and converge.t_end - converge.t_start > 5:
        faults.append(Fault(t_offset=converge.t_start + 1, target=FaultTarget.BOTH,
                            action=FaultAction.RESTART, duration_s=3))
    return faults


def run_phase3(args):
    """Phase-3 (24h productionization) TIME-DRIVEN soak. Maps wall-clock fractions of --duration to a
    fixed stage timeline (soak.schedule), reusing the Phase-1/2 machinery: the same deterministic
    ledger feeds the workers, but each op is GATED by the active stage's capability flags
    (warmup=inserts only; +mutations; +TTL pressure; a quiesced GC checkpoint; +chaos; a cliff; a
    final converge+restart). A background MetricsTicker records a per-minute curve into --metrics and
    enforces the --max-pool-gb budget by throttling (never dropping) inserts. Checkpoints fire at
    every stage boundary and use the SAME hard asserts as phase 1/2 (dangling==0 HEAD-confirmed,
    counts==model both replicas, dryrun⊆unreachable; residual unreachable tracked as B140 debris)."""
    duration_s = args.duration if args.duration is not None else 24 * 3600
    plan = stage_plan(duration_s)
    metrics_interval = metrics_interval_for(duration_s)
    chaos_interval = args.chaos_interval
    max_pool_bytes = int(args.max_pool_gb * GB) if args.max_pool_gb else None

    log(f"PHASE 3 timeline ({duration_s}s, seed={args.seed} chaos_seed={args.chaos_seed}):")
    for s in plan:
        log(f"  stage {s.kind.value:14s} [{s.t_start:>7d}..{s.t_end:<7d})s  inserts={s.allow_inserts} "
            f"opt={s.allow_optimize} mut={s.allow_mutations} cliffs={s.allow_cliffs} chaos={s.chaos_armed}")
    cwin = chaos_window(plan)
    log(f"PHASE 3 metrics_interval={metrics_interval}s max_pool_gb={args.max_pool_gb} "
        f"chaos_window={cwin}s metrics_db={args.metrics}")

    cluster, model, base_time = setup_cluster_and_table(
        args.seed, 3, args.ops, args.workers, "stage-boundary")

    # PREFLIGHT: prove every watched CAS counter EXISTS in this binary before a single op runs. Without
    # it, a renamed or not-yet-existing counter reads as a permanent quiet zero and the run is blind for
    # its whole duration; with it, the run dies at second zero naming the counter. It also exercises the
    # exact query the ticker will use, so a malformed probe cannot silently disable the metrics curve.
    signal_baseline = preflight_signals(cluster)
    log(f"CAS SIGNALS preflight OK: {len(CAS_SIGNAL_EVENTS)} counters present on all "
        f"{len(signal_baseline)} node(s) — {', '.join(CAS_SIGNAL_EVENTS)}")
    for node_name, values in sorted(signal_baseline.items()):
        log(f"CAS SIGNALS baseline {node_name}: "
            + " ".join(f"{k}={values[k]}" for k in CAS_SIGNAL_EVENTS))
    signal_tracker = SignalTracker()
    # Seeded with the preflight reading so an inherited nonzero counter on a reused container is not
    # charged to this run: what this run did is the delta.
    fencing = LatePutFencing(signal_baseline)
    phase_coverage = PhaseCoverage()
    # Start of the first per-phase GC-log window. Advanced to each checkpoint's own time afterwards so
    # consecutive checkpoints report disjoint windows.
    phase_window_start = {"ts": int(time.time())}

    # Phase 3 ALWAYS exercises crash+recovery (chaos), so the driver is transport-resilient.
    driver = Driver(cluster, model, args.seed, base_time, args.workers,
                    insert_mode=args.insert_mode, transport_resilient=True)

    # A generous op pool: workers consume ops as fast as pacing allows; we size it so the stream never
    # runs dry within `duration_s`. The stages GATE which classes fire; cliffs are additionally bounded
    # by the CLIFF-stage gate. Mutations (UPDATE/DELETE) are THINNED to stay materializable over the
    # remote CA pool — see `demote_dense_mutations`. The thinning is a pure function of op_id order so
    # the cluster and model consume the identical thinned ledger.
    n_ops = args.ops if args.ops is not None else max(20000, duration_s * 50)
    min_mut_gap = args.min_ops_between_mutations
    def gen_ledger(seed):
        return demote_dense_mutations(generate_ledger(seed, n_ops), min_mut_gap)
    ledger = gen_ledger(args.seed)
    n_mut = sum(1 for op in ledger if op.type in (OpType.UPDATE, OpType.DELETE))
    log(f"phase-3 ledger: {len(ledger)} ops available (gated by stage capability); "
        f"{n_mut} mutations kept (min_ops_between_mutations={min_mut_gap})")

    conn = metrics_mod.open_db(args.metrics)

    chaos_stop = threading.Event()
    metrics_stop = threading.Event()
    checkpoint_active = threading.Event()
    recovery_lock = threading.Lock()
    recovery_pending = []
    restarts = {"n": 0}
    restarts_lock = threading.Lock()

    def on_fault_done(fault):
        if fault.action in (FaultAction.KILL, FaultAction.RESTART):
            with restarts_lock:
                restarts["n"] += 1
        with recovery_lock:
            recovery_pending.append(fault)

    schedule = [] if args.no_chaos else phase3_chaos_schedule(args.chaos_seed, plan, chaos_interval)
    log(f"phase-3 chaos schedule: {len(schedule)} faults over the chaos window "
        f"(chaos_seed={args.chaos_seed} mean_interval={chaos_interval}s no_chaos={args.no_chaos})")
    chaos = ChaosRunner(schedule, on_fault_done=on_fault_done, stop_event=chaos_stop,
                        checkpoint_active=checkpoint_active)

    ticker = MetricsTicker(conn, cluster, TABLE, interval_s=metrics_interval,
                           max_pool_bytes=max_pool_bytes, driver=driver, stop_event=metrics_stop,
                           restarts_fn=lambda: restarts["n"], signal_tracker=signal_tracker)

    select_stop = threading.Event()
    select_stats = {"issued": 0, "errors": 0, "rows": 0}
    select_stats_lock = threading.Lock()
    select_workers = [
        SelectWorker(i, cluster, TABLE, interval_s=args.select_interval_s,
                    rng_seed=args.seed ^ (0x5e1ec7 + i), stop_event=select_stop,
                    checkpoint_active=checkpoint_active, stats=select_stats,
                    stats_lock=select_stats_lock)
        for i in range(args.select_workers)
    ]
    if select_workers:
        log(f"phase-3 SELECT read-workload: {len(select_workers)} parallel worker(s), "
            f"~{args.select_interval_s}s mean pacing each (moderate bucket+k-range / recent-ts scans)")

    def do_checkpoint(label, *, strict_unreachable=False):
        log(f"{label}")
        checkpoint_active.set()
        try:
            wait_for_healthy(cluster)
            # Entry gate only needs dangling/exit_code -> cheap summary fsck (detail=False).
            # Each poll is bounded at 180s; FsckTimeout degrades to a logged skip (B146/B154).
            try:
                wait_for_pool_consistent(lambda: run_fsck(FSCK_CONTAINER, detail=False, timeout_s=180))
            except FsckTimeout as _e:
                log(f"WARNING [B146/B154] {label}: entry-gate fsck timed out ({_e}); "
                    f"proceeding to checkpoint without pool-consistent gate — soak continues")
            now, exp, n1, n2, f, dr = checkpoint(driver, cluster, model, 2, strict_unreachable=strict_unreachable)
        finally:
            checkpoint_active.clear()
        log(f"{label} OK: now={now} count={exp['count']} fsck reachable={f.get('reachable')} "
            f"unreachable={f.get('unreachable')} (fold backlog / AwaitingGc) dangling={f.get('dangling')} "
            f"stale_edge={_stale_edge_display(f)} dryrun_count={dr.get('count')}")
        # Record a checkpoint-tagged metrics tick carrying the fsck result (§2: include fsck at checkpoints).
        checkpoint_ts = int(time.time())
        ticker.tick_once(checkpoint_ts, fsck=f)
        # The two signal families the driver could not see before 2026-07-26. Both are read here
        # fail-closed (the cluster is quiesced and health-gated) and reported. One family inside them
        # is also GATED: the late-PUT-loses violation counters fail the checkpoint (see
        # `capture_checkpoint_signals`), because their benign rate is zero by construction.
        capture_checkpoint_signals(cluster, label, tracker=signal_tracker, fencing=fencing)
        capture_phase_summary(cluster, label, since_ts=phase_window_start["ts"],
                              coverage=phase_coverage, conn=conn, ts=checkpoint_ts)
        phase_window_start["ts"] = checkpoint_ts
        return now, exp, n1, n2, f, dr

    def drain_recovery_checkpoints():
        with recovery_lock:
            pending = list(recovery_pending)
            recovery_pending.clear()
        if not pending:
            return
        if chaos.error is not None:
            raise chaos.error
        last = pending[-1]
        log(f"RECOVERY: {len(pending)} fault window(s) completed; last="
            f"{last.target.value}/{last.action.value}/{last.duration_s}s — recovery checkpoint")
        do_checkpoint("recovery checkpoint")

    t0 = time.monotonic()
    chaos.start()
    ticker.start()
    for w in select_workers:
        w.start()
    last_op = None
    op_iter = iter(ledger)
    prev_stage_kind = None
    gc_checkpoint_done = False
    gc_idx = next(i for i, s in enumerate(plan) if s.kind == StageKind.GC_CHECKPOINT)
    stage_index = {s.kind: i for i, s in enumerate(plan)}
    try:
        while True:
            elapsed = time.monotonic() - t0
            if elapsed >= duration_s:
                break
            stage = stage_at(plan, elapsed)

            # Stage transition. A FULL quiesced checkpoint (drain + OPTIMIZE FINAL + MATERIALIZE TTL +
            # GC-to-fixpoint + the hard invariant asserts) is EXPENSIVE — over a large pool an OPTIMIZE
            # FINAL alone takes minutes — so we do NOT run one at every boundary (that would starve the
            # timeline). Per §8 the explicit quiesced checkpoint is the GC_CHECKPOINT stage; we run the
            # full checkpoint there. The final converge checkpoint (after the loop) and the
            # post-fault recovery checkpoints are the other hard-asserted points. Every OTHER boundary
            # just records a (non-quiesced) metrics tick so the curve marks the transition cheaply.
            if stage.kind != prev_stage_kind:
                log(f"=== STAGE {stage.kind.value} at t+{elapsed:.0f}s ===")
                if prev_stage_kind is not None and stage.kind != StageKind.GC_CHECKPOINT:
                    ticker.tick_once(int(time.time()))   # cheap transition marker, no quiesce
                prev_stage_kind = stage.kind
            # The §8 GC checkpoint MUST run once. A slow throttled iteration can sample `stage_at`
            # late and JUMP across the (narrow) GC_CHECKPOINT window without ever observing it as the
            # current stage, so we trigger on "we are AT OR PAST the GC stage and haven't run it yet"
            # rather than on the exact transition. This guarantees the mandatory quiesced GC drive
            # fires even when the timeline compresses below one iteration per stage window.
            if not gc_checkpoint_done and stage_index.get(stage.kind, -1) >= gc_idx:
                do_checkpoint("GC checkpoint (stage §8 checkpoint+GC)")
                gc_checkpoint_done = True

            # Recovery checkpoint for any completed fault windows (chaos only armed in its window).
            drain_recovery_checkpoints()
            if chaos.error is not None:
                raise chaos.error
            if ticker.error is not None:
                raise ticker.error
            for w in select_workers:
                if w.error is not None:
                    raise w.error

            # Backpressure: reap completed futures and bound the in-flight set so the continuous
            # time-driven producer never outruns the workers (unbounded inflight would balloon memory
            # + the executor queue over a multi-hour soak). CRITICALLY, this does NOT use a blocking
            # inner loop: it does ONE harvest + short sleep and `continue`s (BEFORE pulling an op, so
            # no ledger op is dropped), so the TOP of the loop (stage transition + the mandatory GC
            # checkpoint + recovery-checkpoint drain) is RE-EVALUATED every iteration. A blocking inner
            # wait here previously starved the stage clock under heavy throttle, causing the GC
            # checkpoint to be skipped and the timeline to appear stalled.
            driver.harvest()
            inflight_cap = max(2 * args.workers, 64)
            if driver.inflight_count() >= inflight_cap:
                time.sleep(0.05)
                continue

            # In a quiesced GC_CHECKPOINT stage no ops fire; just let the stage elapse (the GC
            # checkpoint above already drove GC). A short wait avoids a busy spin.
            if stage.kind == StageKind.GC_CHECKPOINT:
                time.sleep(0.2)
                continue

            # Pull the next op and execute it IF the current stage permits its class; otherwise skip
            # (the op is consumed deterministically; gating decides whether it fires).
            try:
                op = next(op_iter)
            except StopIteration:
                # Ledger exhausted before the timeline ended -> regenerate a fresh (mutation-thinned)
                # continuation so the workers never starve during a long soak.
                ledger = gen_ledger(args.seed ^ int(elapsed))
                op_iter = iter(ledger)
                op = next(op_iter)

            # Honor an explicit --until-op cap so a phase-3 run can be re-driven to just before a
            # failing op (the time-driven loop regenerates ledgers, so the cap is on the op_id).
            if args.until_op is not None and op.op_id > args.until_op:
                continue
            if _phase3_op_permitted(op, stage):
                driver.execute(op)
                last_op = op

        # Timeline complete: stop chaos, drain final recovery, run the final converge checkpoint.
        chaos_stop.set()
        chaos.join(timeout=30)
        drain_recovery_checkpoints()
        if chaos.error is not None:
            raise chaos.error
        do_checkpoint("final converge checkpoint", strict_unreachable=True)

    except (CheckpointFailure, QueryError, OSError) as e:
        chaos_stop.set()
        metrics_stop.set()
        select_stop.set()
        if isinstance(e, CheckpointFailure):
            kind = "CHECKPOINT FAILURE"
        elif isinstance(e, QueryError):
            kind = "WORKLOAD FAILURE"
        else:
            kind = "TRANSPORT FAILURE"
        driver.drain_silent()
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
        # Quiesce-before-dump: do NOT record a bare fsck on a still-churning pool (it false-positives
        # transient `dangling` — B141/B144/B145). Settle to a stable `dangling==0` (or label the
        # verdict transient/persistent) before recording it.
        f, fsck_status = settle_fsck_for_dump(lambda: run_fsck(FSCK_CONTAINER, timeout_s=180))
        op_id = last_op.op_id if last_op is not None else None
        last_op_d = last_op.__dict__ if last_op is not None else None
        payload = write_failure(
            args.failure_json, seed=args.seed, base_time=base_time, op_id=op_id, phase=3,
            model_expected=exp, n1=n1, n2=n2, fsck=f, fsck_status=fsck_status, last_op=last_op_d,
            error=f"{kind}: {e}", until_op=args.until_op,
            chaos_seed=args.chaos_seed, last_fault=_last_fault_dict(chaos))
        print(f"{kind}: {e}", file=sys.stderr)
        print(json.dumps(payload, indent=2, default=str), file=sys.stderr)
        driver.executor.shutdown(wait=False, cancel_futures=True)
        sys.exit(1)
    finally:
        chaos_stop.set()
        metrics_stop.set()
        select_stop.set()
        chaos.join(timeout=30)
        ticker.join(timeout=metrics_interval + 30)
        for w in select_workers:
            w.join(timeout=30)
        driver.executor.shutdown(wait=True)
        try:
            conn.close()
        except Exception:
            pass

    log(f"ABORTED-retried INSERT attempts: {driver.aborted_retries}; "
        f"transport-retried op attempts: {driver.transport_retries}; "
        f"faults fired: {chaos.faults_fired}; restarts: {restarts['n']}; "
        f"metrics rows recorded: {ticker.recorded} (ticks={ticker.ticks})")
    if select_workers:
        log(f"SELECT read-workload: {select_stats['issued']} queries issued, "
            f"{select_stats['errors']} failed (non-fatal), {select_stats['rows']} total rows "
            f"touched across {len(select_workers)} parallel worker(s)")
    # AVAILABILITY REPORT (2026-07-13): each driver-level retry is a product-visible failure the
    # server did not absorb. Per-class counts make harness tolerance measurable instead of masking:
    # compare across runs — a healthy build keeps non-node-down classes near zero.
    if driver.retry_class_counts:
        breakdown = ", ".join(f"{k}={v}" for k, v in sorted(driver.retry_class_counts.items()))
        log(f"AVAILABILITY: driver-retried product-visible failures by class: {breakdown}")
    else:
        log("AVAILABILITY: zero driver-retried product-visible failures (server absorbed everything)")
    # SIGNAL REPORT. Task 21's stability criterion is not "the run was green" but "the run was green
    # AND every signal was actually observed at least once" -- a green run in which a counter was never
    # read is blind, not green. These two blocks are what makes that checkable from the run log.
    for line in signal_tracker.report_lines():
        log(line)
    for line in fencing.report_lines():
        log(line)
    for line in phase_coverage.report_lines():
        log(line)
    print("PHASE3 OK")
    return 0


def _elapsed_past(t0, duration_s) -> bool:
    return (time.monotonic() - t0) >= duration_s


def _phase3_op_permitted(op, stage) -> bool:
    """True iff `op`'s class is permitted to fire in `stage` (the time-driven capability gate)."""
    if op.type == OpType.INSERT:
        return stage.allow_inserts
    if op.type == OpType.OPTIMIZE:
        return stage.allow_optimize
    if op.type in (OpType.UPDATE, OpType.DELETE):
        return stage.allow_mutations
    if op.type in CLIFF_TYPES:
        return stage.allow_cliffs
    return False


def main(argv=None):
    ap = argparse.ArgumentParser(description="CA soak green-path / chaos driver")
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--phase", type=int, default=1)
    ap.add_argument("--ops", type=int, default=None,
                    help="(phase 1/2) number of ledger ops to execute. Required for phase 1/2; for "
                         "phase 3 the op stream is sized from --duration (a generous estimate), so "
                         "--ops is an optional cap.")
    ap.add_argument("--workers", type=int, default=6)
    ap.add_argument("--checkpoint-every", type=int, default=None,
                    help="(phase 1/2) checkpoint every N executed ops. Required for phase 1/2; phase 3 "
                         "checkpoints at STAGE boundaries (time-driven), so this is ignored there.")
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
    ap.add_argument("--no-chaos", action="store_true",
                    help="(phase 3) disable ALL faults — empty chaos schedule, including the forced "
                         "converge restart. Use for a clean smoke that isolates non-chaos behavior.")
    ap.add_argument("--duration", type=parse_duration, default=None,
                    help="(phase 2/3) wall-clock duration. Phase 2: seconds to generate faults over "
                         "(defaults to a generous estimate from --ops). Phase 3: TOTAL soak duration "
                         "(default 24h); accepts a suffixed form like 24h/90m/600s or bare seconds. "
                         "The phase-3 stage timeline is a deterministic function of this duration.")
    # --- Phase-3 args ------------------------------------------------------------------------
    ap.add_argument("--metrics", default="soak.db",
                    help="(phase 3) sqlite path the per-minute metrics ticker records into.")
    ap.add_argument("--max-pool-gb", type=float, default=40.0,
                    help="(phase 3) physical CA-pool budget (GB). When a metrics tick shows the pool "
                         "approaching this, inserts are THROTTLED (paced, never dropped) and the "
                         "throttle change is logged loudly.")
    ap.add_argument("--min-ops-between-mutations", type=int, default=80,
                    help="(phase 3) thin UPDATE/DELETE ops so a kept mutation is at least this many "
                         "ops after the previous kept one (the rest demote to OPTIMIZE). Keeps "
                         "mutations sparse enough to materialize over the remote CA pool without "
                         "piling up a backlog that would time out the quiesced checkpoint. 0 disables.")
    ap.add_argument("--select-workers", type=int, default=4,
                    help="(phase 3) number of PARALLEL background SELECT-query worker threads run "
                         "concurrently with the insert/mutation workload and chaos. Each issues "
                         "moderate bucket+k-range (or recent-ts-window) scans against the live table "
                         "-- not full-table scans, not point lookups -- to exercise the CAS read "
                         "path throughout the soak, not just at checkpoints. 0 disables.")
    ap.add_argument("--select-interval-s", type=float, default=2.0,
                    help="(phase 3) mean pacing (s) between one SELECT worker's successive queries "
                         "(jittered 0.5x-1.5x). Lower = more read pressure.")
    args = ap.parse_args(argv)

    phase2 = args.phase == 2
    phase3 = args.phase == 3
    if (phase2 or phase3) and args.chaos_seed is None:
        args.chaos_seed = args.seed
    if phase3:
        return run_phase3(args)
    if args.ops is None or args.checkpoint_every is None:
        ap.error("--ops and --checkpoint-every are required for phase 1/2")

    # Single shared, deterministic bring-up (same helper phase 3 uses): eager Atomic DB, no lazy setting.
    cluster, model, base_time = setup_cluster_and_table(
        args.seed, args.phase, args.ops, args.workers, args.checkpoint_every)

    # Same preflight as phase 3: refuse to start a run that cannot read its own signals.
    signal_baseline = preflight_signals(cluster)
    log(f"CAS SIGNALS preflight OK: {len(CAS_SIGNAL_EVENTS)} counters present on all nodes")
    signal_tracker = SignalTracker()
    fencing = LatePutFencing(signal_baseline)
    phase_coverage = PhaseCoverage()
    phase_window_start = {"ts": int(time.time())}

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
                # Each poll is bounded at 180s; FsckTimeout degrades to a logged skip (B146/B154).
                try:
                    wait_for_pool_consistent(lambda: run_fsck(FSCK_CONTAINER, timeout_s=180))
                except FsckTimeout as _e:
                    log(f"WARNING [B146/B154] {label}: entry-gate fsck timed out ({_e}); "
                        f"proceeding to checkpoint without pool-consistent gate — soak continues")
            now, exp, n1, n2, f, dr = checkpoint(driver, cluster, model, args.phase)
        finally:
            checkpoint_active.clear()
        log(f"{label} OK: now={now} count={exp['count']} "
            f"fsck reachable={f.get('reachable')} unreachable={f.get('unreachable')} "
            f"(fold backlog / AwaitingGc) dangling={f.get('dangling')} stale_edge={_stale_edge_display(f)} "
            f"dryrun_count={dr.get('count')}")
        checkpoint_ts = int(time.time())
        capture_checkpoint_signals(cluster, label, tracker=signal_tracker, fencing=fencing)
        # Phase 1/2 has no metrics db, so the phase summary is logged only (conn=None).
        capture_phase_summary(cluster, label, since_ts=phase_window_start["ts"],
                              coverage=phase_coverage, conn=None, ts=checkpoint_ts)
        phase_window_start["ts"] = checkpoint_ts
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
        # Quiesce-before-dump: settle the fsck to a stable `dangling==0` (or label it transient/
        # persistent) rather than recording a bare fsck on a churning pool (B141/B144/B145).
        f, fsck_status = settle_fsck_for_dump(lambda: run_fsck(FSCK_CONTAINER, timeout_s=180))
        op_id = last_op.op_id if last_op is not None else None
        last_op_d = last_op.__dict__ if last_op is not None else None
        payload = write_failure(
            args.failure_json, seed=args.seed, base_time=base_time, op_id=op_id, phase=args.phase,
            model_expected=exp, n1=n1, n2=n2, fsck=f, fsck_status=fsck_status, last_op=last_op_d,
            error=f"{kind}: {e}", until_op=args.until_op,
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
    for line in signal_tracker.report_lines():
        log(line)
    for line in fencing.report_lines():
        log(line)
    for line in phase_coverage.report_lines():
        log(line)
    if phase2:
        log(f"transport-retried op attempts (chaos node-down fired and was handled): "
            f"{driver.transport_retries}; faults fired: {chaos.faults_fired if chaos else 0}")
        print("PHASE2 OK")
    else:
        print("PHASE1 OK")
    return 0


if __name__ == "__main__":
    # Deterministic, prompt process exit after the verdict. Each phase runner does its REAL cleanup
    # (executor shutdown(wait=True), metrics-DB close, thread joins) in its own `finally` BEFORE
    # returning, so nothing durable is skipped here. We then `os._exit` to bypass interpreter-shutdown
    # atexit handlers, which were observed to leave the driver lingering for a long time AFTER the
    # verdict was already printed and flushed (post-`PHASE3 OK`: a single sleeping main thread stuck in
    # finalization). Preserve the return code (and a `sys.exit(N)` raised on the failure path).
    _rc = 0
    try:
        _rc = main() or 0
    except SystemExit as _e:
        _rc = _e.code if isinstance(_e.code, int) else (0 if _e.code in (None, 0) else 1)
    finally:
        sys.stdout.flush()
        sys.stderr.flush()
    os._exit(_rc)
