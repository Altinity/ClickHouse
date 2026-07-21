# CAS write path — parallel per-part commit + parallel per-blob upload

- **Date:** 2026-07-22
- **Status:** design approved, ready for implementation plan
- **Area:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (`ContentAddressedTransaction` commit path)
- **Backlog item:** `utils/ca-soak/scenarios/BACKLOG.md` — *"OPTIMIZATION OPPORTUNITY (write-path
  latency, HIGH) — CAS-on-S3 INSERT ~7.6× slower than standard S3"* (logged 2026-07-21), root causes
  #1 (sequential per-part commit) and #2 (sequential per-blob upload).

## Problem

A CAS-on-S3 INSERT of a 500-partition wide part (10M rows, 30 columns) measured **170.6 s** versus
**22.4 s** for the same insert on a standard S3 `ReplicatedMergeTree` — 7.6× slower despite issuing
FEWER `PutObject` (2566 vs 6156 per node). Confirmed from `system.query_log` ProfileEvents, the
slowdown is serialization, not bandwidth: `RealTimeMicroseconds` 802.6 s over ~4.7 threads,
`S3WriteMicroseconds` 117.5 s, `CasRefQueueWaitMicroseconds` 36.2 s, and the smoking gun
`CasRefBatchFlushes` = 1026 == `CasRefBatchedMutations` = 1026 (ref-ledger batch size exactly **1.0**).

Two nested serial loops in `ContentAddressedTransaction` cause it:

1. **Sequential per-part commit** — `commit()` (`ContentAddressedTransaction.cpp:396-404`) drives all
   ~500 parts through `publishStaging` one fully after another. Being single-threaded, it feeds the
   ref-ledger one item at a time, so `CasRefLedger::appendRefOps`' leader/follower batching (which
   collapses concurrent same-namespace appends into one S3 round trip) has nothing to batch — hence
   batch size 1.0, ~1026 serial ref-log PUTs (513 `precommitAdd` + 513 `promote`) plus 36 s of queue
   wait.
2. **Sequential per-blob upload** — `uploadPendingBlobs` (`ContentAddressedTransaction.cpp:241-283`),
   a plain `for` over a part's ~20-60 column blobs, each `putBlob` blocking. The standard
   `DiskObjectStorage` path fans column uploads across its I/O thread pool.

## Goal and non-goals

Parallelize both loops so that (a) concurrent parts fill the ref-ledger batch queue (engaging the
existing batching — the largest win) and (b) blob uploads overlap. Output must stay byte-identical
and every CAS invariant must hold — this is purely a scheduling change.

