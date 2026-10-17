---
description: 'Design for reworking PR #2351: two round-scoped CAS GC thread pools sized by one disk setting, cas_gc_concurrency, and a parallel pending_deletes fan-out whose failures are all logged and counted.'
sidebar_label: 'CAS GC round pools design'
sidebar_position: 100
slug: /superpowers/specs/cas-gc-round-pool-and-parallel-redelete-design
title: 'CAS GC: round-scoped pools and the parallel pending_deletes fan-out'
doc_type: 'reference'
---

# CAS GC: round-scoped pools and the parallel `pending_deletes` fan-out {#cas-gc-round-scoped-pools-and-the-parallel-pending-deletes-fan-out}

- Date: 2026-09-14
- Status: approved design, revision 11, final. Review history: revision 5 passed with no MAJOR; revision 7 (fallback tightening) drew one MAJOR, fixed in revision 8; revisions 8, 9 and 10 passed with no MAJOR and their MINORs are folded in through revision 11. Reports: `tmp/review2351/codex/round*_findings.md`. Review history: revision 5 passed with no MAJOR; revision 7 added the meta-writer fallback tightening and its review found one MAJOR (join-guard declaration order), fixed in revision 8, which passed with three wording MINORs folded in here. Revision 9 also adds section 3.6, a consolidated view of exception paths and counters with no new semantics. Revisions 1 to 4 and three Codex review rounds (`tmp/review2351/codex/round*_findings.md`) converged on the fact that sharing one pool between fire-and-forget meta writes and result-bearing fan-outs needs a bespoke completion contract; revision 5 removes that sharing instead of specifying it.
- Reworks: Altinity/ClickHouse PR #2351 "CAS: parallel delete blobs" (`cas/gc-parallel-delete-blobs`, base `antalya-26.6`)
- Closes backlog item: "GC per-disk thread pools → one server-wide pool" (2026-09-07, `docs/superpowers/cas/BACKLOG.md` on `cas-gc-rebuild`), whose goal is zero idle GC threads per disk between rounds
- Implementer: the PR author, continuing on the PR branch with new commits (no rebase, no amend)

## 1. Problem {#1-problem}

PR #2351 parallelises the GC `pending_deletes` phase (one `HEAD` plus one conditional `DELETE` per blob) and measures a 3.2x speed-up at concurrency 4. The review found no data-safety defect, but:

1. It adds a third `ThreadPool` owned per `Gc` (per CAS disk). The two existing ones (`read_pool`, `GcMetaWriter::pool`) already hold 17 to 32 idle threads per disk for the disk's lifetime, measured 523 threads at 30 inline disks; the ASan memory ceiling on stateless lanes scales with that count.
2. The improvement ships disabled (`cas_gc_redelete_concurrency` default 1) with no stated reason, and a second knob (`cas_gc_redelete_min_batch_size`) duplicates what `GcReadAhead` solves internally.
3. The tests cannot distinguish the parallel path from the sequential one.
4. Per-blob failures beyond the first are neither logged nor counted.
5. The four safety comments of the old loop were dropped in the extraction.

## 2. Goals and non-goals {#2-goals-and-non-goals}

Goals:

