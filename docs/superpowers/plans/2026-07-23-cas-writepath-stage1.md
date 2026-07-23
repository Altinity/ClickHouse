# CAS Writepath Stage-1 (Internal Upload Pool) Implementation Plan {#plan-header}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement spec `docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md` (rev.4, approved): parallel intra-part blob upload on a dedicated server pool, ledger two-phase carve, counts-only chunked flush budget, `RefOp::payload` removal, and streaming recovery — all CAS-internal, zero behavioral seams in shared code.

**Architecture:** Five independent design sections land as 14 tasks. §1 (parallel upload) decomposes into pool wiring → event dispatcher → transaction-detached upload primitive → exception-safe merge → fan-out → condemned-memory cap. §2/§3 rework `CasRefLedger` flushing (two-phase carve, then chunked flush with new caps). §4 removes the dead `payload` field wire-wide. §5 makes recovery replay streaming. A closure task re-runs the s41 baseline.

**Tech Stack:** C++ (ClickHouse), gtest (`src/Disks/tests/`), ThreadPool/`ThreadPoolCallbackRunnerLocal`, the CA scenario suite (`utils/ca-soak/scenarios`, card s41) for the re-profile leg.

**Spec:** `docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md` — the implementer of every task MUST read the spec section named in the task before starting. Where this plan and the spec disagree, the spec governs; report the conflict instead of silently picking.

## Global Constraints {#global-constraints}