Non-goals (separate BACKLOG items, not touched here): the unconditional promote manifest GET
(root cause #3), the 1 MiB HEAD-before-PUT gate (root cause #4), and any change to the ref-ledger
internals or the intra-part ordering protocol.

## Constraints

- **Byte-identical, invariant-preserving.** No change to encoded bytes, manifest content, ref
  protocol, or the EDGE-BEFORE-OBSERVE ordering. The output durable state must be independent of
  scheduling order.
- **Intra-part ordering is a hard invariant.** Within each part the sequence
  `stageManifest → precommitAdd → uploadPendingBlobs → promote` (EDGE-BEFORE-OBSERVE, TLA+-guarded
  "Gate A") must be preserved exactly. Parallelism is only *within* the blob-upload phase and
  *across* parts — never a reorder of a part's four steps.
- **No hand-rolled concurrency limiter.** Reuse ClickHouse's shared IO thread pool; the pool's
  thread count bounds concurrency. Submit tasks via a callback-runner that runs inline under
  backpressure, which also avoids the nested-pool deadlock (a part task waiting on its blob tasks on
  the same pool).
- Branch: a new task branch off the current work; never commit to `master`; no rebase/amend.

## Design

### Two-level parallelism over the shared IO pool

Both loops fan out onto ClickHouse's shared IO thread pool through a callback-runner
(`ThreadPoolCallbackRunner`-style: `scheduleOrThrow` with inline execution under backpressure). The
join for each fan-out happens on the *submitting* thread, not on a pool thread, so the only nested
case — a per-part task's blob fan-out — is made deadlock-safe by the runner's inline fallback rather
than by a semaphore.

1. **Per-part** (`commit()`): replace the sequential `for (auto & [key, st] : parts)` with a
   snapshot of the part keys dispatched as per-part `publishStaging` tasks, joined on the calling
   thread. Each task runs the unchanged four-step `publishStaging` internally.
2. **Per-blob** (`uploadPendingBlobs`): replace the sequential `for (const auto & pb :
   st.pending_blobs)` with per-blob `putBlob` tasks, joined before the function returns (so the
   part's `promote` still runs strictly after all its blobs land).

### Correctness surface

- **Ref-ledger batching engages for free.** Concurrent parts call `precommitAdd`/`promote`
  simultaneously; `appendRefOps` enqueues each into `rt->pending` and one caller becomes leader and
  `flushRefBatch`-es all pending same-namespace items in one append. `appendRefOps` does not submit
  to the IO pool (it has its own leader/queue), so ref appends never nest into the pool.
- **The one genuinely new race: the per-build `deps` map.** Every `putBlob` records
  `deps[ref] = DepEntry{...}` on the shared `PartWriteTxn` (`CasPartWriteTxn.cpp`, `observeAndAdmit`
  and the `uploadFromSource` success path). `deps` is a `std::map`; concurrent inserts — even at
  distinct keys — are a data race. Resolution (chosen): keep the S3 HEAD/PUT (the slow 99%) fully
  parallel and guard only the trivial `deps` write with a per-build mutex; contention is negligible
  because the write is nanoseconds while every thread spends its time in the S3 round trip. (A
  post-join merge of per-task `DepEntry` results is an equivalent alternative if a profile ever shows
  lock contention, which it should not.) Per-part tasks (#1) do **not** share a `deps` map — each
  part owns its own `PartWriteTxn` — so this race is unique to #2.
- **Cross-part shared mutable state:** `created_refs` (rollback tracking) is written from multiple
  part tasks and must be guarded (mutex or concurrent collection).
- **Store-level shared components** — the dedup cache (`dedupCacheContains`/`dedupCacheAdd`), the
  event log (`EventEmitter`), per-hash meta ops (`loadMeta`/`putMetaIfAbsent`), the ref-ledger, and
  the S3 backend — are shared across all parts and blobs. These are almost certainly already
  thread-safe because concurrent INSERTs into CAS tables already exercise them (two queries = two
  concurrent transactions against one store). The design must **explicitly verify** the dedup-cache
  and event-log locking rather than assume it; if either is not thread-safe, add the minimal lock.

## Error handling / rollback

Preserve the existing partial-commit rollback contract (`commit()`'s try/catch: on failure,
best-effort `dropRef` the refs this commit created that were absent before, then rethrow; pre-existing
refs are never dropped). Under concurrency:

- A blob-task exception propagates to its part (the blob join surfaces the first error, the part
  fails); a part-task exception propagates to `commit()`.
- **Wait-all, then roll back.** On the first part failure, `commit()` does not abort in-flight parts
  mid-step (a half-completed `promote` could leave inconsistent state). It waits for every dispatched
  part task to finish (each completing or failing cleanly), then collects the full `created_refs` set
  and best-effort `dropRef`s the absent-before subset. This keeps the existing property that each
  publish is individually gate-checked and journalled and that leftover uploads are GC-reclaimable
  debris.
- **First-exception-wins:** the first captured exception is rethrown; subsequent exceptions are
  logged and swallowed so they never mask the original. `created_refs` collection is mutex-guarded.
- The bounded per-blob retry loop in `putBlob` (`ABORTED`/condemned, max 8 attempts) stays per-blob,
  unchanged.

## Testing

- **Invariant safety net (primary):** because this is a scheduling-only change, the full CA gtest
  battery (the corrected comprehensive filter) and the CA soak must stay green. Any failing invariant
  test means the parallelization broke ordering or shared state.
- **ThreadSanitizer gate (key new test):** run a representative multi-part CAS insert under TSan to
  catch data races on the `deps` map, `created_refs`, and the store-level dedup cache / event log.
  This is a required new gate.
- **Determinism:** a multi-part insert must produce byte-identical durable state (refs, manifests,
  meta) regardless of scheduling — assert the parallel commit yields the same refs/manifests as a
  forced-sequential run.
- **Failure-injection rollback:** inject a `promote` failure in one part of a multi-part commit and
  assert, under concurrency, that the absent-before refs are dropped and pre-existing refs are
  untouched (the wait-all-then-roll-back path).
- **Performance validation (success metric):** re-run the 500-partition benchmark. Success =
  `CasRefBatchFlushes` ≪ `CasRefBatchedMutations` (batching engaged; was 1026 == 1026),
  `CasRefQueueWaitMicroseconds` drops sharply, and wall-clock approaches the standard-S3 baseline.
  Additionally a single-wide-partition insert to validate #2 (intra-part blob parallelism) on its own.

## Out of scope

- Root causes #3 (promote manifest GET) and #4 (1 MiB HEAD-before-PUT) — separate BACKLOG items.
- Ref-ledger internals, the intra-part ordering protocol, and any encoded-byte/format change.
