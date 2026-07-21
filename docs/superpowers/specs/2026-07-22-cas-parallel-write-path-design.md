# CAS write path — bounded-concurrency per-part commit

- **Date:** 2026-07-22
- **Status:** design approved (revised after two independent concurrency reviews), ready for implementation plan
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
  `getThreadPoolWriter` thread, that is the same deadlock one level down. Preferred: a bounded
  `ThreadPoolCallbackRunner` over `getIOThreadPool()` (already disjoint from the writer pool, no new
  global pool), with `cas_commit_concurrency` as the real bound; a dedicated CAS commit pool is an
  acceptable alternative if isolating the blocking ref-lane waits from other IO proves necessary. The
  hard requirement is only "disjoint from `getThreadPoolWriter`".
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
- **Cross-part shared state:** `created_refs` is written by concurrent workers and is mutex-guarded.
  Its contents change per the rollback rework below.
- **Store-level components are thread-safe** (verified in review, not assumed): the dedup cache is a
  locked `CacheBase`; `getRefTableRuntime`/recovery/stale-precommit-sweep are guarded by
  `ref_queue_mutex`/`state_mutex`; `CasRequestController` state is per-call; `EventEmitter` emits into
  a `SystemLog` sink installed before traffic. Concurrent same-namespace `appendRefOps` is already
  exercised by concurrent INSERTs.
- **EDGE-BEFORE-OBSERVE is preserved.** Each worker keeps the durable
  `stageManifest → precommitAdd → uploadPendingBlobs → promote` order internally; parts are
  independent (each adopt is protected by that part's own durable precommit).

## Error handling / rollback

The reviews found the existing rollback is fragile and that concurrency makes late failures — after
many parts already published — far more likely. Rework it:

- **Part-level wait-all on every path.** `commit()` installs a `SCOPE_EXIT` that joins **every
  dispatched worker** (success or failure, including the dispatch-failure-partway case) before it
  rolls back or returns. No worker ever runs concurrently with rollback or with
  `~ContentAddressedTransaction`'s `abandon()`. This is the single hard rule that keeps the design
  safe.
- **Exact-manifest rollback (`dropRefIfMatches`).** Today `created_refs` stores only `(ns, ref)` and
  rollback `dropRef`s whatever manifest currently occupies that name — which can delete another
  transaction's legitimate `repointRef(R, M2)` or a reincarnated ref, and the repoint path can
  *create* a ref yet return `false`, escaping `created_refs` entirely. Fix: `promoteBuild`/`repointRef`
  return the exact committed outcome `(ns, ref, manifest_id, created?)` derived inside the ref-ledger
  mutation; the commit records that tuple in a preallocated, no-throw slot; rollback calls a
  conditional `dropRefIfMatches` that removes the ref only if its current committed binding still
  equals this transaction's exact `manifest_id`, and otherwise leaves it and reports the conflict.
  This also covers the repoint-that-created case (tracked because the outcome carries `created?`).
- **First-exception-wins:** the first captured worker exception is rethrown after wait-all; subsequent
  ones are logged and swallowed so they never mask the original. `created_refs` collection is
  mutex-guarded.
- **Pre-existing residuals stated, not silently inherited:** a wedged/`Unresolved` `promote` that later
  resolves Committed leaves a ref outside `created_refs` (identical in today's sequential code); the
  repoint path should reset `st.build` after `abandon` so a late failure does not double-abandon.
- The bounded per-blob retry loop in `putBlob` (`ABORTED`/condemned, max 8) is unchanged.

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
  assert absent-before refs are dropped and pre-existing refs untouched, under wait-all; (b) T1
  promotes `R`, T2 legitimately `repointRef(R, M2)`, then T1 rolls back → assert T2's binding
  survives (`dropRefIfMatches`); (c) a scratch-`getView` then concurrent `dropRef` so the repoint
  creates a ref → assert it is tracked and rolled back; (d) a scheduling failure partway through
  dispatch → assert all already-submitted workers are joined.
- **Performance validation (success metric):** re-run the 500-partition benchmark. Success =
  `CasRefBatchFlushes` ≪ `CasRefBatchedMutations` (batching engaged; was 1026 == 1026),
  `CasRefQueueWaitMicroseconds` drops sharply, wall-clock approaches the standard-S3 baseline, and the
  metrics attribute to the INSERT's `query_log` row (ThreadGroup propagation).

## Deferred (not in this spec)

- **Explicit intra-part blob fan-out** (for a wide *single-partition* insert, where bounded per-part
  concurrency gives little overlap). If added, blob tasks go on a *third* pool disjoint from both the
  commit pool and `getThreadPoolWriter`, the `deps`-map write is mutexed, and a blob-level wait-all
  with an atomic `alive` guard is required. Deferred because bounded per-part concurrency already
  covers the measured 500-partition case; revisit only if profiling shows a single-part bottleneck.
- Root causes #3 (promote manifest GET) and #4 (1 MiB HEAD-before-PUT) — separate BACKLOG items.
- Ref-ledger internals, the intra-part ordering protocol, and any encoded-byte/format change.