- Definitive CA gtest gate (run after EVERY task): `build_asan/src/unit_tests_dbms --gtest_filter='Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'` — the short `Cas*:CA*` filter under-tests by ~80 suites and is FORBIDDEN as the cited evidence.
- Allman braces; comments state constraints, never narrate changes; error messages tell the truth about what is known vs assumed.
- NEVER `sleep`-sequence a concurrency test — latches / bounded condition-variable waits only (bounded waits carry a comment naming the bound's reason).
- Pre-release no-compat policy: no decoder tolerance for removed fields/wire words (§4); every existing pool is recreated before ship.
- "No upstream changes" boundary (spec §Area): outside `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` and `src/Disks/tests/`, ONLY these files may be touched: `src/Core/ServerSettings.cpp` (one pool-size + one memory-cap setting), server startup wiring (`programs/server/Server.cpp`), gtest wiring, and the docs named in Task 12. Anything else = STOP and report.
- New settings: `0`/uninitialized pool size rejected fail-loud at configuration time; getter throws `LOGICAL_ERROR` if the pool is used before wiring.
- Caps after Task 8 (exact values, from spec §3): `ref_txn_max_ops = 5000`; carve item cap `kMaxRefBatch = 1000`; per-op size cap `4096` bytes (admission AND decode); normal-class whole-transaction byte cap stays `20 MiB` decode-side + writer post-encode runtime throw (NOT a debug-only `chassert`); removal-class keeps `ref_removal_max_bytes = 64 MiB`, no op cap.
- Shared worktree: stage only your own files by explicit path; never `git add -A`; never push; never rebase/amend; foreground `ninja` only (no `-j`), output redirected to a log under `build_asan/`.
- Commit messages end with exactly:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01HKgdqVjZwkpWPxLyHzduPb`

## File Map {#file-map}

- Create: `.../ContentAddressed/Pool/CasBlobUploadPool.{h,cpp}` (T1), `.../Pool/CasEventDispatcher.{h,cpp}` (T2), `src/Disks/tests/gtest_cas_blob_upload_pool.cpp` (T1), `gtest_cas_event_dispatcher.cpp` (T2), `gtest_cas_upload_detached.cpp` (T3/T4), `gtest_cas_upload_fanout.cpp` (T5/T6), `gtest_cas_ref_carve.cpp` (T7), `gtest_cas_ref_chunked_flush.cpp` (T8-T10), `gtest_cas_ref_decode_bounds.cpp` (T11), `gtest_cas_recovery_streaming.cpp` (T13).
- Modify (main): `Pool/CasPartWriteTxn.{h,cpp}` (T3, T4, T6), `ContentAddressedTransaction.{h,cpp}` (T5), `Pool/CasRefLedger.{h,cpp}` (T2 emission points, T7-T10, T13), `Formats/CasRefLogFormat.{h,cpp}` (T8, T9, T11, T12), `Formats/CasTextFormat.cpp` (T11), `Formats/CasRefSnapshotFormat.{h,cpp}` (T12), `Pool/CasRefProtocol.{h,cpp}` (T12, T13), `Tools/CasFsck.cpp` (T13), `Tools/CasInspect.cpp` (T12), `Pool/CasPool.{h,cpp}` (T2, T12), `src/Core/ServerSettings.cpp` + `programs/server/Server.cpp` (T1, T6), existing payload-referencing tests (T12).

Each task below names its spec section; the task brief = this plan section + that spec section.

---

### Task 1: Server-wide CAS blob upload pool {#task-1}

**Spec:** §1, bullets "One dedicated server-wide pool" + Settings section.

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.h`, `.cpp`
- Modify: `src/Core/ServerSettings.cpp` (add `content_addressed_blob_upload_pool_size`, default `16`), `programs/server/Server.cpp` (initialize after the other global pools; shutdown symmetrically)
- Test: `src/Disks/tests/gtest_cas_blob_upload_pool.cpp`

**Interfaces (Produces):**
```cpp
namespace DB::Cas
{
/// Fail-loud lifecycle: the server (or a test) must initialize the pool before any use.
void initializeBlobUploadPool(size_t size);         /// throws BAD_ARGUMENTS on size == 0; throws LOGICAL_ERROR if already initialized
ThreadPool & blobUploadPool();                      /// throws LOGICAL_ERROR if not initialized
void shutdownBlobUploadPool() noexcept;             /// idempotent; joins outstanding tasks
bool blobUploadPoolInitializedForTest();
}
```
Pool metrics: register `CurrentMetrics::CasBlobUploadPoolThreads/...Active/...Scheduled` following the `getThreadPoolWriter` pattern in `src/IO/SharedThreadPools.cpp` (read it first; do NOT reuse its pool — spec forbids nesting on `getThreadPoolWriter`).

- [ ] **Step 1: failing test** — `TEST(CasBlobUploadPool, GetterThrowsBeforeInit)`, `InitZeroRejected`, `InitThenGetWorks`, `DoubleInitThrows`, `ShutdownIdempotent`. Use the exact API above; assert error codes (`LOGICAL_ERROR`, `BAD_ARGUMENTS`).
- [ ] **Step 2:** run the new test binary target — expect FAIL (symbols missing).
- [ ] **Step 3:** implement `CasBlobUploadPool.{h,cpp}` (a `std::unique_ptr<ThreadPool>` behind a mutex; construction with CurrentMetrics ids), ServerSettings entry, `Server.cpp` init (from config) + shutdown ordering (before the object storages are torn down — place next to the IO pools teardown), and gtest wiring: the CA test fixtures that will use the pool initialize it lazily via a `InitOnce`-style helper in the test file (`ensureBlobUploadPoolForTest(4)`).
- [ ] **Step 4:** rebuild `unit_tests_dbms`, run the new suite + full definitive gate → all PASS.
- [ ] **Step 5:** commit `ca: stage1 T1 — dedicated server-wide blob upload pool (fail-loud lifecycle)`.

### Task 2: Single reentrancy-safe event dispatcher; ledger emits outside locks {#task-2}

**Spec:** §1 bullet "Event-sink contract — one dispatcher, emission outside locks". Test 17.

**Files:**
- Create: `.../Pool/CasEventDispatcher.{h,cpp}`; Test: `src/Disks/tests/gtest_cas_event_dispatcher.cpp`
- Modify: `Pool/CasPool.h` (`emitEvent`, ~:570) and `Pool/CasPool.cpp`; `Pool/CasRefLedger.{h,cpp}` (injected sink call sites; restructure `resolveRef` (~`.cpp:190`) and every other emit-under-`state_mutex` site to collect the event and emit after unlock).

**Interfaces (Produces):**
```cpp
namespace DB::Cas
{
/// Serialized, reentrancy-safe delivery: concurrent emitters are serialized; an emission performed
/// FROM INSIDE a sink callback (reentrant) is queued and drained by the already-running dispatch
/// loop instead of self-deadlocking.
class EventDispatcher
{
public:
    using Sink = std::function<void(const CasEvent &)>;
    void setSink(Sink sink);                 /// pre-traffic only (same contract as today's setters)
    void emit(CasEvent event);               /// thread-safe, reentrancy-safe, never throws through
private:
    std::mutex mutex;
    std::deque<CasEvent> queue;
    bool draining = false;                   /// guarded by mutex; the draining thread owns delivery
    Sink sink;                               /// guarded by mutex for swap; called OUTSIDE mutex
};
}
```
Implementation shape for `emit`: lock; push; if `draining` return; set `draining`; loop {pop one, unlock, call sink, relock} until empty; clear `draining`. Sink is invoked outside `mutex`, so a reentrant `emit` from the sink lands in the queue and is drained by the same loop — no recursion, no second lock acquisition on the delivery path.

Wiring: `Pool` owns one `EventDispatcher`; `Pool::emitEvent` and the ledger's injected sink both forward into it (the ledger's sink `std::function` becomes a thin lambda calling `dispatcher.emit`). Ledger emission points restructured: build the event value under the lock if needed, `emit` after the lock scope ends. Every existing test sink (plain lambdas appending to vectors) stays valid because delivery is now serialized.

