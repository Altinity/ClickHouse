# CAS write path — bounded-concurrency per-part commit

- **Date:** 2026-07-22
- **Status:** design approved (bounded-worker architecture validated across two rounds of independent
  concurrency review — Fable + Codex/gpt-5.6-sol at xhigh; all prior blocking findings closed, remaining
  exception-safety hardening folded in), ready for implementation plan
- **Area:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (`ContentAddressedTransaction` commit path)
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"OPTIMIZATION OPPORTUNITY (write-path
  latency, HIGH) — CAS-on-S3 INSERT ~7.6× slower than standard S3"* (logged 2026-07-21), root causes
  #1 (sequential per-part commit) and #2 (sequential per-blob upload).

## Problem

A CAS-on-S3 INSERT of a 500-partition wide part (10M rows, 30 columns) measured **170.6 s** versus
**22.4 s** for the same insert on a standard S3 `ReplicatedMergeTree` — 7.6× slower despite issuing
FEWER `PutObject` (2566 vs 6156 per node). Confirmed from `system.query_log` ProfileEvents, the
cause is serialization, not bandwidth: `S3WriteMicroseconds` 117.5 s over ~4.7 threads,
`CasRefQueueWaitMicroseconds` 36.2 s, and the smoking gun `CasRefBatchFlushes` = 1026 ==
`CasRefBatchedMutations` = 1026 (ref-ledger batch size exactly **1.0**).

`ContentAddressedTransaction::commit()` (`ContentAddressedTransaction.cpp:396-404`) drives all ~500
parts through `publishStaging` in a single-threaded `for` loop. Being single-threaded, it feeds the
ref-ledger one item at a time, so `CasRefLedger::appendRefOps`' leader/follower batching (which
collapses concurrent same-namespace appends into one S3 round trip, up to `kMaxRefBatch`) has nothing
to batch. Each part pays its own two blocking ref-lane round trips (`precommitAdd` + `promote`) — ~1026
serial ref-log PUTs plus 36 s of queue wait — and its blob uploads (`uploadPendingBlobs`,
`ContentAddressedTransaction.cpp:241-283`) run one at a time.

## What the standard S3 write path teaches us

The standard path reaches ~12× write concurrency without deadlocking, and its structure is the model
to copy:

- **Concurrency comes from the coarse level plus async leaf I/O, not from nested orchestration.** The
  INSERT thread streams columns; each `WriteBufferFromS3` fires multipart part-uploads asynchronously
  onto `getThreadPoolWriter()` (`REMOTE_FS_WRITE_THREAD_POOL`, size 100) and keeps going;
  `finalize()`/`waitAll()` blocks on the **caller's own thread**, never a pool worker, and a leaf
  upload task never re-enqueues onto its own pool. **The waiter is never a member of the pool it
  waits on** — the documented anti-deadlock rule (`threadPoolCallbackRunner.h:363-380`).
- **The commit loop itself is sequential per part** (even `ReplicatedMergeTree` does one Keeper
  `multi()` per part serially) — cheap only because Keeper is sub-millisecond in-cluster. CAS's
  per-part ref-append is an S3-latency PUT (~46 ms), so **batching is CAS's unique lever**; the
  standard path never needed it.

Two independent concurrency reviews (Fable, Codex/gpt-5.6-sol) refuted an earlier draft of this design
that nested per-part *and* per-blob tasks on the same pool — a deterministic deadlock of the shared IO
pool, because `ThreadPool` backpressure is queue-fullness (`ThreadPool.cpp:333`), not thread-busyness,
so an "inline fallback" never fires while 100 workers park waiting on queued children. The revised
design below removes all nested same-pool waits.

## Goal and non-goals

Engage the ref-ledger's batching and overlap blob uploads by committing parts with **bounded
concurrency**, mirroring the standard path: coarse-grained workers plus the async multipart I/O that
`WriteBufferFromS3` already provides, with strict no-nested-wait pool layering. Output stays
logically equivalent and every CAS invariant holds.

