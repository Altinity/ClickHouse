---
description: 'Design for CAS-29.9 (r10): on a folding round the namespace janitor lists pages on the round thread and hands each page''s dead-life `_log`/`_snap` deletes to the GC I/O pool as jobs (storage-sized batches, or single keys on a store without batch delete), listing the next page while they run, and starts no new page after a best-effort 20 s budget, so a dropped table drains in one or two folding rounds instead of one page per folding round.'
---

# CAS GC: janitor batch deletes under a phase time budget {#cas-gc-janitor-batches-and-budget}

## Decision {#decision}

- **Pages listed on the round thread, deletes fanned out on the GC I/O pool.** LIST is sequential by
  continuation token; nothing else is. Per page the round thread reads authority, LISTs, reads the
  catalog, classifies, runs the exact-token `_ckpt`/`_files` deletes, and submits the page's dead
  write-once deletes to `io_pool` as jobs. It then lists the next page while those jobs run
  ([pipeline](#pipeline)).
- **A job is one storage request.** Capability true or unknown: one batch of the phase's batch size.
  Capability false: one key. `DeleteObjects` is idempotent and reports per-key errors, and pages are
  disjoint, so concurrent batches are safe.
- **Jobs run on the GC I/O pool, bounded by its threads** (`gc_io_concurrency`, default 16). The pool queue
  is unbounded, as the redelete relies on. Each job is admitted on its own `CasOperation` resumed from
  the phase's generation with the janitor's authority predicate, as `Gc::redeleteBlobs` resumes its jobs.
- **Settle per page, one publication.** A page's jobs are settled after the next page is listed, and once
  after the last page. The phase then publishes the cursor of the longest prefix of complete pages, once.
- **Batches, storage-sized.** A batch holds `bulkDeleteChunkKeys()` = clamp(min(`gc_bulk_delete_chunk_keys`,
  the storage's batch-delete limit), 1, `kBulkDeleteMaxKeys`) keys, fixed at phase start. One CAS batch is
  one admitted request. It is one storage request, except that a credential-refresh reissue
  (`refreshAndClassifyReadFault` in `CasRequests.cpp`, and
  `S3ObjectStorage::refreshAndRetryOnExpiredCredentials` inside the storage call) sends it again without a
  new admission. With the default 1000 and a 1,000-key LIST page, a page is one batch.
- **Authority per cohort, requests per storage.** Authority is read once per cohort: once per page in the
  janitor, once per 1,000-key cohort in `cleanupRefObjects`, as today. A store without batch delete pays
  one request per key, never one authority read per key.
- **Best-effort budget.** Constant `kJanitorPhaseBudget` = 20 s, inside the owner's 15–30 s range; the
  owner may set 30 s. It is a soft limit, checked between pages: once elapsed ≥ budget the phase starts
  no new page. The page in progress and the jobs in flight complete under their standard retry policy.
  No hard ceiling is promised ([budget and in-flight work](#phase-bound)).
- **Capability read once per phase, before any job; the batch size is fixed with it.** False → one-key
  jobs. Unknown → multi-key jobs like true; no sequential probe. `S3Capabilities` remembers the answer for
  the object storage, so only the first phase a process runs on the disk can probe. A rejection stops
  the phase; the next folding round reads false.
- **No per-key recovery of a failed batch, anywhere.** `removeChunkWriteOnceOrOneByOne` loses its
  catch-and-loop. Keys of a rejected batch are never re-sent in the phase. Both other write-once families
  contain the propagated `NOT_IMPLEMENTED` themselves.
- **Failed request leaks and advances.** Once its retry policy is exhausted, an ordinary delete failure,
  transient or permanent, counts its keys as `leaked` and its page stays complete, as the exact-token
  path does today. Only lost authority, `NOT_IMPLEMENTED`, a deterministic local failure and a schedule
  refusal hold the page. A key that fails every time is leaked on every pass that reaches it and shows in
  `batches_leaked`/`leaked`; the next full pass retries it.
- **S3 adapter keeps the canonical error name** at all three throw sites, the one-key path included, so
  a name-only refusal ends its CAS retry at once instead of spending the whole retry window. With a
  credential-refresh callback configured, a refusal the storage classifies as an expired token is
  reissued once by the refresh path before it leaks ([adapter](#adapter)).
- **Deferred rounds unchanged.** A `DEFER` round takes one suppressed page: no deletes, no jobs, no
  cursor move.
- **`gc_io_concurrency` 1 runs the jobs one at a time on one pool thread.** They overlap the next page's
  LIST. Inline execution on the round thread exists only for unit tests that pass no pool.
- **A refresh never publishes a transient `false`.** Jobs read the authority flag while the round thread
  refreshes it, so `refreshAuthority` keeps the confirmed value during its GET and stores `false` only on
  absence, mismatch or a failed read. An observed loss is final for the phase because the loop breaks on
  the first loss a refresh reports and never refreshes after it ([authority](#authority)).
- **Proof before merge.** Fresh alternating A/B on `e2dd0f5d343`, all on the local RustFS stand.
  Predicted: a 20k-key stream drains in one folding round after the DROP on S3, and in at most two with
  the disk's `support_batch_delete` set to false at 20 ms RTT. Quiet pools keep one logical LIST page per
  folding round and no more physical LIST requests than base. Request counts come from process-wide
  `system.events` deltas over a GC-only window.

## Context {#context}

Figures from `docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/08-janitor-batches-on-long-streams-and-memo-reuse-distance.md`.
They are **hypotheses, not a baseline**: report 08's BEFORE was `61d24118251`, not `e2dd0f5d343`, and its
AFTER carried the manifest memo as well as batches. [Value proof](#value-proof) re-measures both sides.

| | report 08 before (`61d24118251`) | report 08 after (batches + memo) |
|---|---|---|
| janitor DELETE requests, 7k / 20k stream | 2,333 / 15,913 | 3 / 16 |
| janitor phase, 7k at 20 ms RTT | 60.8 s | 5.3 s |
| janitor time per page, no latency / 20 ms | 1.0–1.1 s / 24 s | 0.4–0.6 s / 1.9–2.3 s |
| drain wall time, 7k / 20k | 112 s / 1,307 s | 112 s / 1,307 s |

Drain time does not move with batches alone. `Gc::runNamespaceJanitorPage` runs one page per round, and
only folding rounds delete. `shouldDeferRound` defers up to `gc_fold_max_defer_rounds` (8) rounds in a row,
so after a DROP the janitor deletes on rounds 2 and 3 and then on every 9th round. Drain time is
`pages × 9 × gc_interval_sec`.

The one-page cap bounded round time when a page cost ~1,005 requests (one exact DELETE per key). With a
batch, a page costs ~8 requests on S3.

**Open: report 08's per-page time.** The request model below predicts ~140 ms of round-thread time per
page at 20 ms RTT. Report 08 measured 0.4–0.6 s without latency and 1.9–2.3 s at 20 ms, unexplained. If
the unexplained time is the batch request itself, the pipeline hides it behind the next LIST; if it is
the round thread's own work, it does not. Without latency either fits a 20k drain in one 20 s window.
M1 measures it; nothing below depends on the model being right except the predicted round count.

## Execution model {#execution}

### Pipeline {#pipeline}

`NamespaceJanitor::run`, on the round thread:

1. Read the maintenance state once. Read `isBatchDeleteSupported()` once and fix the phase's mode and
   batch size ([capability](#capability)).
2. Per page, while the [stop rules](#stop-rules) allow:
   1. `refreshAuthority`. Authority lost: no further page.
   2. LIST one page (1,000 keys) from the in-memory cursor. Then settle the previous page's jobs.
   3. Read the catalog cut after the LIST, as today, and classify every key.
   4. Delete dead `_ckpt`/`_files` by exact token, as today, on the round thread.
   5. Cut the dead canonical `_log`/`_snap` keys into jobs of the phase's batch size and submit them to the
      GC I/O pool.
3. Settle the last page's jobs. Compute the complete prefix. Publish the cursor once.

A page in progress is finished: its jobs are all submitted even after the budget passes. The budget only
decides whether step 2 starts another page.

| Request | S3, per page | GCS (no batch delete), per page |
|---|---|---|
| authority GET, catalog GET (round thread) | 2 | 2 |
| LIST, physical `ListObjectsV2` (round thread waits) | 3 for a full page with more keys after it; 2 for the page before the last; 1 for the last | same |
| exact `_ckpt`/`_files` deletes (round thread) | a few (listing carries tokens) | a few |
| dead `_log`/`_snap` deletes (`io_pool`) | 1 batch job | 1,000 one-key jobs, 16 in flight |
| **time at 20 ms RTT** | **round thread ~7 requests ≈ 140 ms; the batch overlaps the next page** | **1,000 / 16 × 20 ms ≈ 1.25 s of jobs; the round thread's ~140 ms overlaps them ≈ 1.3 s** |

One logical page costs three `ListObjectsV2` (report `06-list-requests-per-page.md` in the same folder,
measured on RustFS): the response holding the page, the next response for the sentinel key that proves
more keys exist (`CasObjectStorageBackend.cpp:1095`), and the prefetch the async iterator schedules
when that response arrives (`ObjectStorageIteratorAsync.cpp:71`) and its destructor waits for. The round
thread blocks on all three, but they run on the iterator's threads. This change keeps the page contract
and the iterator as they are.

Keys per 20 s budget at 20 ms RTT:

| Store | Pages per phase | Keys per phase | Base, per folding round |
|---|---|---|---|
| S3, batch delete | ~140 (round-thread bound, ~140 ms per page) | ~140k | 1 page, ~24 s of serial exact deletes (report 08) |
| GCS, no batch delete, `gc_io_concurrency` 16 | ~15 (job bound, ~1.3 s per page) | ~15k | 1 page, ~20 s of serial one-key deletes |
| any store, `gc_io_concurrency` 1 | S3 ~125 (160 ms per page); no batch delete 1 (~20 s per page) | ~125k; 1k | same as base on a no-batch store |

On S3 the pipeline saves only the batch's own time per page: the round thread's LIST and GETs are the
bound. On a no-batch store the jobs are the bound and the pipeline is what makes the page ~1.3 s.

### Jobs on `io_pool` {#jobs}

**Shape: `Gc::redeleteBlobs` (`CasGc.cpp:488`).**

- **Submit.** A `ThreadPoolCallbackRunnerLocal<void>` on `io_pool` with its own thread name, one per
  phase. Each job through `enqueueAndGiveOwnership`; the handles are kept, and `SCOPE_EXIT_SAFE` waits for
  all of them on every exit path, a throw included. At `gc_io_concurrency` 1 jobs run one at a time on one
  pool thread; with no pool (unit tests only) they run inline on the round thread.
- **Bound.** The pool's threads bound the running jobs. The pool queue stays unbounded, as the redelete and
  `GcReadAhead` rely on.
- **Admission per job.** Each job builds its own operation with
  `requests.resume(op.generation(), liveness)` and sends `removeManyWriteOnce(keys, Retry::standard())`
  on it. The redelete resumes without a predicate; the janitor passes its `authority_held` predicate, so
  every job is admitted against the mount fence and GC authority. `refreshAuthority` now runs on the
  round thread while jobs read the flag: **`Gc::authority_held` becomes `std::atomic<bool>`**
  (`CasGc.h:991`), and a refresh keeps its value during the GET ([authority](#authority)).
- **Collect.** Each job writes one slot of its page: outcome (deleted, leaked, held, skipped) and its
  exception. At a page's settle the round thread waits for its jobs, calls `future.get()` per handle, as the
  redelete does, and aggregates in submit order. Every logical count on the row comes from the slots, read once;
  a job run inline (no pool, unit tests) writes its slot like any other and adds nothing on the
  round thread, so nothing is counted twice. A failure is reported per job, one WARNING each, as
  `reportRedeleteFailure` does per entry.
- **Stop flag.** A phase-wide `std::atomic<bool>`. A job that ends in a hold class sets it. A job checks
  it once, before its first request: set, it records `skipped` and sends nothing. A job past that check
  runs to its outcome. The round thread checks it before each page and each submit.
- **Schedule refusal.** An `enqueueAndGiveOwnership` throw sets the stop flag and holds that job's page:
  its unsent keys are neither deleted nor leaked. `io_pool_refuse_at_for_test` drives it, as for the
  redelete.
- `io_pool` is free while the janitor runs: its other users (read-ahead, orphan-sweep planning,
  `pending_deletes`) are earlier phases of the same round thread and have returned.

**Per-job outcome.** The [batch failure](#batch-failure) classes apply to each job:

| Job ended | Counts | Its page |
|---|---|---|
| success (absent keys included) | `janitor_deleted` += keys | stays complete |
| refused or exhausted | `leaked` += keys, `batches_leaked` += 1, one WARNING | stays complete |
| authority lost (`job_op.admitted()` false after the throw, or admission refused) | `batches_held` += 1, keys not counted | held |
| deterministic local failure, `NOT_IMPLEMENTED`, schedule refusal | `batches_held` += 1 | held |
| skipped (stop flag set) | `delete_jobs_skipped` += 1 | held |

A held page is incomplete. Jobs of other pages keep their outcomes: a later page's deletes were licensed
by that page's own classification, so they stand and count even though the cursor stops before the held
page. Ordinary failures on a held page are not counted as `leaked`: the next pass re-lists the page.

**Failure asymmetry.**

| Rule | Fails how |
|---|---|
| deletes on the round thread, serial | no-batch store: ~1,000 keys per ~20 s page, one page per folding round, so a 20k drain takes 20 folding rounds, most 9 rounds apart. S3: each page also waits for its batch |
| up to 16 jobs in flight | 16× the delete rate against one bucket: more throttling, which leaks keys for a later pass; up to 16 requests in flight when authority is lost, each naming only terminal-life keys ([authority](#authority)); up to 16 rejected probes once per process ([capability](#capability)) |

Throttling costs a pass; serial costs the drain. Parallel is chosen. 16 is the concurrency
`pending_deletes` already runs against the same bucket.

### Stop rules {#stop-rules}

The round thread starts the next page iff elapsed < `kJanitorPhaseBudget`, the stop flag is clear, and
the last page was decided, had a dead-life candidate, and its read succeeded. "Complete" is not known
yet when the next page starts: its jobs may still run.

| Stop starting pages when | Reason |
|---|---|
| elapsed ≥ budget after a page | soft limit; `budget_exhausted` = 1 |
| authority lost at a refresh or at an admission check | no further request is licensed; nothing is published |
| the stop flag is set: a job held (authority, local failure, `NOT_IMPLEMENTED`, schedule refusal) | the cursor must not pass a page the code did not understand; stop spending requests |
| page not decided (suppressed, ambiguous cut, admission lost) | the cursor did not move; the next page would re-list the same keys |
| page had no dead-life candidate | steady state stays at one page per folding round, today's LIST cost. otel.demo shows `janitor_deleted` 0 over 24 h |
| `next_cursor` empty and the pass began at the stream start | one pass per round; the next round starts from the beginning |
| `next_cursor` empty and the pass began mid-stream | wrap once: continue from the stream start up to the page that began the pass, under the same budget and stop rules; the next round starts from the beginning. Without the wrap every `DROP` costs an extra round: the fold round's pass advances one live page past the debris, and the keys before that page wait for the round after the deleting one |
| a read of page ≥ 2 threw | [cursor](#cursor): publish the complete prefix |

A leaked job does not stop the phase. "Dead-life candidate" is a classification outcome, not a delete
outcome, so a page of already-gone keys still counts as debris.

### Why no per-request deadline {#no-request-deadline}

A per-request deadline was the r4 design (`Retry::within(budget)` frozen for the phase). It fails:

- Every request reserves its whole attempt envelope before admission (`attemptReservationMs`,
  `Backend/CasRequests.h:462`): 7 s with default timeouts, and a conditional write reserves two
  (`CasRequests.cpp:906`). A 20 s window admits requests only for its first ~13 s, conditional writes
  for its first ~6 s, and a store with a larger envelope admits less.
- A deadline that cuts a page mid-way needs a first-page exemption to avoid starving, and the exemption
  gives back the worst case it was meant to remove.

Here every request keeps `Retry::standard`, no policy is frozen for the phase, and no page is exempt: the
budget only stops new pages.

**Failure asymmetry.**

| Rule | Fails how |
|---|---|
| deadline binds every request | a healthy fast store loses the last third of the window, more with a larger envelope; budget refusals surface as read exceptions mid-page |
| soft budget between pages | a phase overruns the budget by the page in progress and the jobs in flight |

Under-binding costs a tail that base already pays on every folding round. Over-binding loses the
throughput the change exists for. The soft budget is chosen.

### Budget and in-flight work {#phase-bound}

The budget is best effort. Expected phase time is **budget + the rest of the page in progress + the jobs
in flight + the cursor publication**. Nothing cuts a request short.

- **Healthy store, 20 ms RTT:** the overrun is under one page: ~140 ms on S3, ~1.3 s on GCS with 16 jobs
  in flight, ~20 s on a no-batch store at `gc_io_concurrency` 1.
- **Failing store:** every request runs `Retry::standard`, up to 90 s (`Backend/CasRetry.h:40`) plus its
  envelope; call that C (~97 s with default timeouts). The round thread's own requests for the page in
  progress are serial: up to ~5 C. Its jobs drain `gc_io_concurrency` at a time: one C on S3 (one batch
  per page, up to 16 jobs overlapping), ⌈1,000 / 16⌉ × C ≈ 100 min on a no-batch store. The publication
  adds one C. Base sends the same 1,000 no-batch requests serially: ~27 h.
- A request that exhausts its window leaks and the elapsed time then exceeds the budget, so a failing
  store usually ends the phase after the page in progress.

Stage C's round deadline is the mechanism that later bounds the tail: it passes
`Retry::within(remaining())` to the requests and replaces the budget with `min(phase_start + budget,
round deadline)`. If a round reaches the janitor with no time left, stage C decides whether it takes a
page; this design does not.

**Why 20 s.** The owner set 15–30 s: 10 s drains too little on a no-batch store. 20 s is 1/3 of
`gc_interval_sec` (60 s) and 1/15 of the planned round deadline (300 s). The owner may set 30 s; every
per-phase figure here then scales by 1.5. A constant with a one-line reason in `CasGc.cpp`; a setting now
would be removed with stage C.

### Authority {#authority}

- `refreshAuthority` runs before each page on the round thread, as before the single page today. Every
  request, on the round thread or in a job, samples authority through its operation's liveness
  predicate.
- **No transient `false` during a refresh.** Base `refreshAuthority` stores `false` before its GET and
  the verdict after it (`CasGc.cpp:4853`). With jobs in flight, a page-1 job admitted or retried during
  page 2's refresh would read that `false`, hold its page and stop the phase under unchanged leadership.
  Rule: the refresh computes its verdict in a local and stores it once, after the read. The verdict is
  `true` only when the lease owner and seq match; absence, mismatch and a failed read store `false`, so
  every exit stays fail-closed. During the GET the flag keeps the value the previous refresh confirmed.
  That widens the no-fencing window below by one GET and adds nothing else.
- **An observed loss is final for the phase.** The predicate must be monotone in the phase. Production's
  is: `authority_held` changes only at a refresh, and the loop breaks on the first `false` a refresh
  stores and never refreshes after it. No job sees authority return inside the phase.
- **A refused admission is classified held.** A request refused at the gate throws
  (`throwReadRefused`: `CasTransientUnavailable` for the fence or the predicate, `CasWriteRetryLater` for
  `NoBudget`). The job then calls `job_op.admitted()`, which samples the gate again rather than
  remembering the refusal (`CasRequests.h:258`). The re-sample is sound because both halves are
  monotone in the phase: the predicate because no refresh follows an observed loss, the mount fence because `LostOrRearmed` is final
  for the operation's generation (`CasMountRuntime::admit`). A predicate or fence refusal therefore
  reads `false` again and holds; it is never counted as `leaked`. A `NoBudget` refusal re-samples
  `false` unless the mount lease was renewed in between; then it classifies as exhausted and leaks.
  Either outcome sends nothing and deletes nothing: held re-lists the page, leaked retries the keys on
  the next pass.
- Observed loss: no further page, no further submit, and the cursor publication is not admitted. Jobs
  not yet past admission fail it and hold their pages; jobs already past it complete, and their deletes
  stand and count. Earlier completed pages keep their deletes.
- `op.admitted()` is checked before every exact delete and before the publication, as today. A job whose
  delete succeeded while liveness turned false stays counted: `readLoop` returns the store's success
  without re-checking liveness (`CasRequests.h:475`), and the keys are gone.
- **No instantaneous fencing.** Another leader can replace the durable lease after the last refresh.
  `authority_held` does not change until this leader observes the replacement, so up to
  `gc_io_concurrency` jobs and the cursor publication can still be sent. Base has the same schedule with
  one request.
- **Safety rests on life identity, not fencing.** Every key in a job belongs to a life absent from an
  unambiguous catalog cut taken after its page's LIST. Such a life is terminal: a table re-created with
  the same UUID writes under a fresh incarnation prefix (`mintFreshIncarnation`), so its keys are never in
  a job, and `buildRefWalkPlan` discards parent lives absent from the catalog. A second leader deleting
  the same keys repeats an idempotent delete. Running jobs concurrently does not change what any job
  names.
- The cursor publication is a conditional write on the maintenance etag read at phase start. If a
  competing leader published first and this leader has not yet observed the loss, the gate admits the
  write, the store refuses the precondition, and the resolve read finds the other leader's state:
  `Conflict` (`CasRequests.cpp:1059`). The competing cursor stands. The janitor records an anomaly only
  for `Refused` and `GaveUp` (`CasNamespaceJanitor.cpp:153`), so a `Conflict` is silent, as today. This
  change adds no conflict logging.
- There is no refresh after the publication in the same phase. This leader observes the loss at the next
  invocation's refresh, which stops it before its first page.

### Cursor {#cursor}

A page is **complete** when it was decided, its exact deletes ran (a failed exact delete is `leaked` and
does not block, as today), and each of its jobs succeeded or [leaked](#batch-failure). Completeness is
decided per page at its settle. The published cursor is the `next_cursor` of the last page of the longest
complete prefix.

- **Publication:** one conditional maintenance write after the last settle, `Retry::standard`. A
  `Refused`, `GaveUp` or thrown publication is an anomaly, not an exception, as today; a `Conflict` means
  another leader's cursor stands and is not recorded. Published only if the operation is still admitted.
- **Held page k, complete pages after it:** the cursor stops before page k. The next pass re-lists pages
  k onward; keys the later pages already deleted are gone and are not listed again.
- **Rejected persisted cursor:** kept as today (`CasNamespaceJanitor.cpp:40`). If page 1's LIST from the
  persisted cursor throws, the phase makes the one-shot maintenance reset under `Retry::once` and
  rethrows. No job has been submitted yet. `BackendRejectedCursorResetsExactlyAndDeletesNothing` stays.
- **Read failure on page ≥ 2** (LIST or catalog GET from this phase's own cursor): no further page, the
  last settle, an anomaly, and the complete prefix published. No reset: the failing cursor is not the
  persisted one.
- **An exception escaping `run`** waits for all jobs (`SCOPE_EXIT_SAFE`) and publishes nothing; `out`
  keeps the counts aggregated so far. A crash publishes nothing. The next round re-lists; deleting an
  absent write-once key is a no-op.

## Batch size {#batch-size}

### Where the limit lives today {#limit-today}

- `S3ObjectStorage::removeObjectsIfExist` splits by `s3_objects_chunk_size_to_delete` (default 1000). The
  CAS path does not use it: `ObjectStorageBackend::removeManyWriteOnce` calls
  `removeObjectsIfExistUnderProfile`, and `removeObjectsIfExistImpl` sends all its keys as one
  `DeleteObjects`. A store configured at 500 still receives 1,000-key requests from GC today.
- `s3_objects_chunk_size_to_delete` is read with `config.getUInt` and no positive check
  (`S3Settings.cpp:54`); 0 is accepted.
- With the capability false, `removeObjectsIfExistImpl` throws `NOT_IMPLEMENTED` for more than one key
  without sending (`S3ObjectStorage.cpp:696`). With it unknown, it sends one `DeleteObjects`; a
  `NotImplemented`/`InvalidRequest`/`InvalidArgument` rejection sets it false and throws `NOT_IMPLEMENTED`
  (`S3ObjectStorage.cpp:742-749`). It never loops per key.
- `removeChunkWriteOnceOrOneByOne` (`CasGc.cpp:393`) catches that `NOT_IMPLEMENTED` and sends one
  admitted request per key.

### The rule {#chunk-rule}

**CAS passes a chunk no larger than the storage batch.** One admitted request is one storage request.

- New `IObjectStorage::batchDeleteKeyLimit() const`, **always ≥ 1**: the most keys one
  `removeObjectsIfExistUnderProfile` call sends as one request.
  - `S3ObjectStorage`: max(1, `s3_objects_chunk_size_to_delete`) while `isBatchDeleteSupported()` is not
    false; 1 once it is false.
  - default: 1.
- `Backend::bulkDeleteKeyLimit()` forwards it; the in-memory and emulated backends take a configurable
  value. `Backend` gives it a default of 1, so **`InstrumentedBackend` must forward it explicitly**:
  `Pool::open` wraps every production backend in it (`CasPool.cpp:391`, `:906`), and an inherited default
  would turn production S3 batches back into one-key requests. The same holds for any capability accessor
  the GC reads. The forwarding sits next to `attemptEnvelopeMs` and `refreshCredentials` in
  `CasInstrumentedBackend.h`.
- `Gc::bulkDeleteChunkKeys()` = `std::clamp(min(gc_bulk_delete_chunk_keys, bulkDeleteKeyLimit()), 1,
  kBulkDeleteMaxKeys)`. The CAS clamp stays although the getter never returns 0: an empty chunk loops
  without advancing (`cleanupRefObjects` reaches `chunk.front()`).
- All three write-once families cut their requests with it: the janitor, `cleanupRefObjects`
  (`CasGc.cpp:3836`) and the owner-removed manifest cleanup (`CasGc.cpp:1280`). The latter two change
  only on a store with `s3_objects_chunk_size_to_delete` below 1000 or without batch delete.
- It sizes **requests, not cohorts**. See [cohort and request](#cohort-and-request).

**Rejected: hand the whole page to the storage and let it split.** One admission would cover several
storage requests, the gap `ObjectStorageBackend::removeManyWriteOnce` exists to close, and CAS could not
tell which request failed.

`batchDeleteKeyLimit` is a fork patch on shared object-storage code: one virtual with a conservative
default of 1, plus the S3 override. Non-CAS deletion does not call it.

### Cohort and request {#cohort-and-request}

A **cohort** is the set of keys one authority validation licenses. A **request** is one admitted
`removeManyWriteOnce`. The storage limit sizes requests; it never shrinks a cohort.

| Family | Cohort | Authority read per cohort | Requests per cohort |
|---|---|---|---|
| janitor | one LIST page | `refreshAuthority` | the page's dead canonical keys, `bulkDeleteChunkKeys()` each |
| `cleanupRefObjects` | clamp(`gc_bulk_delete_chunk_keys`, 1, `kBulkDeleteMaxKeys`) keys, cut by the ref-cleanup budget, as today | `authorityHolds`: a catalog GET and a `gc/state` GET, as today | `bulkDeleteChunkKeys()` each |
| manifest cleanup | clamp(`gc_bulk_delete_chunk_keys`, 1, `kBulkDeleteMaxKeys`) bodies, as today | none, as today | `bulkDeleteChunkKeys()` each; each request's bodies are recorded when it returns |

- Admission differs by family. `CasOperation::gate` checks the mount fence, then the operation's
  `Liveness` predicate if one was given. The janitor supplies its `authority_held` predicate, so an
  observed GC lease loss stops its next request. `cleanupRefObjects` and the manifest cleanup use plain
  `admit()` (`CasGc.cpp:3715`, `:642`) with no predicate: each of their requests checks only the mount
  fence. They check GC leadership per cohort (`authorityHolds` in `cleanupRefObjects`, nothing in the
  manifest cleanup), as base does; this change adds no per-request authority read.
- `NOT_IMPLEMENTED` from any request stops the family for the round ([other families](#other-families)).
  No failed request is re-sent in smaller pieces.
- On a store without batch delete, `cleanupRefObjects` sends exactly base's GETs: base validates once per
  cohort, then its per-key fallback deletes the cohort key by key.

**Failure asymmetry.**

| Rule | Fails how |
|---|---|
| authority read per request | on a no-batch store, 2 GETs per key: ~10,000 extra GETs per 5,000-key cleanup, ~200 s at 20 ms, outside the janitor budget |
| authority read per cohort | a catalog or lease change inside a cohort is seen at the next cohort; that is base's window, and each request still passes the mount-fence admission |

Per cohort costs nothing base does not already accept. Per request is a GCS round-time regression.
Per cohort is chosen.

### Capability {#capability}

Read `isBatchDeleteSupported()` once, before the phase's first job, and fix the phase's batch size with
it. A job is cut at that size for the whole phase, whatever the storage learns meanwhile:

| Capability | Batch size | Jobs |
|---|---|---|
| false (config or learned) | 1 | one-key jobs, no probe |
| unknown | storage limit | multi-key jobs, the same as true; unknown is the default for AWS too |
| true | storage limit | multi-key jobs; a storage limit of 1 gives one-key jobs |

**Remembered per disk.** `S3Capabilities` holds the answer under a mutex for the object-storage
instance (`setIsBatchDeleteSupported`, `S3Capabilities.h:34`). It is not persisted: after a restart the
first phase on the disk reads unknown again. No CAS-side memory is added.

**No sequential probe.** On an unknown store the first page's job goes to the pool like any other. Up to
`gc_io_concurrency` multi-key jobs may reach the store before the first rejection flips the flag; each is
rejected. After the flip, `removeObjectsIfExistImpl` throws `NOT_IMPLEMENTED` for more than one key
without sending (`S3ObjectStorage.cpp:696`). Every such job holds its page and sets the stop flag. So the
cost is at most `gc_io_concurrency` rejected `DeleteObjects` and one held folding round, the first time a
process touches the disk.

**A rejection stops the phase.** `capability_learned` = 1, no further page or submit, the cursor stays
before the first held page. The next folding round reads false and cuts one-key jobs. **The stop is
family-local.** A family that runs later in the same round reads false at its own start and sends
one-key requests that round; that is intended. Its one-key requests are ordinary requests of a known
capability, each licensed by that round's own classification.

**No same-phase per-key recovery can occur.** The r4 schedule was a worker learning false while the
round thread cut the same page's keys again at 1. Here the batch size is fixed at phase start and the
keys of a rejected job are never cut again in the phase. A one-key job in a multi-key phase (a page with
one dead key, or a remainder of one) was cut at phase start from classification, not from a failure. If
another user of the storage flips the capability mid-phase, the janitor's next multi-key job throws
`NOT_IMPLEMENTED` without a request and stops the phase the same way.

**Why no recovery at all.** Deleting keys one by one in response to a failed batch is a destructive
fallback. The rejection is also not reliably "no batch verb": `InvalidRequest` and `InvalidArgument` map
to the same `NOT_IMPLEMENTED`.

**Cost on GCS:** one request per key, 16 in flight in the janitor; serial in `cleanupRefObjects` and the
manifest cleanup, as base.

GCS's JSON-API multipart batch is unreachable through the S3 client; out of scope.

### The other two families {#other-families}

`removeChunkWriteOnceOrOneByOne` becomes `removeCohortWriteOnce(op, cohort, request_keys, policy,
on_request_done)`: consecutive `op.removeManyWriteOnce` requests of at most `request_keys` keys, a callback
after each one returns, and any exception propagated. Today neither caller contains a propagated `NOT_IMPLEMENTED`: it escapes the round,
`isTransientGcRoundError` rejects it, and the scheduler clears `i_am_leader` (`CasGcScheduler.cpp:460`).
So each caller catches it.

| Caller | On `NOT_IMPLEMENTED` from a chunk | Reports | Recovery of the chunk's keys |
|---|---|---|---|
| `cleanupRefObjects` (`CasGc.cpp:3853`) | catch, WARNING, stop the family for this round; other exceptions propagate as today | `capability_learned` = 1 on its phase | the plan recomputes the same candidates from the durable checkpoint next round (`CasGc.cpp:3838`), cut at 1 |
| owner-removed manifest cleanup (`CasGc.cpp:1293`) | catch, WARNING, stop the family for this round; requests before it stay recorded (`CasGc.cpp:1276-1279`); other exceptions propagate as today | `capability_learned` = 1 and the count of unsent bodies on `manifest_deletes` | **not the next fold**: the intake cursor that found them is already committed. The orphan-manifest sweep reclaims them (`CasGc.cpp:1252-1259`) |

The round, the tenure and `i_am_leader` are unaffected. It happens at most once per storage per process:
after it the capability is false and no chunk above one key is cut. The manifest family's cost is one
round's unsent bodies handed to the bounded orphan sweep, once.

## Batch failure {#batch-failure}

`CasOperation::removeManyWriteOnce` reissues the whole batch inside its policy; an absent key counts as
success. `removeObjectsIfExistImpl` sends `DeleteObjects` in quiet mode, skips per-key `NoSuchKey`, and
throws `S3Exception` for the first remaining per-key error after the other keys are gone. A batch that
finally throws may have deleted any subset of its keys; the janitor does not parse which.

**Rule: an ordinary failure leaks and advances; three cases hold.** The same rule the exact-token path
has today (`CasNamespaceJanitor.cpp:134`). `op.admitted()` is checked first: a batch that fails after
authority is lost holds, whatever it threw. Each job is classified by this table on its own operation
([per-job outcome](#jobs)); a one-key job is a batch of one.

| Class | Exceptions | Retry inside the policy | Action after the throw |
|---|---|---|---|
| refused | `isDefinitelyRefusedWrite(e)`: malformed request, entity too large, access denied, a credential error the one refresh did not fix | none: the read loop ends at once (`CasRequests.cpp:480`) | count the keys as `leaked`, `batches_leaked` += 1, one anomaly, page stays complete, continue |
| exhausted | transport, throttling (`SlowDown`, 503), 5xx, timeouts, a non-S3 `Poco::Exception` | until `Retry::standard` gives up | as refused |
| authority lost | the liveness predicate failed; `op.admitted()` is false after the throw | none | hold: `batches_held` += 1, keys not counted, stop flag, no publication |
| capability | `NOT_IMPLEMENTED` | none | [stop the phase](#capability), hold, stop flag |
| local bug | `isDeterministicLocalFailure`: `LOGICAL_ERROR`, `BAD_ARGUMENTS`, `CORRUPTED_DATA`; a non-`Poco` exception | none (`CasRequests.cpp:458`) | hold: `batches_held` += 1, stop flag; never advance past a page the code did not understand |

**Failure asymmetry.**

| Rule for an exhausted transient failure | Fails how |
|---|---|
| hold the cursor | a key that returns `SlowDown` forever starves every page behind it, indefinitely. With one-key batches, each run starts at that key and deletes nothing |
| leak and advance | a key that could have been deleted a round later waits one full pass, counted in `leaked` |

Holding can wedge debris reclamation for good; advancing costs one pass and is never a wrong delete.
Advance is chosen.

- **Repeat failures are visible.** A key that fails every time is leaked on every pass that reaches it:
  on a one-page namespace, every folding round. It shows in `batches_leaked` and `leaked`, and the
  anomaly names the key range and the exception.
- **Termination.** Every job ends in success, a leak, a hold or a skip. Success and leak keep the page
  complete; a hold is one of the stop cases, none of which a storage throttle can produce.
- **Partial deletes.** A leaked batch may have deleted any subset of its keys. `leaked` counts the whole
  batch: an upper bound. The next pass lists only the survivors.
- **Mixed per-key errors** classify by the first remaining one, the type the storage throws. The class
  decides only how long the batch retries; both leak.
- The next pass re-lists and re-classifies against a fresh catalog cut; nothing from a failed attempt is
  reused as a license.

### S3 adapter: keep the error name {#adapter}

`isDefinitelyRefusedWrite` matches `EntityTooLarge` and several malformed-request errors by name only
(`S3Common.cpp:62-80`). All three delete throw sites a CAS batch reaches build `S3Exception` without
the name:

- whole-response error, `S3ObjectStorage.cpp:751`;
- first remaining per-key error, `S3ObjectStorage.cpp:769`;
- **one-key batch:** `removeObjectsIfExistImpl` sends a single key as a plain `DeleteObject` through
  `deleteFileFromS3` (`S3ObjectStorage.cpp:681`), which throws at `deleteFileFromS3.cpp:77`. Every batch
  on a store without batch delete, and any chunk of one key, goes this way.

**Change:** use the existing `S3Exception(PreformattedMessage &&, S3Errors, String exception_name)`
constructor, built with `PreformattedMessage::create` from the current format strings. Pass
`err.GetExceptionName()` at `:751` and at `deleteFileFromS3.cpp:77`, and the first remaining per-key
`err.GetCode()` at `:769`. `deleteFileFromS3` is shared with non-CAS deletes; the change adds the name
and keeps the error code and message.

**Why it matters under leak-and-advance.** Without the name, a name-only refusal counts as exhausted: the
batch retries for the whole `Retry::standard` window (up to 90 s), overruns the budget, and ends the
phase after that page on every pass. With it, the CAS retry loop ends at once and the batch leaks. The
"one attempt" in the tests below means one CAS attempt with no refresh callback configured.
`AccessDenied` is unaffected: it also matches by enum.

**Refresh reissue.** `removeObjectsIfExistUnderProfile` runs inside
`S3ObjectStorage::refreshAndRetryOnExpiredCredentials`, which repeats the call once when
`isAccessTokenExpiredError` holds and a `credentials_refresh_callback` returns a client
(`S3ObjectStorage.cpp:280`); `S3Common.cpp:41` treats an `UNKNOWN` code as expired. CAS has its own
refresh-and-reissue for the same class (`refreshAndClassifyReadFault`). So one admission can produce one
extra storage request, without a new gate check, when a refresh callback is configured. The guarantee
is: **one admitted request = one storage request, except a credential-refresh reissue**. Whether the
preserved name removes the refresh on a name-only refusal is decided by the test below, not assumed.

**Tests** (`gtest_cas_s3_bulk_delete_fallback.cpp`, emulated S3): a whole-response `EntityTooLarge` and
`MalformedXML`, a per-key `EntityTooLarge`, and a one-key `EntityTooLarge` each throw an `S3Exception`
whose `getExceptionName()` is the canonical code and for which `isDefinitelyRefusedWrite` is true. Red on
base. With a refresh callback configured, the same refusals cost at most one extra storage request and
then leak; the test records the callback count and the request count. The janitor side is `OneKeyNameOnlyRefusalLeaksAfterOneAttempt` in [tests](#tests).

### Versioned buckets {#versioned}

**Precondition: the bucket is unversioned.** The mount probe rejects a store that answers a delete with a
delete marker (`CasProbe.cpp:24`). If versioning is enabled after mount, the quiet batch reports success
while noncurrent versions remain, as `cleanupRefObjects` and the manifest cleanup already do. An empty LIST
then proves the namespace drained, not that storage was reclaimed. Detection is out of scope.

## Deferred rounds {#deferred-rounds}

**No janitor deletes on a deferred round.** The deferred path keeps its single suppressed page: one
maintenance GET, one logical LIST page, one catalog GET, no delete, no cursor move. This is the existing conservative
policy, kept because nothing needs it changed.

A DROP changes shards, so the next rounds fold. The janitor starts on round 2, because in round 1 the
life still resolves. At ~140 ms per page round 2 drains a 20k stream (~3 s). At report 08's 0.5 s per
page it also drains it (~10 s). On a no-batch store at 20 ms round 2 takes ~15 pages and round 3, which
also folds, takes the rest. No 9-round wait in any case.

Why a deferred-round delete was not shown unsafe, and why folding does not protect against a vanished
catalog row: see the lifecycle facts in `mintFreshIncarnation`, `CasRefCatalog::createNamespace`,
`identityPreserving` (`CasRefCatalog.cpp:208`) and `buildRefWalkPlan` (`CasGc.cpp:215`).

| Wrong choice | Cost |
|---|---|
| keep suppression, and deferred deletes were safe | debris beyond one 20 s window waits up to 9 rounds per further window |
| allow deferred deletes, and an unexamined term of the fold gate mattered | a destructive family runs on a round the gate would have stopped |

**Reopen if** an A/B shows a realistic drain needing more than two folding rounds, the defer cadence
dominates its wall time, and stage C3 (`requestRoundSoon`) has not landed.

## Settings {#settings}

- None new. `kJanitorPhaseBudget` (20 s) is a constant until stage C absorbs it.
- `gc_io_concurrency`: its description (`ContentAddressedSettings.cpp:82`) gains "namespace janitor
  deletes of dead ref logs and snapshots"; "other GC requests run on the round thread" stays true of
  everything else. 1 keeps the janitor sequential.
- Test seam: `PoolConfig::gc_janitor_phase_budget_ms_for_test` (`std::optional<uint64_t>`). Not a setting.
- `gc_bulk_delete_chunk_keys`: its description gains "dead-life ref logs and snapshots of the namespace
  janitor" and "capped by the storage's batch-delete limit".
- `s3_objects_chunk_size_to_delete` now also caps GC batches on a CAS disk; 0 behaves as 1 there.
- CAS-29.9's acceptance criteria #2 and #3 name page settings and a `pages=10` run; this design replaces
  both. The backlog task needs that edit when the spec is accepted.

## Code shape {#code-shape}

- `NamespaceJanitor::runOnePage` → `NamespaceJanitor::run(bool suppress_deletes, JanitorRunContext,
  NamespaceJanitorResult & out)`. The context carries the phase budget, the clock, the batch-size
  function, a `refresh_authority` callback, `io_pool`, `gc_io_concurrency`, the schedule-refusal test
  seam. `run` owns the page loop, the per-page settle, the maintenance read and the single
  cursor publication. It writes into `out` as pages and jobs settle, so a throw leaves completed work
  counted.
- From `cas/gc-request-cuts` @`1edf28f2a9a`: the classification and `Layout::writeOnceStreamKey`
  (`CasLayout.h`), which batches a listed key only if it equals its canonical rendering; others go to the
  exact path. A write-once batch has no token, so it must name only keys the layout renders. Not taken:
  `flush_chunk`'s per-key retry through `remove_exactly`, and `batch_supported`.
- `removeChunkWriteOnceOrOneByOne` → `removeCohortWriteOnce`; both callers keep their cohort loop and
  per-cohort work (`authorityHolds`, the budget cut, recording) and catch `NOT_IMPLEMENTED`
  ([cohort and request](#cohort-and-request), [other families](#other-families)).
- `Gc::runNamespaceJanitorPage` → `Gc::runNamespaceJanitor`. Its existing `catch` emits the partial `out`.
  The deferred call site keeps `suppress_destructive = true`; the stop rule ends it after one page.
- The job fan-out follows `Gc::redeleteBlobs`'s structure: submit with handles, `SCOPE_EXIT_SAFE` wait,
  per-slot results, aggregation in submit order on the round thread. It adds the
  stop flag and per-page completeness, and shares no code with the redelete.
- `Gc::authority_held` becomes `std::atomic<bool>`: `refreshAuthority` writes it while jobs read it.
  `refreshAuthority` drops its leading `authority_held = false` and stores its local verdict once, on
  every exit ([authority](#authority)).
- Context test seam `on_job_start_for_test(page, job)`, called on the worker before the stop-flag check
  and the admission, so a test can place a job's first admission inside another step.
- `IObjectStorage::batchDeleteKeyLimit`, `S3ObjectStorage` override, `Backend::bulkDeleteKeyLimit`,
  `InstrumentedBackend` forwarding (and of any capability accessor), `Gc::bulkDeleteChunkKeys`; the
  [adapter](#adapter) change.

## Observability {#observability}

The janitor issues its requests from the round thread, but the LIST requests run on the async
iterator's threads. `GcPhaseTimer` snapshots the round thread's `ProfileEvents`, so its delta misses every
`ListObjectsV2`: report 06 found `DiskS3ListObjects` 0 on phase rows.

**The row carries logical counts, not wire requests.** The janitor's write-once deletes run on `io_pool`
threads, which the phase delta also misses. `pending_deletes` has the same gap today: its row carries
only `jobs_scheduled` and `jobs_failed` (`CasGcPhaseTimer.h`, "what the delta does not cover"; the same
note at `CasGc.cpp:2216`). The janitor follows it: no per-job `ProfileEvents` delta, no wire count on
the row. Wire requests are counted once, outside the row, by the process-wide `system.events` delta over
a GC-only window on an idle stand ([value proof](#value-proof)).

Metrics are `std::map<String, UInt64>` (`CasGc.h:219`), so every value is numeric.

`namespace_cleanup` row in `system.cas_gc_log`, summed over the round's pages:

| Metric | Status | Meaning |
|---|---|---|
| `janitor_pages`, `janitor_keys`, `janitor_deleted`, `leaked`, `evidence_rows` | existing | now sums; `janitor_pages` counts logical pages; `janitor_deleted` counts keys of succeeded jobs, including already-absent keys; `leaked` adds all keys of leaked jobs |
| `batches` | new | jobs that reached the store call (a batch or one key), including a job the request gate refused |
| `batches_leaked` | new | jobs refused or exhausted: keys leaked, page stays complete |
| `batches_held` | new | jobs that held their page: authority lost, a deterministic local failure, `NOT_IMPLEMENTED` or a schedule refusal |
| `delete_jobs` | new | jobs submitted (inline ones included when no pool is given) |
| `delete_jobs_skipped` | new | jobs that found the stop flag set and sent nothing |
| `batch_keys` | new | the phase's batch size; shows the storage limit and capability in effect |
| `capability_learned` | new | 1 when a job was rejected as unsupported |
| `budget_exhausted` | new | 1 when the soft budget stopped new pages |
| `cursor_advanced` | new | 1 when a new cursor was published |

ProfileEvents: new `CASGCNamespaceCleanupObjectsDeleted` (twin of `CASRefCleanupObjectsDeleted`, same
"includes absent keys" caveat), new `CASGCNamespaceCleanupBatchFailures` (leaked and held jobs), and
`CASGCNamespaceCleanupLeaks` also counts keys of leaked jobs. All three are incremented on the round
thread during aggregation. One WARNING per failed job, as the redelete warns per entry
(`reportRedeleteFailure`). No new `CasEvent`.

Docs: phase 16 of `docs/en/antalya/cas/architecture/garbage-collection.md`, the `namespace_cleanup` row
of `docs/en/operations/system-tables/cas_gc_log.md`, the batch-size note in
`docs/en/antalya/cas/configuration.md`, and the unversioned-bucket consequence for batched keys.

## Value proof {#value-proof}

Owner rule: no merge without an A/B that moves the target metric. **Target: drain wall time from the end
of `DROP TABLE ... SYNC` until no `_log`/`_snap` key of the life remains.** Secondary: wire DELETE
requests, janitor phase time, per-page time, round duration.

Base = `e2dd0f5d343`. After = this change on the same base. Both built from the same tree, run
alternately (B, A, B, A, ...) on the same local stand (`ch1` + RustFS + Keeper). Report 08 supplies
predictions only.

**Two scopes, never mixed in one criterion.**

- **Janitor phase:** read from the `namespace_cleanup` rows of `system.cas_gc_log`. Logical counts
  only (`delete_jobs`, `batches`, `batches_leaked`, `batches_held`, `janitor_pages`, `cursor_advanced`,
  `budget_exhausted`, `capability_learned`) and phase time.
- **Whole drain:** from the end of the DROP until the drain completes. Wire requests are process-wide
  `system.events` deltas over that window, on a server that runs nothing but the forced GC rounds: the
  method of `utils/ca-soak/gc-ab` and of report 08. They include every GC family, the fold's own ref
  cleanup in the first folding round among them, on both sides.

### M1: long-stream drain {#m1-long-stream-drain}

Driver `master/tmp/gc-cuts-stand/value/m1.py`, `ch1` + RustFS + Keeper, rounds forced every 10 s, ≥ 3 runs
per side per cell. **M1 carries the value proof.** ch1 runs nothing but the forced rounds, so
the process-wide `DiskS3ListObjects` delta is the janitor's LIST count. Every after-run reports the phase
time and round-thread CPU; that settles the [open question](#context).

| Cell | Metric | Predicted (after) | Pass |
|---|---|---|---|
| 7k, no latency | janitor phase | ≤ 1 s | |
| | janitor-active rounds | 1 | 1 |
| | janitor delete calls (`batches`), janitor phase | 7 | ≤ 10 |
| | wire DELETE requests, whole drain | base − after ≈ base's `janitor_deleted` − after's `batches`: the fold's own deletes are equal on both sides (report 08: 2,551 → 221) | after ≤ base, and the saving within 1% of that difference |
| | physical LIST requests | janitor: ~3 per page, 2 and 1 on the last two: ~18 | process-wide `DiskS3ListObjects` over the drain window (`metric_log`, report 01's method) ≤ base; base has no `list_requests_process` |
| 20k, no latency | janitor phase | ~3 s at 140 ms per page; ~10 s at 0.5 s | |
| | janitor-active rounds | 1 | 1 |
| | drain wall | round 1 (~23 s) + one interval + round 2 ≈ 45 s | ≤ 90 s, and below every base run |
| | janitor delete calls (`batches`), janitor phase | 16 (~15k keys; the fold deleted ~5k in round 1) | ≤ 25 |
| | wire DELETE requests, whole drain | as the 7k cell (report 08: 16,163 → 266) | as the 7k cell |
| 7k, 20 ms RTT | janitor phase | ~1 s at 140 ms per page | ≤ 5.3 s (report 08's batches-only figure) |
| 100k, no latency | janitor-active rounds | 1 at 140 ms per page (~14 s for 100 pages); 3 at 0.5 s (40 pages per window) | ≤ 3; `budget_exhausted` 1 on every janitor-active round but the last |

If per-page time is report 08's 0.4–0.6 s rather than ~140 ms, the 20k cell still fits one 20 s window
(~12 s), and the 100k cell shows the budget stop. The profile names the hot spot. Fixing it is a separate
task unless trivial.

Base runs the 7k, 20k and 7k latency cells, measured fresh. Base is not run at 100k: ~95 pages × 9 × 10 s
≈ 2.4 h per run. If seeding 100k keys takes more than 1 h, stop at 50k and say so.

**Fail:** janitor-active rounds on the 9-round cadence with `budget_exhausted` 0: the loop did not
continue. Debug before re-measuring.

### M2: S30 create/drop churn {#m2-s30}

`utils/ca-soak/gc-ab/run-stand.sh run <label> <binary> --scenario S30 ...`, read with `t2.py` and
`extract.py`, 3 runs per side. Prediction: `defer_decision.dead_life_debris` and `ref_keys_listed` grow
with drops on base and stay flat on after; leader round duration over the last third is after ≤ base;
`janitor_pages` 1 on folding rounds without debris, both sides. If base does not grow at S30's scale, say
so; M1 carries the proof.

### M3: steady state, no regression {#m3-steady-state}

S11 `--scale ci`, 3 runs per side (`compare.py`). On every folding round: `janitor_pages` = 1 (one
logical page), `batches` = 0, phase time unchanged within spread. Physical LIST requests: process-wide
`DiskS3ListObjects` per round from the `metric_log` window, after ≤ base; S11's other LIST callers
run concurrently, so the count is not a criterion on the janitor alone. Request mix per round
otherwise unchanged: authority GET, maintenance GET, catalog GET, maintenance PUT.

### M4: storage without batch delete {#m4-no-batch}

The local stand with the CAS disk's `support_batch_delete` set to false in its disk config, rounds
forced every 10 s, 20 ms RTT added on the RustFS container by `utils/ca-soak/gc-ab/longround/netem.sh`.
Capability false sends one `DeleteObject` per key, the request shape of a store without batch delete,
so no GCS stand is needed. **This store now has a gain.**

**M4a, 20k drain, base vs after**, 2 runs per side, alternating. Report 08's DROP workload: the first
folding round's ref cleanup deletes ~5,000 dead-life `_log` keys, serially on both sides
(`CasGc.cpp:3846`): ~100 s at 20 ms, unchanged by this design. The janitor gets the other ~15,000 keys,
~16 pages.

| Metric | Scope | Predicted (after) | Base | Pass |
|---|---|---|---|---|
| janitor-active folding rounds | janitor phase | 2: ~15 pages, then ~1 | ~16: one page each, 2 right after the DROP, then one per 9 rounds | after ≤ 2 at ≤ 20 ms median one-key DELETE latency |
| janitor phase time | janitor phase | ~20 s + the page in progress (~1.3 s), then ~1.3 s | ~20 s per page | reported |
| `batch_keys`, `delete_jobs` | janitor phase | 1, ~15k | no such metrics | `delete_jobs` = janitor keys; `batches_held` 0 |
| authority GETs per deleting page | janitor phase: `system.blob_storage_log` `Read` rows for the `gc/state` key inside the phase's time window | 1 | 1 | after = base |
| drain wall | whole drain | round 1 (~100 s of ref cleanup plus the fold) + one interval + round 2 (~21 s) + one interval + round 3 ≈ 2.5 min | ~100 s + ~14 × 9 rounds of cadence ≈ 30 min | after below every base run |
| wire DELETE requests | whole drain, `DiskS3DeleteObjects` from `system.events` | ≈ keys of all families: ~20k plus manifests and state keys | the same | after ≤ 1.01 × base |
| authority GETs | whole drain, `blob_storage_log` `Read` rows for `gc/state` | fewer: fewer rounds | one per round at least | after ≤ base |

Every criterion compares one scope with the same scope. The per-page authority GET is the only
per-key figure, so it alone comes from `system.blob_storage_log`; a run first checks that the log holds
`Read` rows for `gc/state`, and if it holds none the authority rows are reported as unmeasured.

A base run may stop after 5 janitor-active rounds (~30 rounds, ~10 min), once its one-page-per-round
cadence is shown; the report then states the extrapolated drain wall. If the stand's median DELETE
latency is above 20 ms, the pass is page time ≤ 1.2 × (1,000 / 16 × measured latency) plus the serial
requests, and the round count is reported, not judged. One run per side without netem is reported, not
judged: without latency both sides may drain in a few rounds.

**M4b, capability unknown, leaves the A/B.** The local store accepts `DeleteObjects`, so nothing on the
stand rejects the probe. The per-family behaviour is asserted on the emulated S3 store by
`UnknownCapabilityConcurrentProbesAllStop`, `RefCleanupContainsCapabilityRejection` and
`ManifestCleanupContainsCapabilityRejection`:

- The rejecting family: `capability_learned` 1 on its row, no one-key request for its failed cohort or
  page in that round. Rejected `DeleteObjects`: exactly one for the manifest or ref cleanup, which send
  in sequence; between 1 and `gc_io_concurrency` for the janitor, whose jobs overlap.
- Families after it in the same round (manifest cleanup `CasGc.cpp:1252`, namespace janitor
  `CasGc.cpp:1334`, ref cleanup, in that order) may read false and send one-key requests; allowed.
- If manifest cleanup rejected, its unsent bodies are reported and later deleted by the orphan-manifest
  sweep.

No live store in this plan exercises a rejected probe.

#### M4 ref-cleanup comparison, base vs after {#m4-ref-cleanup}

Same local stand, `support_batch_delete` false, 20 ms RTT, 2 runs per side, alternating. A live table
takes single-row inserts until `ref_object_cleanup` deletes ≥ 2,000 keys per round on both sides. The
inserts make this window not GC-only, so every count here is per key from `system.blob_storage_log`,
scoped to the phase window and the ref-key prefix; with one key per request a `Delete` row is one
request attempt.

| Metric, per `ref_object_cleanup` phase | Source | Pass |
|---|---|---|
| authority GETs: reads of the catalog object and of `gc/state` | `system.blob_storage_log` inside the phase window | after = base, = ⌈keys / 1,000⌉ each |
| delete requests | `system.blob_storage_log` `Delete` rows under the ref-key prefix inside the phase window | after = base, ≈ keys |
| phase duration | `system.cas_gc_log` | after ≤ base + spread |

## Tests, failing first {#tests}

`src/Disks/tests/gtest_cas_namespace_janitor.cpp`, suites `CASNamespaceJanitor` (in-memory backend,
injected request clock) and `CASNamespaceJanitorIntegration` (`Gc` + `openPoolForTest`). Tests run with no pool (jobs
inline in submit order), the sequential model with exact request orders and counts, unless a row or
section says otherwise. "Red" fails
against `e2dd0f5d343`; "guard" is green on base and stays green. A red test fails on
an assertion of new behaviour, not only on a missing hook or metric name. Orderings use backend hooks, never
sleeps. Fault schedules first; the happy path once.

**Batching and size**

| Test | Asserts | Base |
|---|---|---|
| `DeadLifeStreamKeysDrainInBatches` | 2,500 dead `_log` over 3 pages, limit 1000: 3 `removeManyWriteOnce` calls, no exact remove on them, all gone | red |
| `ZeroStorageLimitClampsGcChunkToOneKey` | backend storage limit 0: `bulkDeleteChunkKeys` is 1 | red |
| `PoolWrappedBackendForwardsStorageLimit` | a pool opened through `Pool::open` (so the backend is wrapped in `InstrumentedBackend`) over storage with limit 250: `bulkDeleteKeyLimit` is 250 and a 1,000-key dead page is 4 batches; a decorator that dropped the forwarding would give 1,000 batches | red |
| `S3BatchDeleteKeyLimitFollowsChunkSetting` | `objects_chunk_size_to_delete` 500 → 500; 0 → 1; after `setIsBatchDeleteSupported(false)` → 1 | red |
| `StateKeysNeverEnterTheBatchPath` | dead `_ckpt`/`_files` go by exact token | guard |

**Capability and the other families**

| Test | Asserts | Base |
|---|---|---|
| `NoBatchCapabilitySendsOneKeyRequestsWithoutProbe` | capability false, `gc_io_concurrency` 4: every dead canonical stream key goes through a one-key `removeManyWriteOnce` on a thread other than the caller's, zero exact-token `remove` calls for them, `batch_keys` 1, `delete_jobs` = keys, no `NOT_IMPLEMENTED`, `batches_held` 0 | red |
| `UnknownCapabilityRejectionStopsThePhase` | unknown, 3 dead pages, the first batch throws `NOT_IMPLEMENTED`: 1 backend delete call, zero one-key calls, no page 2 LIST, cursor unchanged, `capability_learned` 1; the next run cuts at 1 and drains | red |
| `CapabilityFlippedByAnotherUserStopsThePhase` | capability flips false between page 1's and page 2's batch: page 2's batch throws without a backend call; phase stops; cursor after page 1 | red |
| `CohortHelperPropagatesNotImplemented` | `removeCohortWriteOnce`, cohort 1,000: request size 1,000, first request rejected → 1 call, rethrows, no per-key call; request size 250, second request rejected → 2 calls, callback once, rethrows. Replaces the per-key tests of `gtest_cas_gc_bulk_delete_fallback.cpp`; the teardown-between-keys test goes with the loop | red |
| `RefCleanupReadsAuthorityOncePerCohort` (integration) | capability false (backend limit 1), 2,500 deletable ref keys of a live namespace, `gc_bulk_delete_chunk_keys` 1000, budget 5,000. A counting decorator around the in-memory backend records reads by key: 3 catalog reads and 3 `gc/state` reads in the phase, 2,500 one-key `removeManyWriteOnce` calls, all keys gone | red on the call count (base sends 3 bulk calls); the authority count is the guard and equals base |
| `ManifestCleanupSendsCohortAsStorageRequests` (integration) | capability false, 2,500 owner-removed bodies: 2,500 one-key requests, `requests` 2,500, `accepted` 2,500, zero catalog and `gc/state` reads in the phase | red |
| `RefCleanupContainsCapabilityRejection` (integration) | `cleanupRefObjects` chunk rejected: the round completes, `i_am_leader` stays, no one-key call that round; next round deletes the candidates at size 1 | red |
| `ManifestCleanupContainsCapabilityRejection` (integration) | manifest chunk rejected: the round completes, earlier chunks recorded, unsent count reported; the orphan-manifest sweep later deletes the unsent bodies | red |

**Batch failure and adapter**

| Test | Asserts | Base |
|---|---|---|
| `ExhaustedTransientBatchLeaksAndAdvances` | every attempt answers `SlowDown`: attempts until `Retry::standard` gives up on the injected clock, then `leaked` = batch size, `batches_leaked` 1, cursor advances, no per-key delete | red |
| `PermanentRefusalLeaksAndAdvances` | armed `AccessDenied`, no refresh callback: one attempt, `leaked` = batch size, `batches_leaked` 1, cursor advances, next page taken | red |
| `SlowDownForeverDoesNotStarveLaterDebris` | capability false, key 500 of page 1 answers `SlowDown` on every attempt, forever; 2 dead pages behind it. Budget seam above the retry window: one run deletes every other key of the 3 pages, `leaked` 1, `batches_leaked` 1, cursor past page 3. Default budget: run 1 ends after page 1 with `budget_exhausted` 1 and the cursor past page 1; run 2 drains pages 2–3. After the pass wraps, the next run leaks key 500 again | red |
| `PerKeyErrorInOkResponseLeaksTheBatch` | all but one key deleted, the survivor answers `SlowDown` on every attempt: `janitor_deleted` excludes the batch, `leaked` = batch size, cursor advances, the next pass lists only the survivor | red |
| `AuthorityLostBeforeRetryHolds` | limit 500, one dead 1,000-key page, 2 dead pages behind it. `failNextBulkRemoveWith` arms a `SlowDown` on the first batch; liveness is true only while `bulkRemoveCalls()` is 0, so the reissue fails admission. The in-memory store is the record of the outcome: `bulkRemoveCalls()` 1, all 1,000 keys present, `batches_held` 1, `leaked` 0, `batches` 1, no second batch, no page 2 LIST, no cursor publication | red |
| `AuthorityLostDuringSuccessfulBatchCounts` | same page; `onBeforeBulkRemove` turns liveness false on the first batch, and the delete then applies. `bulkRemoveCalls()` 1, the first 500 keys gone, the second 500 present, `batches` 1, `janitor_deleted` 500, `batches_held` 0, `leaked` 0, no second batch, no page 2 LIST, no cursor publication | red |
| `S3BatchErrorKeepsCanonicalName` | [adapter](#adapter) test: whole-response `EntityTooLarge` and `MalformedXML`, per-key `EntityTooLarge`, one-key `EntityTooLarge` through `deleteFileFromS3` → name preserved, `isDefinitelyRefusedWrite` true | red |
| `NameOnlyRefusalLeaksAndAdvances` | multi-key batch, per-key `EntityTooLarge` through the emulated S3 backend, no refresh callback: one attempt, `leaked`, cursor advances | red |

**Pipeline and jobs**

`gc_io_concurrency` 4 on a real `io_pool`, 3 dead 1,000-key pages unless stated. Orderings use backend
hooks, latches and a test-owned liveness flag that `refresh_authority` also reads; assertions hold for
any interleaving of the jobs. Throughput is not a unit test: an injected clock serializes the jobs'
time; [M1](#m1-long-stream-drain) and [M4a](#m4-no-batch) measure it.

| Test | Asserts | Base |
|---|---|---|
| `PipelineListsNextPageWhileJobsRun` | batch store: page 1's batch blocks in `onBeforeBulkRemove` until the LIST hook sees page 2's LIST, then proceeds. All 3 pages drained, cursor past page 3. On base, or if the round thread waited for the batch, the test deadlocks; a latch timeout fails it | red |
| `UnknownCapabilityConcurrentProbesAllStop` | emulated S3, capability unknown, the store rejects `DeleteObjects`: between 1 and 4 rejected requests, zero `DeleteObject` requests, `capability_learned` 1, cursor unchanged, capability false after; the next run sends one-key jobs and drains | red |
| `ParallelPathScheduleRefusalHolds` | capability false, `io_pool_refuse_at_for_test` 10: `delete_jobs` 10; jobs that found the stop flag send nothing, so `batches` + `delete_jobs_skipped` = 10 and the backend's one-key call count = `batches`; keys absent = `janitor_deleted` = `batches`; the other 1,000 − `batches` keys present and not `leaked`; `batches_held` 1 (the refusal), page held, cursor unchanged | red |
| `RefreshInFlightKeepsAuthority` (integration: the real `refreshAuthority`) | `gc_io_concurrency` 4, backend limit 500, 3 dead 1,000-key pages. A decorator blocks the `gc/state` read of page 2's refresh. Page 1's second job waits in `on_job_start_for_test` until that read has begun, so its first admission falls inside it; page 1's first job fails its first attempt with `SlowDown` in a hook that waits for the same read, so its reissue is gated inside it. The read is released after both jobs' requests have applied. Page 1's 1,000 keys gone, `batches_held` 0, `delete_jobs_skipped` 0, all 3 pages drained, cursor past page 3. With a fail-closed-first refresh both jobs would hold | red |

**Budget, cursor and authority**

| Test | Asserts | Base |
|---|---|---|
| `PhaseBudgetCheckedBetweenPages` | default budget 20 s, clock advances 8 s per page: pages start at 0, 8, 16 s; page 3 completes although it ends at 24 s; no page 4; `budget_exhausted` 1; cursor after page 3 | red |
| `BackendRejectedCursorResetsExactlyAndDeletesNothing` | existing, kept | guard |
| `LaterPageReadFailurePublishesPrefix` | pages 1–2 delete, page 3's catalog GET throws: cursor after page 2 published, no reset, counts of pages 1–2 on the row | red |
| `ObservedAuthorityLossPublishesNothing` | refresh before page 3 fails: no page 3 LIST, no cursor publication, pages 1–2 deletes stand | red |
| `LeaseReplacedBeforeFinalBatch` | hook after the last page's classification, before its final batch: a second leader replaces the durable lease, the table's UUID is re-created under a fresh incarnation and writes keys, and the second leader publishes its maintenance state. This leader's batch is still sent and names only old-life keys; new-life keys survive; this leader's cursor publication returns `Conflict` (recorded by a test wrapper around `casGcMaintenanceState`), no publication anomaly is recorded, and the stored maintenance state is the second leader's. The next invocation's refresh stops it before its first LIST | guard: expected green on base's exact path under the same schedule; kept to pin it on the batch path |
| `RecreatedLifeBetweenListAndDeleteKeepsItsKeys` | from the branch: a new life created between LIST and catalog read keeps its keys | guard |

**Stop rules and observability**

| Test | Asserts | Base |
|---|---|---|
| `SteadyStateTakesOnePage` | 5,000 live keys: `janitor_pages` 1, one maintenance PUT | guard |
| `DeferredRoundTakesOneSuppressedPage` | `DEFER` with debris: 1 page, 0 deletes, cursor unchanged, no PUT | guard |
| `MidStreamPassWrapsToTheStart` | cursor at page 3 of 5, dead keys on every page: pages 3, 4, 5, 1, 2 in one pass, cursor reset to empty, one maintenance CAS | red |
| `WrapStopsAtAnAllLivePage` | cursor at page 3 of 5, pages 3–5 dead, page 1 live: pages 3, 4, 5, 1; page 2 not listed; cursor reset to empty | red |
| `FoldingRoundDrainsWithinBudget` (integration) | one folding round, 3,500 dead keys then live keys: `janitor_pages` 5, all dead keys gone, one maintenance CAS | red |

No new `LOGICAL_ERROR`. If implementation adds one, its test is an `EXPECT_DEATH` whose child exits through
`std::_Exit`, split from the throwing tests.

**Integration.** `tests/integration/test_cas_janitor_drain` on RustFS (`with_rustfs`), no `no-parallel`.
`gc_round_ref_cleanup_budget` = 1 leaves the stream to the janitor; a long `cas_gc_interval_sec` lets the
test drive rounds with `SYSTEM CAS GC RUN`. Setup: `SYSTEM STOP MERGES`, ~2,500 single-row inserts,
`DROP TABLE ... SYNC`. Assert: within 3 rounds no `_log`/`_snap` key of the life remains; exactly one round
has `janitor_deleted` > 0, with `janitor_pages` ≥ 3 and `budget_exhausted` 0. Base leaves keys behind. A
second case sets `s3_objects_chunk_size_to_delete` 100 and asserts `batch_keys` 100, `batches` ≥ 25. A third
case sets `support_batch_delete` false and asserts one deleting round, `batch_keys` 1 and `delete_jobs`
≥ 2,500.

## Out of scope {#out-of-scope}

- Parallel one-key deletes in `cleanupRefObjects` and the manifest cleanup: they stay serial on a no-batch
  store, as base.
- A per-request phase deadline ([why not](#no-request-deadline)); stage C's round deadline owns the tail.
- GCS JSON-API batch deletes.
- The manifest-body memo (`CasGcManifestMemo` on the parked branch).
- Batching `_ckpt`/`_files`: mutable, needs a bulk conditional delete.
- Reclassifying the exact-token path's failures: it keeps leaking on every failure.
- Detecting bucket versioning enabled after mount.
- LIST cuts: skipping the deferred round's page, per-life discovery, overlapping authority and catalog
  GETs.
- Janitor deletes on deferred rounds ([reopen condition](#deferred-rounds)).
- Stage C (round deadline, `requestRoundSoon`).
- The targeted drain of retired lives (frozen).

## Review findings addressed {#findings}

| Finding | Resolution | Section |
|---|---|---|
| r3-1 MAJOR: unknown capability deletes per key after a failed batch | rejection stops the phase; `removeChunkWriteOnce`, no per-key loop anywhere | [Capability](#capability) |
| r3-2 MAJOR: bound ignores 90 s retry windows | soft budget: expected time is budget + page in progress + jobs in flight, failing-store worst case stated; stage C bounds the tail | [Phase bound](#phase-bound) |
| r3-3 MAJOR: permanent error starves later debris | permanent refusal leaks and advances; starvation test | [Batch failure](#batch-failure) |
| r3-4 MAJOR: storage limit may be 0 | getter ≥ 1, CAS clamp; zero-limit tests | [Batch size](#chunk-rule) |
| r3-5 MAJOR: phase counters miss worker requests | logical `delete_calls` on the row; wire requests from process-wide `system.events` over a GC-only window (owner decision 2026-10-01, r10). LIST still runs off the round thread: see r5-3 | [Observability](#observability) |
| r3-6 MAJOR: report 08 is not a same-base baseline | fresh alternating A/B on `e2dd0f5d343`; report 08 gives predictions; per-page time open, measured in M1 | [Context](#context), [Value proof](#value-proof) |
| r3-7 MINOR: authority-loss test vs publication | observed loss publishes nothing. "Sequential, so no in-flight batch" was wrong: see r5-5 | [Authority](#authority) |
| r3-8 MINOR: deferred-round rationale | suppression kept as the conservative policy; reopen condition | [Deferred rounds](#deferred-rounds) |
| r3-9 MINOR: versioned buckets | unversioned precondition; drain ≠ reclamation | [Versioned](#versioned) |
| r3-10 MINOR: test plan | fault schedules; `CasLayout.h` canonical-key check a prerequisite | [Tests](#tests), [Code shape](#code-shape) |
| r4-1 MAJOR: frozen 10 s policy gives ~3 s of work under attempt reservations | no per-request deadline; budget checked between pages; cursor publication policy stated | [Why no per-request deadline](#no-request-deadline), [Cursor](#cursor) |
| r4-2 MAJOR: first-page exemption rests on a false wedge argument | exemption dropped; stage C interplay stated. The "partial page is progress" answer did not hold: see r5-2 | [Phase bound](#phase-bound), [Batch failure](#batch-failure) |
| r4-3 MAJOR: removing the fallback escapes both cleanup families; manifest retry claim false | per-caller containment of `NOT_IMPLEMENTED`; manifest bodies go to the orphan sweep; tests for both callers | [Other families](#other-families) |
| r4-4 MAJOR: parallel capability learning allows same-phase per-key deletes | batch size fixed at phase start, keys of a rejected job never re-cut, phase stop; external flip also stops. Holds with jobs in parallel | [Capability](#capability) |
| r4-5 MAJOR: S3 adapter drops the canonical error name | adapter passes the name at the batch throw sites; the one-key site added in r5-1 | [Adapter](#adapter) |
| r4-6 MINOR: "any exception publishes nothing" loses rejected-cursor recovery | reset kept for page 1's persisted cursor; page ≥ 2 read failure publishes the prefix | [Cursor](#cursor) |
| r4-7 MINOR: test expectations contradict the model | tests rewritten for sequential pages; recreate-after-classification schedule added | [Tests](#tests) |
| r4-8 MINOR: string fingerprints; inline double-count | numeric `sipHash64`, 0 for empty; round-thread delta only | [Observability](#observability) |
| r5-1 MAJOR: a one-key chunk goes through `deleteFileFromS3`, which drops the error name | name passed at `deleteFileFromS3.cpp:77` too; one-key adapter test and `OneKeyNameOnlyRefusalLeaksAfterOneAttempt` | [Adapter](#adapter), [Tests](#tests) |
| r5-2 MAJOR: holding on transient failure starves later debris; no strictly positive progress | exhausted ordinary failures leak and advance, as the exact-token path; holds only for lost authority, `NOT_IMPLEMENTED`, local failures; `SlowDownForeverDoesNotStarveLaterDebris` | [Batch failure](#batch-failure), [Stop rules](#stop-rules) |
| r5-3 MINOR: one logical page is three `ListObjectsV2` on iterator threads | request table per report 06; `list_requests_process`, `list_ms`; M1/M3 criteria on logical pages and process-wide LIST ≤ base | [Pipeline](#pipeline), [Observability](#observability), [M1](#m1-long-stream-drain), [M3](#m3-steady-state) |
| r5-4 MINOR: M4's round-wide expectation contradicts the family-local stop | M4 asserted per family; later families may send one-key requests the same round | [Capability](#capability), [M4](#m4-no-batch) |
| r5-5 MINOR: a lease replacement can overlap an in-flight batch | no-fencing statement; safety rests on terminal life identity; `LeaseReplacedBeforeFinalBatch` | [Authority](#authority), [Tests](#tests) |
| r5-6 MINOR: some "red" tests pass on base | one-key-path assertions on `NoBatchCapabilitySendsOneKeyRequestsWithoutProbe`; `budget_exhausted` on `SlowFirstPageCompletesThenStops`; the lease test marked guard | [Tests](#tests) |
| r6-1 MAJOR: storage limit 1 makes `cleanupRefObjects` run `authorityHolds` before every key, ~10k extra GETs per 5,000-key cleanup on GCS | the storage limit sizes requests, not cohorts; authority once per 1,000-key cohort as today; manifest cleanup gets the same shape; `RefCleanupReadsAuthorityOncePerCohort`; M4 base-vs-after authority GETs | [Cohort and request](#cohort-and-request), [M4](#m4-ref-cleanup), [Tests](#tests) |
| r6-2 MINOR: lease-replacement test expects `Refused` for the maintenance write | the stale-etag write resolves to `Conflict`, silent as today; the competing cursor stands; loss seen at the next invocation | [Authority](#authority), [Cursor](#cursor), [Tests](#tests) |
| r6-3 MINOR: authority-loss test conflates a rejected retry with a successful in-flight delete | split into `AuthorityLostBeforeRetryHolds` and `AuthorityLostDuringSuccessfulBatchCounts`; the in-memory store records each batch's fate | [Authority](#authority), [Tests](#tests) |
| r7-1 MINOR: per-request admission does not check GC leadership in all families | admission is mount fence plus an optional `Liveness`; only the janitor supplies `authority_held`; ref and manifest cleanup check leadership per cohort as base does | [Cohort and request](#cohort-and-request) |
| r7-2 MINOR: a credential refresh can reissue a batch without new admission | guarantee restated as one admitted request = one storage request except a refresh reissue; "one attempt" tests run without a refresh callback; refresh-callback test added | [Decision](#decision), [Adapter](#adapter), [Tests](#tests) |
| r7-3 MINOR: capability plumbing omits the production decorator | `InstrumentedBackend` forwards the limit and any capability accessor; `PoolWrappedBackendForwardsStorageLimit` | [Batch size](#chunk-rule), [Code shape](#code-shape), [Tests](#tests) |
| r7-4 MINOR: `blob_storage_log` counts objects, not requests | alternative dropped; `DiskS3DeleteObjects` stays the primary source, with a stated fallback to the server-side total | [Observability](#observability) |
| owner change 2026-10-01 (a): serial one-key deletes drain ~1,000 keys per 20 s page on a no-batch store; 10 s budget too small | one-key jobs on `io_pool` (16), admission per job on a resumed `CasOperation` with the authority predicate, per-job classification, stop flag; `kJanitorPhaseBudget` 20 s and every dependent figure recomputed; M4a base-vs-after drain | [Decision](#decision), [Jobs](#jobs), [Observability](#observability), [M4](#m4-no-batch), [Tests](#tests) |
| owner change 2026-10-01 (b): one fan-out for both capability cases; LIST page k+1 while page k's jobs run | [pipeline](#pipeline): a job is one batch or one key, ≤ `gc_io_concurrency` outstanding, one final wait, one publication of the complete prefix; `authority_held` atomic. Parallel batches across pages leave Out of scope. It answers the three objections that kept r4–r8 sequential: **frozen policy envelopes** (r4-1): every request keeps `Retry::standard` and no policy is frozen; **first-page exemption** (r4-2): none, the budget only stops new pages; **capability race** (r4-4): batch size fixed at phase start and no re-cut of a rejected job's keys | [Decision](#decision), [Pipeline](#pipeline), [Why no per-request deadline](#no-request-deadline), [Capability](#capability), [Tests](#tests) |
| owner change 2026-10-01 (c): the budget is best effort; the capability is remembered per disk | soft limit: no new page after it, page in progress and jobs in flight complete; the bound section states expected time and the failing-store worst case, no ceiling. No sequential probe: up to `gc_io_concurrency` rejected probes on the first phase a process runs on the disk, all stop | [Budget and in-flight work](#phase-bound), [Capability](#capability), [M4](#m4-no-batch) |
| r9-1 MAJOR (codex round 6): `refreshAuthority` stores `false` before its GET, so a job admitted or retried during the next page's refresh holds under unchanged leadership; `admitted()` re-samples, so a refusal can be classified as a leak | refresh stores its verdict once, after the read, `false` only on absence, mismatch or failure; loss latched for the phase by a predicate wrapper; a refused admission re-samples monotone state and holds; `RefreshInFlightKeepsAuthority`, `LivenessRefusalIsLatchedForThePhase` | [Decision](#decision), [Authority](#authority), [Code shape](#code-shape), [Tests](#tests) |
| r9-2 MINOR (codex round 6): inline jobs double-count wire deletes | the row has no wire count; logical counts come from job slots, read once, inline or not; `ParallelPathAtConcurrencyOneRunsInline` asserts exact counts with a retry | [Jobs](#jobs), [Observability](#observability), [Tests](#tests) |
| r9-3 MINOR (codex round 6): two concurrency tests assert orderings their hooks do not establish | `HeldPageStopsCursorButLaterDeletesStand` gets a backend barrier on page 1's and page 3's completion; `ParallelPathScheduleRefusalHolds` asserts sent, skipped and deleted counts by identity; a job past its stop-flag check runs to its outcome | [Jobs](#jobs), [Tests](#tests) |
| r9-4 MINOR (codex round 6): M4a mixes janitor-only and whole-drain accounting; the drain prediction omits the fold's ~5,000 serial ref deletes | answered by the owner's measurement decision below: every M4a row names its scope, criteria compare like with like, the round-1 ref cleanup (~100 s at 20 ms) is in the drain prediction | [Value proof](#value-proof), [M4](#m4-no-batch) |
| owner decision 2026-10-01 (r10): M4/M4a on the local store with `support_batch_delete` false in the disk config (one `DeleteObject` per key), base vs after, drain wall time and authority GETs; GCS stand dropped | M4 on RustFS with netem 20 ms; M4b leaves the A/B for the emulated-store tests, since the local store accepts `DeleteObjects` | [Decision](#decision), [M4](#m4-no-batch) |
| owner decision 2026-10-01 (r10): A/B request attribution from process-wide `system.events` deltas in a GC-only window, as `utils/ca-soak/gc-ab` and the yardstick | `delete_requests`, the per-job `ProfileEvents` deltas and `WireRequestsIncludeRetriedAttempts` removed; the row keeps logical counts; tests count wire requests at the emulated store | [Observability](#observability), [Value proof](#value-proof), [Tests](#tests) |