- [ ] **Step 1: failing tests** — in `gtest_cas_event_dispatcher.cpp`:
  - `SerializesConcurrentEmitters`: N threads × M emits into a vector-collecting sink; assert `N*M` entries and no torn/interleaved mutation (TSan is the real assert; the test must be in the gate so the TSan lane sees it).
  - `ReentrantSinkDoesNotDeadlock`: sink that emits a second event on first delivery; bounded latch wait (comment the bound); assert both delivered.
  - `LedgerEmissionOutsideLocks` (test 17 core): install a sink that, on delivery of a ledger event, calls a `Pool`/ledger read API that itself takes `state_mutex` (e.g. `resolveRef`); drive a real `resolveRef` emission concurrently with an upload-task emission; bounded completion = no lock-order deadlock.
- [ ] **Step 2:** run → FAIL/hang-bounded (dispatcher absent; ledger still emits under lock).
- [ ] **Step 3:** implement dispatcher; route `Pool::emitEvent` + ledger sink; move every ledger emit outside its lock (grep `emit` in `CasRefLedger.cpp`, handle each; keep event construction data captured before unlock).
- [ ] **Step 4:** rebuild; new suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T2 — single reentrancy-safe event dispatcher, ledger emission outside locks`.

### Task 3: `BlobUploadRequest`/`BlobUploadResult` + transaction-detached `uploadBlobDetached` {#task-3}

**Spec:** §1 opening bullets (contract, branch inventory, "no `PartWriteTxn` mutation", complete result per branch).

**Files:**
- Modify: `Pool/CasPartWriteTxn.h` (public types + method), `Pool/CasPartWriteTxn.cpp` (carve `putBlob`'s branch bodies into build-neutral internals; `putBlob` becomes `uploadBlobDetached` + immediate single-result merge to preserve the serial API for existing callers)
- Test: `src/Disks/tests/gtest_cas_upload_detached.cpp`

**Interfaces (Produces):**
```cpp
/// Public, CAS-owned; the transaction's PendingBlob stays private (spec §1).
struct BlobUploadRequest
{
    BlobRef ref;                             /// mint algo + digest
    BlobSourceDescriptor source;             /// local path | staged S3 key | in-memory body — mirror PendingBlob's source variants 1:1
    uint64_t declared_size = 0;
};
struct BlobUploadResult
{
    BlobRef ref;
    BlobDepRecord dep;                       /// the COMPLETE dep effect this upload contributes (no side channel)
    BlobUploadOutcome outcome;               /// enum: DedupCacheHit / HeadHit / HeadMissAdopted / FreshUpload / StagingPromoted / ResurrectedLocal / ResurrectedS3
};
/// Thread-safe w.r.t. the owning PartWriteTxn: MUST NOT read or write `build` (single-writer doc).
BlobUploadResult uploadBlobDetached(const BlobUploadRequest & req) const;
```
(If `BlobDepRecord` does not exist as a named type, extract the exact record `putBlob` currently folds into `deps` — same fields, no widening.) The primitive keeps every ordering-sensitive durable pool effect inside itself (conditional create, freshness-meta `Clean` transition ~`CasPartWriteTxn.cpp:510`, condemned resurrection incl. the S3 sanctioned same-content rewrite ~`:607`, dedup-cache read/insert, event emission via T2 dispatcher, request-controller accounting, ProfileEvents). The resurrect invariant (never GET a condemned object) is preserved by keeping each branch body intact.

- [ ] **Step 1: failing tests** — for each branch (dedup-cache hit / HEAD-first hit / HEAD-first miss + live adopt with meta point-read + backfill / fresh local streaming / S3-native staging promotion (`with_rustfs` fixture) / condemned-local / condemned-S3 resurrection): drive `uploadBlobDetached` directly against the gtest backend fixtures (reuse the builders in existing `gtest_cas_part_write*` tests), then assert (a) the returned `BlobUploadResult` is complete (dep + correct outcome enum), (b) the txn's build state is UNTOUCHED (whatever accessor the tests already use to snapshot build/deps — compare before/after), (c) backend object state matches the serial path's for the same input.
- [ ] **Step 2:** run → FAIL (method absent).
- [ ] **Step 3:** implement by MOVING branch bodies out of `putBlob` into build-neutral private helpers returning dep records; `uploadBlobDetached` composes them; `putBlob` = `uploadBlobDetached` + existing dep fold (call sites unchanged this task).
- [ ] **Step 4:** rebuild; new suite + full gate PASS (serial path still green via existing suites).
- [ ] **Step 5:** commit `ca: stage1 T3 — transaction-detached uploadBlobDetached with complete per-branch results`.

### Task 4: `mergeBlobUploadResults` — all-or-nothing merge {#task-4}

**Spec:** §1 bullet "Merge is encapsulated and strongly exception-safe". Test 16.

**Files:** Modify `Pool/CasPartWriteTxn.{h,cpp}`; extend `src/Disks/tests/gtest_cas_upload_detached.cpp`.

**Interfaces (Produces):**
```cpp
/// Applies dep records on the CALLING thread after the fan-out join. Either all results merge or
/// the build is untouched (prevalidate, then build-and-swap / provably no-throw application).
void mergeBlobUploadResults(std::span<const BlobUploadResult> results);
/// Test-only allocation-failure seam, same pattern as setGcVerbAdmitWindowHookForTest:
void setMergeHookForTest(std::function<void(size_t applied_so_far)> hook);
```

- [ ] **Step 1: failing tests** — `MergeAppliesAllDeps` (N results → build equals serial fold), `MergeFailureLeavesBuildUntouched` (test 16: hook throws `std::bad_alloc` after the first result would have applied; assert build snapshot identical to pre-merge), `MergeValidatesSizes` (conflicting duplicate declared sizes → `LOGICAL_ERROR`, build untouched).
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** implement: prevalidate all results (sizes, duplicate grouping consistency), clone-or-stage the dep container growth, apply with no-throw moves, swap.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T4 — exception-safe all-or-nothing blob-result merge`.