- Zero GC pool workers per CAS disk between rounds (the scheduler's own scheduling and heartbeat threads are not pool workers and stay); at most `2 × cas_gc_concurrency` pool workers during a round.
- One disk setting governs every GC fan-out.
- Parallel `pending_deletes` on by default, every failure visible, bounded memory regardless of backlog size.
- Tests that fail if the parallel path silently stops running.

Non-goals:

- One pool shared by all consumers (tried in revisions 1 to 4; the completion contract it needs is not worth the thread it saves).
- A process-wide cap across disks whose rounds overlap.
- Any change to the GC protocol, budgets, lease handling, outcome ordering, or `blobUploadPool`. The meta writer's exception and drain semantics change in exactly one place (3.1, the inline fallback); everything else about it is preserved.
- Attributing worker-thread ProfileEvents to phase rows (the existing gap stays, documented as it is for `meta_pool_wait`).

## 3. Design {#3-design}

### 3.1 Two pools per round, none per disk {#3-1-two-pools-per-round-none-per-disk}

`Gc` owns no `ThreadPool` and no `GcMetaWriter`. It owns `std::shared_ptr<GcMetaWriter::State>` (the cross-round state: `scheduled`/`completed` counters and the in-process confirmed condemn markers), which today already lives behind a `shared_ptr` inside the writer.

A `GcRound` object is constructed at the top of `runRegularRound` and of `rebuildBaseline` and destroyed when the verb returns or throws. It holds, in declaration order:

1. `ThreadPool io_pool`: `max_threads = max_free_threads = cas_gc_concurrency`, unbounded queue (the hinting sites throttle themselves, as today), `shutdown_on_exception = false`, dedicated metrics `CASGCIOPoolThreads`, `CASGCIOPoolThreadsActive`, `CASGCIOPoolThreadsScheduled`.
2. `GcMetaWriter meta_writer`: constructed exactly as today (`store`, `logger`, pool size), except that the pool size is `cas_gc_concurrency` and the `State` is passed in rather than created. Its pool keeps today's construction (queue limit equal to thread count, which is its backpressure) with dedicated metrics `CASGCMetaPoolThreads`, `CASGCMetaPoolThreadsActive`, `CASGCMetaPoolThreadsScheduled`.

`GcRound`'s destructor body runs before its members are destroyed, so the order is written out explicitly: the body first calls `meta_writer.drainOnExitNoThrow()` (today's round-exit `SCOPE_EXIT`, moved here), then `io_pool.wait()` inside a `try` that logs and swallows (a `ThreadPool` destructor only finalizes; it neither rethrows nor logs a pending `first_exception`); after the body, members are destroyed in reverse declaration order, the meta writer (whose own destructor drains again, finding nothing) and then `io_pool`. Consumers join their own jobs before returning on every path (3.3 and the existing `GcReadAhead` destructor), so these waits normally find nothing outstanding. Neither pool's jobs depend on the other pool draining, so the order carries no deadlock.

Pre-commit barrier, in both round verbs: in `runRegularRound` where the round drains the meta writer today, and in `rebuildBaseline` immediately before its own commit (it has no meta drain today, so the barrier there is the I/O wait alone), the round calls `io_pool.wait()`, which rethrows a pending I/O-pool framework failure and fails the verb before its commit, symmetric with `drain` on the meta pool. This is the one place a framework failure on the I/O pool is guaranteed to surface on a successful path; a successful submission does not surface it (`scheduleImpl` consults `first_exception` only on its failure path), and the destructor only logs.

Consumers are unchanged in shape:

- `GcReadAhead` and the orphan-manifest sweep take `ThreadPool &` today and keep doing so; they receive `round.io_pool` and `round.concurrency()`.
- `Gc::fold`, `applyRedeleteOutcome` and `runRegularRound` reach the meta writer through `round.meta_writer` instead of the removed member. Its API (`scheduleCondemnMarkerWrite`, `scheduleConfirmedMetaDelete`, `drain`, `scheduled`, `completed`, the condemn-marker queries) does not change; `drain` is `pool.wait()` and therefore still rethrows, at the round's pre-commit barrier, any framework failure pending in the pool at that moment. One tightening: today `submit`'s fallback catches whatever `scheduleOrThrowOnError` throws and runs the operation inline, and `scheduleImpl` rethrows a pending worker exception (clearing the pool's record of it) before it reports a refusal, so a framework failure from an earlier job could be logged as a scheduling failure and never reach the barrier. Under this design the inline fallback applies only to `CANNOT_SCHEDULE_TASK`; any other exception thrown by `scheduleOrThrowOnError` propagates out of `submit` unchanged (after `completed` has been accounted for the never-run operation, so the rejected operation itself never leaves `scheduled` ahead of `completed`; the writer-wide equality is a property of operations that ran or were rejected, and a job that let an exception escape its body, or that the pool discarded after entering shutdown, is a framework failure outside that accounting: it fails the round when its exception is propagated by a later `submit` or by `drain`, with one acknowledged exception consistent with the code-based policy: an escaped exception that itself carries `CANNOT_SCHEDULE_TASK` is indistinguishable from a refusal, takes the inline path, and leaves an accounting gap of one in a round that may still succeed), fails the fold or apply step that called `submit`, and therefore the round, before the commit. The check is by error code, since the rethrown exception keeps its original type and code.
- `redeleteBlobs` takes `ThreadPool &` (3.3).

`ThreadPool` borrows workers from the global pool and starts them on the first job; workers return to the global pool at round end, where other disks' rounds reuse them. Between rounds a disk holds zero GC pool workers. During a round it holds at most `cas_gc_concurrency` I/O workers plus `cas_gc_concurrency` meta workers, the same peak as today's 16 plus 16.

The `shutdown_on_exception = false` flag on `io_pool` closes a hazard that exists today: with the default, one job letting an exception escape shuts the pool down and discards queued jobs, and a discarded read-ahead job leaves its waiter blocked forever in `take`. Under the flag, queued jobs still run; the pool keeps the first escaped exception and rethrows it from `wait`. No I/O consumer lets an exception escape a job (each settles a promise), so this is a guard, not a path.

### 3.2 Settings {#3-2-settings}

| Key | Scope | Default | Meaning |
|---|---|---|---|
| `cas_gc_concurrency` | disk | 16 | Worker count of each of the two per-round GC pools: the I/O pool used by the fold's read-ahead, the orphan-manifest sweep and the `pending_deletes` fan-out, and the meta-write pool. `1` gives each pool a single worker, so every fan-out runs one job at a time. Must be at least 1. The round thread's own inline requests are not counted. |

Removed: `cas_gc_read_concurrency`, `cas_gc_meta_pool_size`, `cas_gc_redelete_concurrency`, `cas_gc_redelete_min_batch_size`. An old key fails config load with the existing unknown-`cas_` diagnostic. No alias: the branch is pre-release; the docs carry a migration note.

`PoolConfig` carries one field, `gc_concurrency = 16`, with a comment in the style of its neighbours. `ContentAddressedMetadataStorage` copies it as it copies the others. There is no server-level key.

### 3.3 The `pending_deletes` fan-out {#3-3-the-pending-deletes-fan-out}

The extraction from the PR stays: `performRedeleteIo` (worker side: `HEAD`, compare token to etag, conditional `DELETE`; absent is `Gone`, token mismatch is `Mismatch`) and `applyRedeleteOutcome` (round thread: `BlobDelete` event, audit row under the outcome budget, `report.redeleted`, `CASGCRetiredRedeleted`, meta drop on `Removed` or `Gone` only, `forgetCondemnMarker` always). Changes:

- Signature: `redeleteBlobs(const std::vector<RetiredEntry> & entries, ThreadPool & pool, size_t concurrency, CasOperation & op, RedeleteRoundContext & ctx)`. It always uses the pool; with one worker the behaviour is sequential. There is no `min_batch_size` branch, no separate sequential loop, and `redeleteBlob` is removed.
- `RedeleteRoundContext { uint64_t new_round; uint64_t round_generation; GcRoundWorkBudget & budget; RoundReport & report; OutcomeLog & outcome_log; GcMetaWriter & meta_writer; }`, the writer being `round.meta_writer`, which `applyRedeleteOutcome` needs for `scheduleConfirmedMetaDelete` and `forgetCondemnMarker`. `round_generation` feeds the event row; `op.generation()` is what workers resume under. The two are distinct on purpose and named so.
- Fan-out mechanism: `ThreadPoolCallbackRunnerLocal<RedeleteIo>` over the round's I/O pool (`src/Common/threadPoolCallbackRunner.h`), through its `enqueueAndGiveOwnership` entry, which returns a `std::shared_ptr<Task>` whose `future` the caller moves into its own storage; the runner attaches the submitting thread's `ThreadGroup` to each callback (a null group on a bare test thread is accepted). `redeleteBlobs` keeps the moved futures in a caller-owned bounded deque (the window) and joins them itself; the runner keeps no record of tasks handed over, so its destructor is not a backstop for them. Declaration order in `redeleteBlobs` is: the runner, then the deque, then a scope guard that waits on every future still in the deque. Locals are destroyed in reverse order, so the guard runs first and joins while the deque and the runner are still alive; an exceptional exit can never destroy a slot a callback may still write. `RedeleteIo` stays a plain value.
- Submission window: the round thread walks entries in index order. Before waiting on entry i it has submitted entries up to `i + 2 × concurrency - 1`; consuming entry i's future frees a slot. Memory held at any time is bounded by the window, not by the backlog (`gc_round_redelete_budget` defaults to unlimited, so it is not the bound).
- Each callback captures by value: the `PoolPtr`, `gen`, a copy of the `RetiredEntry`, and a `const Layout *` (the pool-owned layout outlives the round). No `this`, no `[&]`. Each builds its own `CasOperation` via `store->openRequests().resume(gen)`, as `GcReadAhead::hint` does.
- Joining: the caller-owned futures are the join. On every exit path, including a submission failure and an apply-phase exception, `redeleteBlobs` waits for every callback it submitted before it logs, before it rethrows, and before its locals are destroyed. It never calls the pool's `wait`, so it never waits for unrelated jobs.
- The four comments from the old loop move onto the corresponding statements: why `HEAD` precedes the conditional delete; why a mismatch is terminal-OK; why the audit row is observability only; why the meta is dropped only on `Removed` or `Gone`.

### 3.4 Errors {#3-4-errors}

Per entry, unchanged: a failed `HEAD` or `DELETE` leaves the entry `delete_pending` with its condemn marker intact, records no outcome row, and it is retried next round.

Per batch:

- Worker failure (`HEAD`, conditional `DELETE`, or `resume` refusing because the fence moved): arrives through the entry's future. The round thread records it in a per-slot `std::exception_ptr`, logs it at Warning with the blob key, condemn round and exception text, counts it in `RoundReport::redelete_failed` and in the ProfileEvent `CASGCRetiredRedeleteFailed`, and continues the walk. Successful entries are applied in index order. After the walk, the lowest-index failure is rethrown. N failures produce N log lines, a count of N, and one exception.
- Submission failure (the runner's submit throws `CANNOT_SCHEDULE_TASK` at index k): entries before k are walked and applied as above; k and later are never attempted and stay pending; the failure is logged and counted once; it is rethrown only if no worker failure exists at a lower index, so the surfaced message is the earliest real cause.
- Apply-phase failure (`applyRedeleteOutcome` throws for entry j): the outstanding futures are joined and inspected, every failure among them is logged and counted like any other worker failure, and then the apply exception propagates and wins. "Every failure visible" holds on this path too; only the exception type surfaced to the caller is the apply one. Entries whose I/O completed but were not applied replay next round from a fresh observation, not from this round's result: `performRedeleteIo` re-issues `HEAD` and decides again. If the backend did not change in between, a removed blob replays as `Absent`, a `Mismatch` replays as `Mismatch`, and an entry whose request failed before any mutation, and whose condemned incarnation is still present, replays as `Deleted` once the retry succeeds (if a failed `HEAD` had targeted an already absent or replaced blob, the replay is `Absent` or `Mismatch`); if it did change (the replacement was removed, or a `DELETE` took effect remotely before the transport failure was reported), the replay reflects the new state, typically `Absent`. Every case is correct by construction because the exact-etag delete matches only the incarnation that was condemned. Entry j itself may be partially applied: `applyRedeleteOutcome` emits the event, appends the audit row and counts `redeleted` before it reaches the meta writer, and the meta writer's `submit` is now a propagation point (3.1), so a failure there leaves entry j counted and recorded in memory but with its condemn marker not yet forgotten. No `GcOutcomes` object is persisted and no round commit happens on this path; the physical delete of entry j stands, and its `BlobDelete` event may already have reached `system.cas_log`, which is the intended audit of what physically happened. The next round re-observes entry j and forgets the marker then. This window is documented at `RoundReport::redeleted` and in a comment at the meta-writer call inside `applyRedeleteOutcome`, and covered by tests 9 and 9a.
- Round outcome: unchanged. A throwing round leaves only attempt-scoped debris; lease and `gc/state` handling are untouched.

### 3.5 Observability {#3-5-observability}

| Layer | Addition |
|---|---|
| ProfileEvents | `CASGCRetiredRedeleteFailed`, one per failed slot (existing `CASGCRetiredRedeleted` per applied entry stays). `CASGCMetaWriteAnomaly` is removed (declaration, docs, tests); the meta writer keeps its `tryLogCurrentException` on a failed operation and no counter replaces it |
| `RoundReport` and the GC round log record | `redelete_failed` next to `entries_redeleted`; `gc_concurrency` (the effective setting) |
| `pending_deletes` phase timer | `jobs_scheduled`, `jobs_failed`, `pool_wait_ms` (round-thread time blocked in its own future waits), following the `meta_pool_wait` pattern; a comment stating, as `fold_ref_intake` does, that requests performed on pool workers land on those workers' ProfileEvents |
| `system.metrics` | the six `CASGCIOPool*` and `CASGCMetaPool*` metrics; both are 0 on an idle server |

### 3.6 Exception paths and counters at a glance {#3-6-exception-paths-and-counters-at-a-glance}

This section restates 3.1, 3.3, 3.4 and 3.5 in one place; it adds no rule.

Where an exception can arise, where it goes, and who fails:

| Origin | Carried by | Surfaces at | Effect on the round |
|---|---|---|---|
| I/O job body (`HEAD`, conditional `DELETE`, `resume` refusing on a moved fence) | the job's `std::promise` (`ThreadPoolCallbackRunnerLocal::executeCallback` catches everything into it) | `future.get()` on the round thread inside `redeleteBlobs` | logged and counted per entry; siblings applied; lowest-index failure rethrown after the walk; round fails |
| read-ahead job body | the slot's promise (`GcReadAhead::hint`, unchanged) | `take` on the round thread, at the site that would have read inline, if the slot is consumed; a hinted slot that is never taken is waited for and its exception dropped by `discard` or the `GcReadAhead` destructor, unchanged | round fails from the `take` site; an unconsumed failed hint does not fail the round, unchanged |
| meta-write operation | caught inside the job, `tryLogCurrentException` (unchanged) | log only | none; advisory write, `completed` still incremented |
| `enqueueAndGiveOwnership` or `scheduleOrThrowOnError` refusing with `CANNOT_SCHEDULE_TASK` | thrown to the submitter | `redeleteBlobs`: submission failure of 3.4; read-ahead: `hint` erases the slot and rethrows (unchanged); meta writer: inline fallback (unchanged) | redelete and read-ahead fail the round after joining what they submitted; meta writer continues |
| an exception that escaped an earlier job body (framework failure) | stored by the pool as `first_exception`; rethrown and cleared by the next submission that takes the pool's failure path (a refusal), or by `wait`; a successful submission leaves it pending | meta writer: `submit` propagates it unless its code is `CANNOT_SCHEDULE_TASK`, in which case it is indistinguishable from a refusal and takes the inline path (3.1); I/O pool: a fan-out submit that fails sees it and propagates; otherwise the pre-commit barrier (`drain` on the meta pool, `io_pool.wait()` on the I/O pool) | fails the round before commit when seen by a propagating `submit`, a failing fan-out submit, or the barrier; the one gap is the `CANNOT_SCHEDULE_TASK`-coded case on the meta pool; on a throwing exit the destructor waits only log |
| `applyRedeleteOutcome` on the round thread (event sink is contained by `EventDispatcher`; realistic origins are the meta writer's propagating `submit` and allocation) | direct | `redeleteBlobs`: outstanding futures joined and inspected first | apply exception propagates and wins; round fails; entry j may be partially applied (3.4) |
| `GcRound` destructor body (meta `drainOnExitNoThrow`, then guarded `io_pool.wait()`), then member destruction | never leaves it | log | none; on a successful exit both pools were already drained at the barrier, on a throwing exit the round is already failing and the original exception is preserved |

Counters, complete list for this change:

| Kind | Name | Change |
|---|---|---|
| ProfileEvents | `CASGCRetiredRedeleted` | kept, one per applied entry |
| ProfileEvents | `CASGCRetiredRedeleteFailed` | new, one per failed slot (worker failure or submission failure) |
| ProfileEvents | `CASGCMetaWriteAnomaly` | removed |
| ProfileEvents | `CASGCMetaOps`, `CASGCReadAheadHit`, `CASGCReadAheadMiss`, `CASGCReadAheadWasted`, request-level `CASRequest*` and `CASGCHead`/`CASGCDelete` | unchanged; those incremented on pool workers are visible in `system.events` and absent from phase rows, as today |
| CurrentMetrics | `CASGCIOPoolThreads`, `CASGCIOPoolThreadsActive`, `CASGCIOPoolThreadsScheduled`, `CASGCMetaPoolThreads`, `CASGCMetaPoolThreadsActive`, `CASGCMetaPoolThreadsScheduled` | new, replace the shared `LocalThread*` triple for the two GC pools |
| `RoundReport` and GC round log | `redelete_failed`, `gc_concurrency` | new fields |
| `pending_deletes` phase row | `jobs_scheduled`, `jobs_failed`, `pool_wait_ms` | new metrics |

## 4. Tests {#4-tests}

Failing-first order: write each test, watch it fail against the PR as posted, then implement. Suite names must match the `CAS*` gate filter.

New `src/Disks/tests/gtest_cas_gc_round_pools.cpp`:

1. `CASGCRoundPools.NoThreadsBetweenRounds`: after `runRegularRound` returns, `CASGCIOPoolThreads` and `CASGCMetaPoolThreads` are 0. During a round with concurrency 4 and 6 blobs to redelete, an overlap-witness backend (as in `gtest_cas_gc_read_ahead.cpp`) records peak in-flight `head` or `remove` greater than 1.
2. `CASGCRoundPools.ConcurrencyOneIsSequential`: the same round with `gc_concurrency = 1`; the witness records peak in-flight `remove` of 1 during `pending_deletes`; outcomes, events and audit rows are identical to the concurrency-4 run.
3. `CASGCRoundPools.MetaStateSurvivesRounds`: a condemn marker confirmed in round N is still confirmed in round N+1 (the `State` outlives the per-round writer), and `scheduled == completed` when each round returns.
3a. `CASGCRoundPools.MetaSubmitDistinguishesRefusalFromWorkerFailure`: two cases on a writer constructed directly over its own `State`. (a) A pool seam makes `scheduleOrThrowOnError` throw `CANNOT_SCHEDULE_TASK`: the operation runs inline on the calling thread, `scheduled == completed`. (b) A previous job is made to let an exception with a code other than `CANNOT_SCHEDULE_TASK` escape its body (a test-only job submitted through the same seam; the code matters because the policy is by code and a worker exception carrying `CANNOT_SCHEDULE_TASK` would legitimately take the inline path), so the pool holds it as pending; the next `submit` propagates that exception with its original code and does not run the operation inline; the rejected operation is counted, so the only gap between `scheduled` and `completed` is the escaped job itself (`scheduled - completed == 1` if the seam's job was counted as scheduled, 0 otherwise; the test states which).
4. `CASGCRoundPools.RoundThrowingMidPhaseJoinsBothPools`: a fault injected after I/O jobs and meta jobs are queued makes the round throw; assert no job runs after the round verb returned, both pool metrics are 0 afterwards, and the exception the caller receives is the injected one (the destructor's logging did not replace it).
4a. `CASGCRoundPools.IOPoolFrameworkFailureFailsTheVerbBeforeCommit`: a test-only job submitted through a pool seam lets an exception escape its body on the I/O pool while later submissions succeed (request faults cannot exercise this: the runner catches them into promises); for each of `runRegularRound` and `rebuildBaseline`, the verb throws that exception at its pre-commit barrier, no `gc/state` commit or baseline publish happened, and the next verb is healthy. Omitting the barrier's `io_pool.wait()` in either verb fails this test.

Reworked `src/Disks/tests/gtest_cas_gc_redelete_concurrency.cpp`:

5. `WorkerRemoveFaultIsLoggedCountedAndRetried`: fault backend armed on one key and firing only on a non-round thread (the `armAgainstOtherThreads` idiom); the fault throws before the object is removed. Assert `redelete_failed == 1`, `CASGCRetiredRedeleteFailed` incremented by 1, `report.redeleted == 5`, five `BlobDelete` events with outcome `Removed` via the event-sink idiom from `gtest_cas_event_log.cpp`, no `GcOutcomes` object persisted for the throwing round. The next round deletes the sixth blob (`Deleted`) and replays the five siblings as `Absent`.
6. `WorkerHeadFaultIsLoggedCountedAndRetried`: same shape, fault in `head`.
7. `TwoFailuresProduceTwoLogLinesAndOneException`: two keys armed; count is 2; the rethrown exception names the lower-index key.
8. `ReplacedEntryInsideAParallelBatch`: one blob re-published with a new etag before the round; it is spared as `Replaced` while its siblings are deleted.
9. `ApplyPhaseFaultLeavesSiblingsForNextRound`: a test-only hook in `PoolConfig` (`gc_redelete_apply_hook_for_test`, the existing `*_hook_for_test` pattern) throws from inside `applyRedeleteOutcome` for entry j. The event sink cannot be used: `EventDispatcher` catches and logs sink exceptions. The fixture keeps the batch within one submission window so every entry is submitted before the hook fires, and no request fault is armed, so every unapplied sibling was removed. The round throws; `redeleted` equals the applied count; the next round records every unapplied sibling as `Absent`. The hook fires at the start of `applyRedeleteOutcome`, before any effect of entry j, so entry j is neither counted nor recorded and replays as `Absent` with the others. The hook must not capture test-frame locals by reference.
9a. `ApplyPhaseFaultWithLaterWorkerFailureLogsBoth`: the same hook fires for entry j while a later entry's request is armed to fail on a pool worker; the round throws the apply exception, and the later failure is still logged once and counted in `redelete_failed` and `CASGCRetiredRedeleteFailed`. A cleanup that only waits, without inspecting, fails this test.
9b. `MetaSubmitFailureInsideApplyLeavesAPartialEntry`: the meta writer's pool seam makes `submit` propagate (3.1) for entry j; assert the event and `redeleted` for j were recorded, its condemn marker is still confirmed in `State`, the round throws, and the next round observes j absent and forgets the marker.
10. `SubmissionWindowBoundsOutstandingWork`: a batch larger than `4 × concurrency` never holds more than `2 × concurrency` futures in the caller-owned deque (the bound the caller controls), and `CASGCIOPoolThreadsScheduled` never exceeds `2 × concurrency + concurrency` (the pool releases a job's count when it destroys the job, after the promise was fulfilled, so a replenishment can observe up to `concurrency` completed jobs still counted) (observed through a test seam on the deque size); a submission refusal injected at index k (pool seam) applies entries before k, leaves k and later pending, and the round throws.

No death tests: no `LOGICAL_ERROR` path is introduced.

`gtest_cas_gc_meta_writer.cpp` schedules jobs through `metaWriterForTest` without a round and inspects the writer between rounds. It migrates to constructing a `GcMetaWriter` directly over a `State` it owns (the writer is a standalone class) where it tests the writer, and to reading the shared `State` accounting through `Gc` where it tests cross-round bookkeeping; `metaWriterForTest` is removed.

`gtest_cas_settings.cpp`: `gc_concurrency` default 16; `0` rejected with the exact message; each removed key rejected as unknown. Existing read-ahead tests switch from `gc_read_concurrency` to `gc_concurrency`. Tests and docs that reference `CASGCMetaWriteAnomaly` are updated for its removal.

## 5. Documentation and changelog {#5-documentation-and-changelog}

- `docs/en/operations/storing-data.md`, `docs/en/antalya/cas/configuration.md`, `docs/en/antalya/cas/architecture/garbage-collection.md`: replace the `cas_gc_read_concurrency` and `cas_gc_meta_pool_size` rows with one `cas_gc_concurrency` row; one sentence that the pools exist only for the duration of a round; the migration note listing the four removed keys; and the failure sentence: "An entry is recorded as deleted only if its own `HEAD` and delete ran; entries whose request failed, or that were never submitted, stay pending and are retried next round."
- Changelog category: Performance Improvement. Entry: "GC `pending_deletes` runs its `HEAD` and conditional `DELETE` requests in parallel on the round's I/O pool, shared with the fold's read-ahead and sized by the new `cas_gc_concurrency` (default 16). GC thread pools now live only for the duration of a round; idle GC pool workers per CAS disk drop to zero between rounds. Removed `cas_gc_read_concurrency` and `cas_gc_meta_pool_size`."
- PR body links the backlog item it closes and this spec.

## 6. Acceptance before requesting review {#6-acceptance-before-requesting-review}

- `CAS*` gtest gate green under ASan and TSan on the exact pushed tree; the PR does not tick the TSan exclusion.
- Thread census: `SELECT metric, value FROM system.metrics WHERE metric LIKE 'CASGC%PoolThreads'` returns 0 for both on an idle server with several CAS disks mounted.
- Bench re-run at `cas_gc_concurrency` 1, 4 and 16 on the same 21k-blob backlog as the PR's table, with the S3 client's max-connections setting recorded next to the results.
- Stateless CAS S3 lanes green, or each red traced to a cause outside the PR. Known lane issues, not this PR's: the ASan memory-accounting storm (fix in PR #2349) and the ASan fake-stack degradation on fiber threads (backlog item "ASan memory ceiling on stateless lanes = thread count × fake stack").

## 7. Verification items for the plan {#7-verification-items-for-the-plan}

1. `rebuildBaseline` uses only the I/O pool today; confirm it needs no meta writer, or give its `GcRound` one for symmetry.
2. `ThreadPoolCallbackRunnerLocal` captures the submitting thread's group through `getCurrentThreadGroup`, which returns null on a bare thread, and `ThreadGroupSwitcher` accepts null; no guard is needed on the gtest path. Confirm only that the callback's `CurrentThread` state is what `CasOperation` expects for its ProfileEvents.
3. In production code no caller reaches the meta writer outside a round (`fold`, `applyRedeleteOutcome` and `runRegularRound` are the only users); the test access is migrated as described in section 4.