Non-goals (separate BACKLOG items, not touched here): explicit intra-part blob fan-out (see "Deferred"
below), the unconditional promote manifest GET (root cause #3), the 1 MiB HEAD-before-PUT gate (root
cause #4), and any change to the ref-ledger internals or the intra-part ordering protocol.

## Design

### Bounded-concurrency part commit

Replace the sequential `for (auto & [key, st] : parts)` in `commit()` with a bounded pool of workers,
each running one whole part's `publishStaging` **synchronously and unchanged** (`stageManifest →
precommitAdd → uploadPendingBlobs → promote`). Concurrency bound = a new setting
(`cas_commit_concurrency`, default modest, e.g. 16 — enough to make ref batches meaningful while
bounding how many threads block on ref-lane round trips). The commit loop dispatches the parts,
snapshotting the part keys first (the `parts` map is not mutated during commit), and joins on the
calling thread.

Why this hits both root causes:
- **Ref-ledger batching engages.** Up to `cas_commit_concurrency` workers reach `precommitAdd`/
  `promote` concurrently; their items pile into `rt->pending` and one worker becomes leader and
  `flushRefBatch`-es them all in one append. Batch size goes from 1.0 toward the worker count
  (capped at `kMaxRefBatch`). Workers block in `appendRefOps` as leader/follower — safe, because
  `appendRefOps` runs its leader on the calling thread and never schedules onto the commit pool.
- **Blob uploads overlap for free.** Each worker's `putBlob` streams through `WriteBufferFromS3`,
  which already multiparts onto `getThreadPoolWriter()`. So N workers give N-way cross-part blob
  overlap, and each blob still gets its own multipart parallelism — without any intra-part fan-out.

### Pool-layering discipline (the anti-deadlock rule)

- The commit-worker pool **must be disjoint from `getThreadPoolWriter()`**. A worker blocks in
  `WriteBufferFromS3::finalize` waiting on `getThreadPoolWriter`; if the worker were itself a
  `getThreadPoolWriter` thread, that is the same deadlock one level down.
- **Use a dedicated CAS commit pool of exactly `cas_commit_concurrency`, not a per-part fan-out over a
  shared pool.** A plain runner that schedules one task per part over `getIOThreadPool()` does NOT bound
  concurrency to `cas_commit_concurrency` — it schedules every part directly, so up to the whole pool
  (100) run at once — and each commit worker *blocks* for the entire multi-second `publishStaging`
  (blob PUTs + up to two ref-lane round trips, each up to the controller's full retry envelope on a
  blip). Blocking that many shared-`getIOThreadPool` threads starves latency-sensitive users of that
  pool (`MergeTreeSource` async reads, parallel format reading, `ParallelSyncFiles`) — a cross-workload
  stall, not a deadlock, but a real one under a few concurrent inserts. So dispatch exactly
  `min(cas_commit_concurrency, part_count)` worker-*loop* callbacks that claim part indices from the
  pre-snapshotted `parts` array (or a dedicated `ThreadPool` sized `cas_commit_concurrency`); either
  isolates the blocking and makes the bound real. The saturation test asserts the max active worker
  count.
- **No worker ever waits on work queued to its own pool.** Blobs run serially inside a worker, so a
  worker never fans out onto the commit pool.
- The commit-worker tasks **must propagate the query `ThreadGroup`** (via the standard runner
  mechanism, `threadPoolCallbackRunner.h:33-38`), or the CAS ProfileEvents (`CasRefBatchFlushes` /
  `CasRefBatchedMutations` / `CasRefQueueWaitMicroseconds`) and `content_addressed_log.query_id` lose
  attribution to the INSERT. Capture the group without extending its lifetime past the query
  (the B90 freed-ThreadGroup precedent).

### Correctness surface (much smaller than the nested design)

- **No new intra-build race.** Each part owns its own `PartWriteTxn` (`buildFor`,
  `ContentAddressedTransaction.cpp:133-141`), and blobs run serially within its worker, so the build's
  `deps` map is touched by a single thread. (The `deps`-map mutex from the earlier draft is
  unnecessary here; it returns only if intra-part fan-out is ever added — see Deferred.)
- **Cross-part shared state:** the per-part rollback outcome is written into a **preallocated,
  per-part, index-addressed slot** (one per dispatched part, reserved before any scheduling) so a
  worker never needs to grow a shared container after its commit is durable — a `vector::push_back`
  that throws `bad_alloc` *after* a durable promote would leak the ref past rollback.
- **Worker tasks capture non-owning references only** (never an owning `shared_ptr` to the transaction
  or its state). On a mid-dispatch failure a cancelled task's closure is destroyed on an arbitrary pool
  thread after query teardown; an owning capture could run `~ContentAddressedTransaction`/`abandon`
  there. Join-before-destruction (below) makes non-owning captures safe.
- **Store-level components are thread-safe** (verified in review, not assumed): the dedup cache is a
  locked `CacheBase`; `getRefTableRuntime`/recovery/stale-precommit-sweep are guarded by
  `ref_queue_mutex`/`state_mutex`; `CasRequestController` state is per-call; `EventEmitter` emits into
  a `SystemLog` sink installed before traffic. Concurrent same-namespace `appendRefOps` is already
  exercised by concurrent INSERTs.
- **EDGE-BEFORE-OBSERVE is preserved.** Each worker keeps the durable
  `stageManifest → precommitAdd → uploadPendingBlobs → promote` order internally; parts are
  independent (each adopt is protected by that part's own durable precommit).
- **Straggler-vs-`abandon` is closed ONLY under the exception-safe join** (Error handling below): with
  blobs serial per worker there is no leaked in-part blob task, and the join guarantees no worker runs
  during rollback or the dtor's `abandon()`. If that join is not airtight the `deps`/`alive`/
  `precommitted` single-owner contract breaks — hence the join discipline is the load-bearing rule.

## Error handling / rollback

The reviews found the existing rollback is fragile and that concurrency makes late failures — after
many parts already published — far more likely. Rework it:

- **Exception-safe join that is structurally ordered before rollback (the single hard rule).** A
  function-scope `SCOPE_EXIT` is WRONG here: it runs at function exit, i.e. *after* a same-function
  catch has already performed rollback — leaving a worker live during rollback/`abandon`. And a
  worker's task handle must never be lost to a throw: reserving-then-scheduling where the *registration*
  of the returned future can throw (a growing container) would leave a task running but untracked, also
  escaping the join. Therefore: **preallocate every per-part task handle, outcome slot, and exception
  slot BEFORE any scheduling** (reserve so moving a handle into its slot cannot throw); wrap the
  dispatch+drain in an **inner scope** whose non-throwing guard `wait()`s (never `get()`s — a throwing
  `get` in a noexcept guard terminates) on every *registered* handle; only after that inner scope has
  fully drained does the outer logic inspect the stored exceptions and roll back. No worker is ever live
  when rollback or `~ContentAddressedTransaction`'s `abandon()` runs, on any path including a
  dispatch-partway throw. Prefer a runner whose destructor cancels not-yet-started tasks and waits for
  running ones (e.g. `ThreadPoolCallbackRunnerLocal`) declared *inside* the try scope, so unwinding
  drains before the catch body.
- **Exact-manifest rollback (`dropRefIfMatches`), outcome published AT confirm.** Today `created_refs`
  stores only `(ns, ref)` and rollback `dropRef`s whatever manifest currently occupies that name —
  which can delete another transaction's legitimate `repointRef(R, M2)` or a reincarnated ref, and the
  repoint path can *create* a ref yet return `false`, escaping `created_refs`. Fix: derive the exact
  committed outcome `(ns, ref, manifest_id, created?)` **inside the `appendRefOps` builder** (the
  existing `repoint_old` pattern proves this is sound — the closure sees the current committed state on
  the leader thread and same-ref mutations are lane-serialized), and **publish it into the preallocated
  per-part slot the instant `appendRefOps` confirms the commit — before any throwable post-commit work**
  (`eraseView`/cache invalidation, event logging, scratch `abandon`). Otherwise a throw in `eraseView`
  after a durable `R→M1` leaves the outcome unrecorded and the ref escapes rollback. Rollback then calls
  a conditional `dropRefIfMatches` whose read+remove happen inside **one** `appendRefOps` builder (never
  a racy post-read), removing the ref only if its current committed binding still equals this txn's
  `manifest_id`, else leaving it and reporting the conflict. Covers both prior races and the
  repoint-that-created case.
- **First-exception-wins:** the first captured worker exception is rethrown after the drain; subsequent
  ones are logged and swallowed so they never mask the original.
- **Mandatory `st.build.reset()` after `abandon`.** The repoint path abandons its scratch build; the
  normal path leaves the build set after promote. The dtor re-abandons every set build, and a *second*
  `abandon()` on the repoint scratch build throws `LOGICAL_ERROR` from a destructor (caught+logged in
  release, **aborts under `abort_on_logical_error`/ASan**). Parallelism makes late-part failures common,
  so resetting `st.build` after every `abandon`/promote is required, not optional.
- **Pre-existing residual, stated:** a wedged/`Unresolved` `promote` that later resolves Committed
  leaves a ref outside the rollback set (identical in today's sequential code — best-effort contract).
- The bounded per-blob retry loop in `putBlob` (`ABORTED`/condemned, max 8) is unchanged.

### Bundled dependency: ref-lane exception-safety (a latent bug parallelism activates)

Concurrent workers surface a pre-existing `CasRefLedger` defect that must be fixed as part of this work.
The `appendRefOps` `build_ops` closures capture the caller's stack **by reference** (`[&]`). If a leader
throws **before carving the batch** (e.g. while copying `RefTableState`), the `appendRefOps` catch resets
`leader_active`, notifies, and rethrows — but **leaves the throwing leader's own item in `rt->pending`**.
That worker unwinds, destroying the referenced stack; a follower then wakes, becomes leader, carves the
stranded item, and invokes its dangling `[&]` closure → **use-after-free**. Single-threaded commit never
hits this (no concurrent follower re-runs the stranded item); concurrent part workers make it immediate.
Fix: make the whole post-enqueue lane exception-safe — on **every** exceptional leader exit, remove and
complete every still-pending item (and once a batch is carved, a guard completes every not-yet-completed
item before unwinding), so no item is ever left neither `done` nor validly owned by a live caller;
additionally capture `build_ops` state **by value** as defense-in-depth. Gated by a two-caller
fault-injection test at the pre-carve hook (below).

## Testing

- **Invariant safety net (primary):** this is a scheduling change, so the full CA gtest battery (the
  corrected comprehensive filter) and the CA soak must stay green. Any failing invariant means the
  concurrency broke ordering or shared state.
- **Deadlock / saturation gate (catches the class TSan cannot see):** run a multi-part parallel commit
  against a commit pool of size `N` with at least `N+1` parts and ≥1 blob each; assert bounded
  completion. Deterministically proves no worker starves waiting on its own pool.
- **ThreadSanitizer gate:** a representative multi-part CAS insert under TSan for `created_refs` and
  the cross-part store-level components.
- **Determinism = semantic equivalence, NOT byte-identical.** Batching intentionally changes ref-log
  packing, `RefTxnId`s, `published_at_ms`, and incarnation tags. Assert the parallel commit yields the
  same *folded logical state* (committed ref→manifest bindings, decoded manifest entries, blob
  payloads, in-degree, clean meta) as a forced-sequential run — explicitly excluding encoding, IDs,
  audit ordering, and timestamps.
- **Rollback correctness tests:** (a) inject a `promote` failure in one part of a multi-part commit and
  assert absent-before refs are dropped and pre-existing refs untouched, under the drain; (b) T1
  promotes `R`, T2 legitimately `repointRef(R, M2)`, then T1 rolls back → assert T2's binding
  survives (`dropRefIfMatches`); (c) a scratch-`getView` then concurrent `dropRef` so the repoint
  creates a ref → assert it is tracked and rolled back; (d) a **mid-dispatch throw with one deliberately
  slow worker** → assert (via a barrier or log ordering) the drain completed *before* the first rollback
  `dropRefIfMatches` ran — this pins the join-before-rollback rule permanently.
- **Hardlink shared-pending-blob shape:** two parts referencing one staged blob (`createHardLink` →
  `adoptStagedBlob`), committed in parallel — the only cross-part coupling that survives into commit;
  exercises the concurrent same-key `putIfAbsent`/412/adopt race deterministically. Include it in both
  the determinism oracle and a TSan run.
- **Ref-lane exception-safety gate (for the bundled dependency):** a two-caller fault-injection test at
  the `appendRefOps` pre-carve hook — leader throws before carving while a follower waits — asserts no
  item is left in `pending` and every waiter completes (no use-after-free of a stranded `build_ops`).
- **Performance validation (success metric):** re-run the 500-partition benchmark. Success =
  `CasRefBatchFlushes` ≪ `CasRefBatchedMutations` (batching engaged; was 1026 == 1026),
  `CasRefQueueWaitMicroseconds` drops sharply, wall-clock approaches the standard-S3 baseline, and the
  single-query metrics attribute to the INSERT's `query_log` row (ThreadGroup propagation). Note:
  `CasRefBatchFlushes`/`CasRefBatchedMutations` accrue on whichever caller is the batch *leader*, so
  under *concurrent* inserts sharing a lane they cross-attribute between queries — a pre-existing
  property, harmless to the single-query benchmark, worth knowing when reading production metrics.

## Deferred (not in this spec)

- **Explicit intra-part blob fan-out** (for a wide *single-partition* insert, where bounded per-part
  concurrency gives little overlap). If added, blob tasks go on a *third* pool disjoint from both the
  commit pool and `getThreadPoolWriter`, the `deps`-map write is mutexed, and a blob-level wait-all
  with an atomic `alive` guard is required. Deferred because bounded per-part concurrency already
  covers the measured 500-partition case; revisit only if profiling shows a single-part bottleneck.
- Root causes #3 (promote manifest GET) and #4 (1 MiB HEAD-before-PUT) — separate BACKLOG items.
- Ref-ledger internals, the intra-part ordering protocol, and any encoded-byte/format change.