### Task 5: Fan-out in `uploadPendingBlobs` {#task-5}

**Spec:** §1 bullets "One task per unique ref", "Failure contract — merge nothing", "Capture and lifetime discipline". Tests 1, 2, 3, 4, 5, 6.

**Files:**
- Modify: `ContentAddressedTransaction.cpp` (`uploadPendingBlobs`, ~:242 — replace the serial loop), `ContentAddressedTransaction.h` (if a helper struct is needed)
- Test: `src/Disks/tests/gtest_cas_upload_fanout.cpp`

**Consumes:** T1 `blobUploadPool()`, T3 `uploadBlobDetached`/`BlobUploadRequest`, T4 `mergeBlobUploadResults`.

Implementation contract (all from spec — the implementer re-reads §1 in full):
1. Group `pending_blobs` by `BlobRef`; conflicting declared sizes → `LOGICAL_ERROR`; ONE `BlobUploadRequest` per unique ref; merge exactly one dep per unique ref (the duplicate-membership filter at `.cpp:248` is subsumed).
2. `ThreadPoolCallbackRunnerLocal` on `blobUploadPool()` declared INSIDE the owning scope (B90 lesson, `threadPoolCallbackRunner.h:68`): tasks capture values/owning state only; query `ThreadGroup` propagated per task; the calling thread only submits and joins (pool size 1 degenerates to serial and must not deadlock).
3. Join ALWAYS drains every task (including on a dispatch throw — the runner's scope destructor guarantees it); merge-nothing on any failure: no partial `mergeBlobUploadResults` call, publish fails, precommit abandoned via the existing path, staging cleanup unchanged.
4. ProfileEvents: add `CasBlobUploadFanoutTasks`, `CasBlobUploadFanoutBatches` counters (descriptions truthful).

- [ ] **Step 1: failing tests** (each uses latches, never sleeps):
  - test 1 `DepsEquivalenceAcrossBranches`: build a multi-blob part covering every T3 branch; run once through a serial reference (pool size 1 forced via `ensureBlobUploadPoolForTest(1)`) and once fanned out; assert identical build/deps and identical backend end state.
  - test 2 `DuplicateRefsLaunchOneTask` (staged-hardlink duplicate → 1 task via an invocation counter in a test hook; conflicting sizes rejected; condemned-S3 duplicate pair resurrects content-correctly).
  - test 3 `MergeNothingOnFailure`: one task fails (inject via a poisoned source path), one sibling succeeds; assert build pre-fan-out, precommit abandoned, then run a GC round in the test and assert the sibling's body is reclaimed (pins no-new-orphan-class).
  - test 4 `ConcurrentDedupCacheInsertion` (two tasks, same cache key, latch-crossed; TSan-clean).
  - test 5 `PoolSaturationBounded` (pool 1 and pool 2 with 8 blobs; watchdog latch `wait_for` with commented bound; self-wait fails fast).
  - test 6 `DrainPrecedesUnwind` (one failing + one latch-slowed task: failure surfaces only after join; a throw injected during dispatch still drains).
- [ ] **Step 2:** run → FAIL (serial loop still in place).
- [ ] **Step 3:** implement the fan-out per the contract above.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T5 — parallel intra-part blob upload fan-out (merge-nothing failure contract)`.

### Task 6: Condemned-local resurrection byte-weighted admission cap {#task-6}

**Spec:** §1 bullet "Condemned-local resurrection memory cap". Test 18.

**Files:** Modify `Pool/CasPartWriteTxn.cpp` (condemned-local branch, ~:631), `Pool/CasBlobUploadPool.{h,cpp}` (host the semaphore next to the pool), `src/Core/ServerSettings.cpp` (`content_addressed_condemned_upload_memory_bytes`, default `0` = derived `pool_size × 64 MiB`), `programs/server/Server.cpp`; extend `gtest_cas_upload_fanout.cpp`.

**Interfaces (Produces):**
```cpp
/// Byte-weighted admission for condemned-local body materialization. Weight = checked
/// header+payload size (known before materialization). A single blob heavier than the whole
/// capacity acquires EXCLUSIVE access (drains the semaphore, runs alone) instead of waiting forever.
class ByteWeightedSemaphore
{
public:
    explicit ByteWeightedSemaphore(uint64_t capacity_bytes);
    void acquire(uint64_t weight);     /// blocks; overweight (> capacity) waits for full drain then holds exclusively
    void release(uint64_t weight) noexcept;
};
ByteWeightedSemaphore & condemnedUploadAdmission();   /// same fail-loud lifecycle as the pool
```
Permit released immediately after `putOverwrite` returns AND the body buffer is destroyed, before event/meta work (scope the buffer so its destructor runs before `release`).

- [ ] **Step 1: failing tests** — test 18: `CondemnedCapLimitsPeakBytes` (N concurrent condemned large local blobs, capacity < N×size; a test hook samples in-flight materialized bytes — assert peak ≤ capacity; all resurrections complete) and `OverweightBlobRunsExclusively` (one blob heavier than capacity completes; assert it held exclusive access via the hook).
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** implement semaphore + wire into the condemned-local branch + settings.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T6 — byte-weighted admission cap for condemned-local resurrection`.

### Task 7: Ledger two-phase carve + validation-loop exception safety {#task-7}

**Spec:** §2 (entire section). Tests 7, 8.

**Files:** Modify `Pool/CasRefLedger.{h,cpp}` (carve ~`.cpp:1240-1265`; validation loop ~`.cpp:1340`); Test: `src/Disks/tests/gtest_cas_ref_carve.cpp`.

**Interfaces (Produces):**
```cpp
/// Test-only fault seam for the carve/validation protocol (same *ForTest pattern):
enum class CarvePhaseForTest { PlanSeenRefs, PlanBatchGrow, PlanReserveOwned, PublishPop, ValidateFinalOps };
void setCarveHookForTest(std::function<void(CarvePhaseForTest)> hook);
```
Two-phase carve exactly as spec §2: plan (may throw, mutates nothing — scan without popping, build selection, ALL reservations incl. `owned_items` capacity) then publish (no-throw pops + appends, moves only) under one continuous `ref_queue_mutex` hold; ProfileEvents deferred past the plan. Validation loop: reserve `final_ops`/`survivors` growth BEFORE applying the item to `working`; publish `working` past all throwing points.

- [ ] **Step 1: failing tests** — test 7 `CarveThrowLeavesQueueIntact`: for EACH plan-phase hook point, inject `std::bad_alloc`; assert every already-selected item either completes normally on retry or was never popped, and no waiter hangs (bounded latch). Test 8 `ValidationAllocFailureLeavesWorkingClean`: hook at `ValidateFinalOps`; assert the failed item's effects absent from `working` and from the encoded transaction (decode the committed object in the test).
- [ ] **Step 2:** run → FAIL (today's interleaved carve loses popped items — the test must demonstrate the hang/loss deterministically via the hook).
- [ ] **Step 3:** implement both fixes.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T7 — two-phase ref carve + validation-loop exception safety`.

### Task 8: Counts-only caps + per-op bound; oversized fails alone {#task-8}

**Spec:** §3 bullets 1, "single item exceeds cap fails alone", "per-op size cap". Tests 10, 12 (canonical round-trip leg).

**Files:** Modify `Formats/CasRefLogFormat.h` (`ref_txn_max_ops` 1000→5000, at :71), `Pool/CasRefLedger.h` (`kMaxRefBatch` 128→1000, at :357) and the admission path in `CasRefLedger.cpp` (per-op encode-one-op check, 4096 B), `Formats/CasRefLogFormat.cpp` (decoder enforces the same per-op bound); Test: `src/Disks/tests/gtest_cas_ref_chunked_flush.cpp` (started here, extended in T9/T10).

- [ ] **Step 1: failing tests** — test 10 `OversizedItemFailsAlone` (item with > 5000 ops completed with error, neighbors commit; its ops never enter a chunk) and `OversizedOpFailsItsItemAlone` (one op > 4 KiB via a maximum-length ref name — `checkCanonicalRefName` imposes no length limit; neighbors commit); test 12 leg `CanonicalMaxTransactionRoundTrips` (5000 × 4 KiB ops = 20,480,000 bytes round-trips; the byte check uses the existing strict-greater convention).
- [ ] **Step 2:** run → FAIL (caps still 1000/128; no per-op bound).
- [ ] **Step 3:** implement cap bumps + per-op admission check (encode exactly one op, no accumulation) + decoder-side per-op bound.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T8 — counts-only admission caps (5000 ops, 4 KiB/op), oversized fails alone`.

### Task 9: Removal-class detection by op inspection {#task-9}

**Spec:** §3 bullet "Removal-class is exempt, detected by op inspection". Test 11.

**Files:** Modify `Pool/CasRefLedger.cpp` (admission classification; the codec already classifies via `RemoveNamespace` — `Formats/CasRefLogFormat.cpp:51`); extend `gtest_cas_ref_chunked_flush.cpp`.

- [ ] **Step 1: failing tests** — test 11, falsifiable: `DropNamespaceOverOpCapSucceeds` (> 5000 refs, byte-budgeted under 64 MiB, no op cap) and `SyntheticWholeShardNonRemovalRejected` (a SYNTHETIC `WholeShard`-scoped item with > 5000 NON-removal ops → rejected by the op cap; the production stale-precommit sweep self-limits (`CasRefLedger.cpp:1964`) so only a synthetic item pins the discriminator).
- [ ] **Step 2:** run → FAIL if classification uses scope (`WholeShard`) anywhere in admission.
- [ ] **Step 3:** implement: removal-class iff built ops contain `RemoveNamespace`; keep singleton carve + 64 MiB byte budget for that class.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T9 — removal-class exemption keyed on op inspection, not scope`.

### Task 10: Chunked flush with complete per-chunk commit boundary {#task-10}

**Spec:** §3 bullet "Chunked flush…" (the whole long bullet: `commitChunk`, reseed, tenure exception containment, snapshot-trigger coalescing). Test 9 with ALL variants. THE HARDEST TASK — implementer reads spec §3 verbatim first; reviewer = strongest available model.

**Files:** Modify `Pool/CasRefLedger.{h,cpp}` (leader flush loop; outer catch ~`.cpp:996`; publication single-flight ~`.cpp:1589`, settlement ~`.cpp:1635`); extend `gtest_cas_ref_chunked_flush.cpp`.

Contract highlights the tests must pin (from spec, abbreviated — spec governs):
- `commitChunk` = the FULL committed arm (real txn id, encode, `PUT`, apply to `rt->state` under `state_mutex`, tail count/bytes, per-txn metrics, complete THIS chunk's survivors with the REAL id, wake waiters, schedule snapshot) then reseed `working` + trial-id high-water mark from live state.
- Overflowing item validated exactly once, in the chunk where it lands; `owned_items` untouched across chunks; several ref-log transactions per tenure.
- Failure isolation: chunks < N stay committed; only chunk N + unattempted remainder fail; wedge contains only chunk N; holds on EVERY throwable exit.
- Tenure exception containment: after any committed chunk, later exceptions (incl. reseed throw) fail only incomplete owned items; baton retained until carved remainder completed; the leader's own call returns its chunk-1 committed result instead of rethrowing chunk-2's error.
- Snapshot coalescing: settlement re-evaluates the accumulated tail (or remembers + re-fires a pending trigger) so a publisher that captured chunk 1's prefix cannot suppress chunks 2..N.

- [ ] **Step 1: failing tests** — test 9: `ChunkedFlushCommitsPerChunk` (carve > 5000 ops total ⇒ ≥ 2 txns in one tenure; per-chunk assertions: committed ids, tail counters, per-chunk metrics, snapshot scheduling, follower wakeups; `build_ops` at-most-once via invocation counters; folded state equals sequential result); `ChunkFailureDefinite` / `ChunkFailureWedge` (wedge contains only chunk-2 items) / `ChunkFailureThrow` — in all three chunk-1 callers observe SUCCESS with chunk 1's real id; `LeaderOwnItemCommittedBeforeThrow` (leader returns success, not chunk-2 error); `SnapshotPublisherLatchedAcrossChunks` (publisher latched after chunk-1 prefix; assert a follow-up publication covers later chunks — no lost trigger).
- [ ] **Step 2:** run → FAIL.
- [ ] **Step 3:** implement per spec; keep the existing committed-arm code as the body of `commitChunk` (refactor, don't duplicate).
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T10 — chunked ref flush: complete per-chunk commit, tenure containment, snapshot coalescing`.

### Task 11: Decode-side byte bounds; `object_cap` for raw bodies {#task-11}

**Spec:** §3 bullet "Byte limits…". Test 12 (remaining legs).

**Files:** Modify `Formats/CasTextFormat.cpp` (`openObject` ~:384 — enforce `object_cap` for raw/uncompressed bodies, today zstd-only), `Formats/CasRefLogFormat.cpp` (`checkBudget` decode-side 20 MiB stays; writer post-encode RUNTIME throw — not `chassert`); Test: `src/Disks/tests/gtest_cas_ref_decode_bounds.cpp`.

- [ ] **Step 1: failing tests** — `RawOverCapObjectRejected` (uncompressed body over `object_cap` → decode error; today it passes = RED), `PaddedNormalTxnOver20MiBRejected` (pad via TOLERANT meta/trailer records — each op individually legal; padding an op line would only trip the 4 KiB per-op cap and prove nothing), `WriterPostEncodeThrowIsRuntime` (force an over-cap encode through a test seam; assert a real exception in release-mode semantics, i.e. the check is an `if+throw`, not `chassert`).
- [ ] **Step 2:** run → RawOverCap RED (the gap), others per current behavior.
- [ ] **Step 3:** implement both closures.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T11 — object_cap for raw bodies; decode-side 20 MiB bound pinned`.

### Task 12: `RefOp::payload` removal + `SetPublishedAt` rename, repo-wide {#task-12}

**Spec:** §4 (entire section — the consumer inventory there is the checklist). Test 13.

**Files:** Modify `Formats/CasRefLogFormat.{h,cpp}` (drop `payload` from `RefOp`; `SetPayload`→`SetPublishedAt`, wire `set_payload`→`set_published_at`), `Pool/CasRefProtocol.{h,cpp}` (`RefPayloadUpdate`→`RefPublishedAtUpdate` at h:111; `applySetPayload` consumer), `Formats/CasRefSnapshotFormat.{h,cpp}` (committed row :41), `Pool/CasPartWriteTxn.cpp` (promote op ~:1089), `Pool/CasRefLedger.cpp` (state-growing classification ~:1321), `Pool/CasPool.{h,cpp}` (forwarding update API rename `updateRefPayload`→`updateRefPublishedAt`), `Tools/CasInspect.cpp` (:137, :185), docs: `docs/superpowers/cas/cache.md` (8 refs), `docs/superpowers/cas/03-writer-protocol.md` (1 ref), stale comment `ContentAddressedTransaction.cpp:427`; every test using `payload` as a byte carrier (rewrite against real fields or drop).

- [ ] **Step 1: failing tests** — test 13 pins in the codec suites: `set_published_at` wire word present, NO `payload` key in ref-log or snapshot encodings (assert absence in encoded text), `CasInspect` renders the renamed op, codec round-trips.
- [ ] **Step 2:** run → FAIL (old wire word).
- [ ] **Step 3:** implement removal + renames; then the mandated repo-wide sweep: `grep -rn 'updateRefPayload\|RefPayloadUpdate\|SetPayload\|set_payload\|RefOp.*payload'` over `src/ docs/ utils/` must return ZERO relevant hits (cite the grep in the report). No decoder tolerance for the old field.
- [ ] **Step 4:** rebuild; full gate + encoding-pin suites PASS.
- [ ] **Step 5:** commit `ca: stage1 T12 — remove RefOp payload; SetPayload -> SetPublishedAt everywhere`.

### Task 13: Streaming recovery with candidate discipline {#task-13}

**Spec:** §5 (entire section). Tests 14, 15.

**Files:** Modify `Pool/CasRefLedger.cpp` (replay ~:404-423, publication inventory ~:518-541), `Pool/CasRefProtocol.{h,cpp}` (in-place builder near the poisoning path ~:361; `recoverRefTableDetailed` ~:667), `Tools/CasFsck.cpp` (snapshot oracle ~:209), `Formats/CasFormat.cpp` (stale 1 MiB comment ~:80); Test: `src/Disks/tests/gtest_cas_recovery_streaming.cpp`.

**Interfaces (Produces):**
```cpp
/// Owns a private candidate RefTableState; applies decoded transactions IN PLACE; discards the
/// candidate on any failure. Never touches live rt.state.
class RefReplayBuilder
{
public:
    void applyOne(RefLogTxn && txn);                 /// decode/apply corruption propagates fast (non-transient class)
    RecoveryResult finish() &&;                      /// complete publication payload
};
struct RecoveryResult
{
    RefTableState state;
    /* cleanup markers, newest snapshot identity, tail count/bytes, base_snapshot_bytes,
       admission budgets, needs_stale_precommit_sweep, recovery-seal facts —
       exact field list mirrors CasRefLedger.cpp:518-541 (the reference inventory). */
};
```
Failure split: vanished selected object (LIST/GET race) → discard candidate, bounded re-LIST restart (existing behavior); decode/apply corruption → discard candidate, fail fast (no re-LIST loop). Installation atomic under `state_mutex`; `recovered` set LAST, waiters notified only after complete publication. Orphan-sweep recovery and fsck oracle stream through the SAME builder.

- [ ] **Step 1: failing tests** — test 14: `LongTailReplaysUnderMemoryBound` (tail of maximum-op-count txns; harness asserts peak RSS delta / allocation counter below a bound the old whole-tail vector provably exceeds — compute the bound from txn sizes and assert the old impl fails it in the RED demo), `MidTailVanishedObjectReLists` (bounded restarts), `CorruptObjectFailsFast` (no re-LIST loop — assert LIST count), `ConcurrentWaiterUnblockedOnce`, `OrphanSweepAndFsckSameBound`. Test 15: `RecoveryResultInventoryComplete` — recover a table with stale predecessor precommits + non-trivial snapshot base; assert EVERY `RecoveryResult` field against pre-change behavior (state, cleanup markers, snapshot identity, tail count/bytes, `base_snapshot_bytes`, admission budgets, `needs_stale_precommit_sweep`, recovery-seal facts).
- [ ] **Step 2:** run → FAIL/RED-demo the memory bound against the current materializing implementation.
- [ ] **Step 3:** implement builder + three call sites + comment fix.
- [ ] **Step 4:** rebuild; suite + full gate PASS.
- [ ] **Step 5:** commit `ca: stage1 T13 — streaming recovery replay with candidate discipline and complete RecoveryResult`.

### Task 14: Closure — gate, stateless family, soak leg, s41 re-profile, docs {#task-14}

**Spec:** §Testing "Integration / soak" + spec-vs-code truthfulness.

**Files:** run-only + docs: `docs/superpowers/cas/BACKLOG.md` (close/annotate the write-path HIGH item for stage 1), report `docs/superpowers/reports/2026-07-23-cas-wide-insert-stage1-effect.md`.

- [ ] **Step 1:** full definitive gate on a fresh `build_asan` rebuild → cite `N/N PASSED` post-rebuild.
- [ ] **Step 2:** stateless CA family via praktika: `python3 -m ci.praktika run "Stateless tests (arm_binary, parallel)" --test "05020_content_addressed_fsck 04290_content_addressed_no_leftovers 04295_content_addressed_mutation_no_leftovers 01271_show_privileges 05019_content_addressed_fsck_access 05022_content_addressed_verb_access"` (binary symlink must point at a rebuilt `build/`).
- [ ] **Step 3:** short TXN/GC soak leg (phase-3 style `--duration 15m` per `reference_ca_soak_duration_phase3`) — green, no `Failed` GC rounds.
- [ ] **Step 4:** re-profile: rerun scenario s41 (`python3 -m scenarios.run --scenario s41_wide_insert_baseline --seed 1` from `utils/ca-soak`) on the rebuilt release binary; compare against the 2026-07-23 baseline report — expected: blob-upload segment shrinks (bounded, ~2 blobs/part on this profile); `CasRefBatchFlushes`/`CasRefBatchedMutations` ~1.0 BY DESIGN (stage-2 metric — do not chase it); write the comparison report.
- [ ] **Step 5:** spec-vs-code drift check on the five spec sections; fix stale comments; commit `ca: stage1 T14 — closure: gate + family + soak + s41 re-profile report`.

## Self-Review {#self-review}

- Spec coverage: §1 → T1-T6; §2 → T7; §3 → T8-T11; §4 → T12; §5 → T13; Settings → T1/T6; Testing 1-18 → mapped in-task (1-6→T3/T5, 16→T4, 17→T2, 18→T6, 7-8→T7, 9→T10, 10→T8, 11→T9, 12→T8/T11, 13→T12, 14-15→T13); integration/soak → T14. No orphan spec requirement found.
- Type consistency: `BlobUploadRequest/Result` defined in T3, consumed in T4/T5; `blobUploadPool()` defined T1, consumed T5; `ByteWeightedSemaphore` defined T6 only; `RefReplayBuilder`/`RecoveryResult` defined T13 only. Names match across tasks.
- Placeholders: none — every step names exact files, symbols, and expected RED/GREEN outcomes; exact code is given for public interfaces, and branch-body moves are specified as refactors of named existing code (`putBlob` branches, committed arm), which the spec pins by file:line.
