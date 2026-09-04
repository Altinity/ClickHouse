---
description: 'Design for a content-addressed disk teardown that no longer waits out a GC round: the open request plane carries a teardown fence, so an in-flight round is refused at its next request, its next retry sleep or its next streamed body chunk, and the round is recorded as Stopped rather than Aborted'
sidebar_label: 'CAS GC teardown stop'
sidebar_position: 10
slug: /superpowers/specs/cas-gc-teardown-stop-design
title: 'CAS GC does not hold up teardown'
doc_type: 'design'
---

# CAS GC does not hold up teardown {#cas-gc-teardown-stop-design}

**Status:** IMPLEMENTED, rev.5 (2026-09-04) — see the implementation record at the end for the
three things the build changed. rev.4 was the last review draft. rev.1–rev.3 were reviewed by `codex`
(`gpt-5.6-sol`, high, one resumed session) with Occam's razor as the first question; every accepted
finding is folded in below and the record is at the end. rev.3's verdict: direction sound, joins and
lifetimes preserved, first failing-first test named (T3). Line references are against `86b60a23de4` on `cas-gc-rebuild` and will drift; the symbol
names will not.

## Decision {#decision}

A content-addressed disk's teardown — server `shutdown`, the storage destructor, `SYSTEM CAS FORGET` —
arms one flag before it takes any lock that can block and before it joins any thread. The pool's open
request plane (`Pool::openRequests`) carries that flag as its `Fence`, so a GC round in flight is
refused at its next request attempt, woken from its next retry sleep, and refused at its next chunk
of a streamed body. The joins stay; the ownership of every object stays; the round becomes short
instead of the join becoming optional.

A round cut this way is recorded with a new outcome, `Stopped`, distinct from `Aborted`, defined
honestly: a transient failure observed after teardown began.

What this bounds: the **additional** wait that GC adds to a teardown — today unbounded, afterwards at
most **the remainder of one interval between two admission observations** on the open plane.
Admission is observed before each request attempt, before each retry sleep, and before each refill
of a streamed body; it is not observed after a request returns. One interval can therefore contain,
in sequence:

1. the I/O already in flight when the arm lands — one control-plane attempt (`attempt_timeout_ms`,
   5 s at the default, `ContentAddressedSettings.cpp:80`), or one SDK refill of a streamed body
   **including the SDK's own retries inside that refill** (`ReadBufferFromS3::nextImpl` reissues up to
   `max_single_read_retries` times with a doubling backoff from 100 ms,
   `src/IO/ReadBufferFromS3.cpp:184-227`, under the storage's ordinary read settings — this design
   does not reach inside it); a retry sleep already entered is woken at once instead;
2. then the synchronous CPU work between that I/O and the next gate: decoding and walking the
   object it returned (a manifest or a fold seal is capped at 256 MiB, `Formats/CasFormat.cpp:127`,
   `:129`), encoding the next artifact, finishing one bounded advisory page (the namespace janitor's
   1000-key page, `Gc/CasGc.cpp:360-366`), or — the longest known — the fold's in-memory sort and
   merge over one generation's deltas (`Gc/CasBlobInDegree.cpp:376-394`, `:606-684`, with the
   delta-wide preparation at `Gc/CasGc.cpp:3228-3242`).

The I/O part is pool-size independent. The CPU part is bounded by the object caps and, for the
reduce, by measurement rather than by a gate; the review's census found no other wait under `Gc/` —
no direct backend call, no thread-pool or future wait that is not subordinate to an admitted request
(`GcReadAhead`'s futures, the meta-writer's queue and drain, the heartbeat's condition variable, the
scheduler joins). The total teardown remains additive over its other, already-bounded phases (the
detached-work drain, the ref-lane drain, the farewell's own retry budget); a single global teardown
deadline is out of scope.
Sub-second teardown, which would need the join removed and the round's objects to outlive the
storage, is explicitly not the goal.

## What is wrong today {#defect}

`ContentAddressedMetadataStorage::shutdown` (`ContentAddressedMetadataStorage.cpp:899-905`) and the
teardown it delegates to (`stopAndDrainForTeardown`, `:908-967`) contain exactly two unbounded waits,
both on the GC round:

| step | waits for | bound |
|---|---|---|
| `std::lock_guard round_lock(gc_scheduler_mutex)` | a synchronous round (`SYSTEM CAS GC`, `GC REBUILD`), which holds the mutex for its whole duration (`:371`, `:617`, `:663`) | none |
| `old_scheduler->stop()` → `thread.join()` (`Gc/CasGcScheduler.cpp:100-112`) | the background round in flight | none |
| `stopAndDrainDetachedWork(deadline_ms)` | detached tasks | `attempt_timeout_ms + lease_safety_margin_ms`, then a warning and on |
| `~Pool` → `drainRefLanesForShutdown`, then the farewell | the ref lanes; the farewell write | the same budget for the drain; the farewell's own retry budget (`Pool/CasServerRoot.cpp`, 10 s) |

`CasGcScheduler::stop` sets `stopping`, wakes the loop and joins. The loop reads `stopping` only at
the top of its `wait_for` (`:298-300`); a round in flight never looks at it. Worse, the loop comment
at `:331-334` records an accepted extra round: if `stop` lands while the loop is blocked on
`gc_round_mutex` behind a manual round, one more full scheduled round runs before the loop observes
the flag. The comment at `:901-902` states the priority in force: "clean GC completion over fast
shutdown".

A round has no time budget at all. `GcRoundWorkBudget` caps the amount of destructive work, not the
wall clock, and against a slow bucket the wall clock is whatever the bucket makes it.

Nothing in this wait protects durable state. The round is one-pass: everything it decides is
committed by a single `gc/state` conditional write at the end, a pass that dies before it leaves
only attempt-scoped debris that is never adopted, and every destructive action before that write is
justified by previously published state alone (`Gc/CasGc.cpp`, the `ONE-PASS round` comment above
the round's `CasOperation`). An interrupted round is a crash the protocol is already built to survive.
The wait exists so that no thread touches a freed object — and the change below keeps that property
intact by keeping the join.

### Why not a check between phases {#why-not-phase-checks}

A phase is long because it makes thousands of requests, not because it makes one long one. A check at
phase boundaries would let a fold run for hours after teardown began. The check belongs at the
request, and it already exists there: `CasOperation` re-checks its admission before each attempt,
before each sleep and once more after a proven commit (`Backend/CasRequests.h:206-215`). The gap is
that the round's own operation (`Gc/CasGc.cpp:430`) and the twenty other `admit` sites under `Gc/`
run on a plane whose fence never trips (`Fence::open`, `Pool/CasPool.cpp:199`).

Two places the request gate does not reach, found in review, are closed separately below: the retry
sleep, which is sampled before and never during (`CasRequests.h:406-417`), and the body of a
streamed object, which the SDK reads at the consumer's pace after the opening attempt returned
(`Backend/CasObjectStorageBackend.cpp:678-682`).

## The mechanism {#mechanism}

### The flag already exists {#flag}

`DetachedRegistryState::stopping` (`Pool/CasDetachedWork.h:26`) already means "this pool is being
torn down": it is read through `DetachedStopToken`, it is already a liveness on the open plane at
`CasPool.cpp:1723`, and the ref ledger's recovery consults it at every point it can park
(`Pool/CasRefLedger.cpp:241`, `:761`, `:823`, `:1432`). This design widens its reach, not its meaning.
No second flag.

One change to it: **`stopping` becomes `std::atomic<bool>`.** `DetachedStopToken::stopping`
(`CasDetachedWork.cpp:9-13`) takes the registry mutex — the mutex `in_flight` and its condition
variable live under. The fence below is consulted before every attempt of every open-plane request,
including the fold's read-ahead workers; a pool-wide mutex on that path is not acceptable. The write
stays under the mutex and the dispatch-side check-and-increment (`CasPool.cpp:1000-1017`) stays under
it too, so the drain's wait predicate (`in_flight == 0`) and the "no task is admitted after the stop"
serialization are unchanged; only the out-of-lock readers move to an acquire load.

No member is reordered. The mount plane's fence already captures `this` and reaches
`mount_runtime`, declared far below it, because its closures run only after construction
(`CasPool.cpp:188-197`); the open plane's fence does the same with `detached_work`.

### Delivery: a teardown fence on the open plane {#delivery}

`CasRequests` is not changed. `Fence` (`Backend/CasFence.h`) is three closures — `generation`,
`admit`, `check_or_throw` — and is the extension point every plane is already built with. The open
plane stops being `Fence::open()` and becomes:

```cpp
, gc_requests(pool_backend, Fence{
      [] { return uint64_t{0}; },
      [this](uint64_t, uint64_t) { return teardownBegun() ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
      [this](uint64_t) { if (teardownBegun()) throwCasTransientUnavailable(...); }},
      config.boot_ms_fn,
      openPlaneSleepFn())
```

Every `admit` and `resume` on the plane sees it; a caller's own `Liveness` (GC's `authority_held`)
stays the second half of `CasOperation::gate`, untouched. `Pool::teardownBegun` is the new public
accessor reading the atomic; the existing `detachedWorkStoppingForTest` keeps its mutex and its
test-only role.

### The retry sleep {#retry-sleep}

A reissue samples the gate and then calls the plane's `sleep_ms` with nothing to wake it
(`CasRequests.h:406-417`); backoff is capped at 5 s (`Backend/CasRetry.cpp:11-17`), which happens to
fit the default budget and is not tied to it. The mount plane already solves this with an
interruptible sleep (`mountPlaneSleepFn`, `CasPool.cpp:197`, `CasMountRuntime::sleepInterruptibly`
at `CasMountRuntime.cpp:448`). The open plane gets `openPlaneSleepFn`: a `wait_for` on
`detached_work->cv` with the predicate `stopping`. `beginTeardown` already notifies that condition
variable, and a predicate `wait_for` does not return early on a spurious wake (a detached task
completing notifies the same variable; harmless).

The test seam must restore it. `CasRequests::setSleepFnForTest({})` falls back to the engine's plain
sleep (`CasRequests.cpp:246-249`), and `Pool::setCasRetrySleepForTest` today re-installs the mount
plane's own function on reset but resets `gc_requests` to the plain one (`CasPool.cpp:1883-1893`).
It re-installs `openPlaneSleepFn` the same way, so clearing the seam cannot leave the open plane
non-interruptible in a later test.

### The stream body {#stream-body}

`Backend::stream` bounds the open only; the returned buffer carries the storage's ordinary read
settings and the consumer reads the body "at its own pace, long after this attempt returned"
(`CasObjectStorageBackend.cpp:678-682`). Its consumers: the fold, GC preview and FSCK, which stream
generation runs through `SourceEdgeRunView` (`Gc/CasBlobInDegree.cpp:279`; `Tools/CasFsck.cpp`), and
S3-staged blob publication, which streams the staged object on the **mount** plane
(`ContentAddressedTransaction.cpp:293`). On a large pool one run is gigabytes. Without this section
the bound would be "one run", which grows with the pool — the case the design exists for.

`CasOperation::stream` wraps the SDK buffer in `AdmittedBodyReadBuffer`, a zero-copy delegating
`ReadBuffer` in `CasRequests.cpp`'s anonymous namespace, modelled on `LimitReadBuffer` (`src/IO`).
Three details are load-bearing:

1. **The constructor adopts the window the open already loaded.** `Backend::stream` forces the
   first GET before returning (`nextIfAtEnd`, `CasObjectStorageBackend.cpp:696`), so the inner buffer
   arrives with pending data, and calling `in->next()` over pending data violates `ReadBuffer::next`'s
   precondition. The wrapper is constructed as `ReadBuffer(in->position(), in->available(), 0)` — the
   three-argument form that keeps the working buffer (`src/IO/ReadBuffer.h:38`) — exactly as
   `LimitReadBuffer` exposes its first window before its first `nextImpl`. The admission check
   therefore first fires when the consumer advances past the first window.
2. **`nextImpl`:** `in->position() = position()` (account the bytes the consumer took), the admission
   check, then `if (!in->next()) { BufferBase::set(in->position(), 0, 0); return false; }` and
   otherwise `BufferBase::set(in->position(), in->available(), 0)`. No byte is copied. A refusal from
   the check cancels the wrapper (`ReadBuffer::next` cancels the buffer whose `nextImpl` threw,
   `src/IO/ReadBuffer.cpp:103-119`); an exception from `in->next()` cancels both layers. Either way the
   buffer the consumer holds is unusable and unwinds with ownership intact.
3. **The refusal keeps the request gate's mapping, and the wrapper holds no reference to the
   operation.** `CasOperation::gate` reads `owner` (a `CasRequests &`) and the operation's own
   members (`CasRequests.cpp:285-297`, `CasRequests.h:267-274`); a callback such as
   `[this] { gate(0); }` would dangle on the staging path. The mapping is factored into a free
   helper — `admitOrThrow(const Fence::Admit, const Liveness &, verb, subject)` — used by the request
   gate and by the wrapper alike, and the wrapper captures **by value** exactly:

   ```cpp
   std::function<Fence::Admit(uint64_t, uint64_t)> admit = owner.fence.admit;   /// a copy of the closure
   const uint64_t admitted_generation;
   const Liveness liveness;                                                     /// a copy, never a move
   const String key;                                                            /// for the exception text
   ```

   and evaluates fence first, caller liveness second — the gate's order. `LostOrRearmed` maps to
   `throwCasTransientUnavailable`, `NoBudget` to `throwCasWriteRetryLater` (`CasRequests.cpp:354-364`).
   On the open plane only the first ever fires; on the mount plane, where S3 staging streams under a
   fence that can answer `NoBudget` (`Pool/CasMountRuntime.cpp:138-151`), the body refusal reads
   exactly like a refused open.

Granularity: one SDK refill, about a megabyte, including that refill's own SDK retries (see the unit
definition in [Decision](#decision)).

The predicate is captured **by value** — the admission callback, the admitted generation, the
`Liveness` — never through a `CasOperation &`, because `CasOperation::stream` returns a buffer that
can outlive the operation object: S3 staging opens its stream under a local mount-plane operation
and returns the buffer to the backend, which consumes it after that operation is gone
(`ContentAddressedTransaction.cpp:286-314`, `CasObjectStorageBackend.cpp:866-915`). The GC and FSCK
streams are opened and consumed on the same thread inside one call (the read-ahead returns whole
`Object`/`Meta` values and never streams, `Gc/CasGcReadAhead.h`). The closures capture `Pool::this`,
not a `PoolPtr`; the copy does not extend the pool's life, the audited owners do — the `Gc` holds a
`PoolPtr` for its lifetime (`Gc/CasGc.h`), FSCK's readers live inside `runFsckImpl`, the staging
callback holds a `PoolPtr` and stays inside its `BlobPublishRequest` while the body is consumed. GC
streams carry no `Liveness` today; one added later must pass the same audit.

Nothing outside `ContentAddressed/` changes: no `src/IO`, no `ReadBufferFromS3`, no
`ObjectStorageBackend::stream`, no fold, no read-ahead; the consumer receives the same
`std::unique_ptr<ReadBuffer>` it receives today.

### Which plane {#which-plane}

Only `gc_requests`. The three planes are already split along exactly the line this needs:

| plane | admits | needed by teardown itself |
|---|---|---|
| `gc_requests` (open) | GC, `Tools/CasFsck.cpp:428`, `system.content_addressed_mounts` (`StorageSystemContentAddressedMounts.cpp:166`), the storage's own probe (`confirmPoolIdentityForEmptyEnumeration`, `:1249`) | no — all of it can and should stop |
| `mount_requests` | the whole ref ledger (`CasRefLedger.h:93-96`), the mount runtime | yes — `drainRefLanesForShutdown` decides whether the farewell is earned |
| `farewell_requests` | the mount runtime only, by reference from its constructor; never reached from outside (`farewellRequests` has no caller) | yes — it is the farewell |

The teardown fence on the open plane therefore cannot reach the two pieces of I/O teardown depends on.

What this buys: nothing under `Gc/` changes. Every `admit` there, the read-ahead workers through
`resume`, the meta-writer's jobs (`Gc/CasGcMetaWriter.cpp:154`, `:164`), FSCK, the probe and the
streamed run bodies all become interruptible through the plane they already pass through. Object
ownership is untouched.

## Sequencing: arm first, then lock, then join {#sequencing}

`Pool::beginTeardown` — new, `noexcept`, idempotent — sets `stopping` under the registry mutex and
notifies its condition variable. `Pool::stopAndDrainDetachedWork(deadline_ms)`
(`CasPool.cpp:1038-1049`) becomes `beginTeardown` followed by the wait it already performs; its
behaviour is unchanged, the flag merely gains the right to be armed earlier.

One arm per wait it bounds, and no more: four waits (`shutdown`'s mutex, `forgetDisk`'s mutex,
`stopAndDrainForTeardown`'s join, `Pool::forgetDisk`'s injected join), four arms. `~Pool` gets none:
by the time it runs the storage has joined the scheduler, and the workers it joins itself are on the
mount plane, which the fence does not reach.

The invariant that makes early arming safe: the flag frees nothing, nulls nothing and swaps nothing.
Every pointer swap stays under `gc_scheduler_mutex` and `pointer_mutex` exactly as today. Arming is a
pure "no further open-plane request will be admitted"; it cannot be armed too early with respect to
any object's lifetime. The arm needs a pool snapshot, taken under `pointer_mutex` and held as a
`shared_ptr` while arming — so "before any lock" means before any lock that can block: the declared
order is lifecycle → scheduler → pointer (`ContentAddressedMetadataStorage.h:660-675`), and taking
`pointer_mutex` alone, releasing it, then taking the outer locks does not invert it.

| entry | today | after |
|---|---|---|
| `shutdown` (`:899`) | `lock(gc_scheduler_mutex)` → `stopAndDrainForTeardown` | **`pool->beginTeardown()`** (pool snapshot under `pointer_mutex`, taken alone) → `lock(gc_scheduler_mutex)` → as before |
| `~ContentAddressedMetadataStorage` (`:314`) | `stopAndDrainForTeardown` | unchanged; covered by the next row |
| `stopAndDrainForTeardown` (`:908`) | swap pointers → `old_scheduler->stop()` → … | swap pointers → **`old_pool->beginTeardown()`** → `old_scheduler->stop()` → … |
| `forgetDisk` (`:985`) | `lock(lifecycle_mutex)` → `lock(gc_scheduler_mutex)` → `pool->forgetDisk(join_gc)` | **`pool->beginTeardown()`** → the same locks → as before |
| `Pool::forgetDisk` (`CasPool.cpp:1069`) | `publishVanishedIntent` → `tripMountLost` → `stop_and_join_gc` → … | **`beginTeardown()`** as step 0, then as before |

What happens to the holders of `gc_scheduler_mutex`: the synchronous round and `GC REBUILD` are
refused at their next request, sleep or body chunk, throw, and their `lock_guard` releases —
`shutdown` proceeds within one such unit. `REBUILD` admits through the same plane
(`Gc/CasGc.cpp:3970`) and needs no separate handling.

The double GC join — `stopAndDrainForTeardown` stops the scheduler, and `Pool::forgetDisk` invokes
the injected `stop_and_join_gc` — is unchanged: `stop` on already-joined threads is a no-op.

The round's exit tail closes by itself. `SCOPE_EXIT({ meta_writer->drainOnExitNoThrow(); })` waits
for the meta jobs, but those admit on the open plane and are refused at their first request, so the
drain is short. The read-ahead's futures (`Gc/CasGcReadAhead.cpp`) are waited on destruction; each
worker's in-flight attempt or chunk finishes and the next is refused.

## Classification: `Stopped` {#classification}

### The honest definition {#classification-definition}

`Stopped` is **a transient failure observed after teardown began**. It is a correlation, not a
cause: the engine deliberately does not distinguish a refused fence from any other refusal ("the
engine does not need to know which of the two refused", `CasRequests.h:95-97`), and this design does
not teach it to. Two consequences are accepted rather than engineered away:

- A genuine transient incident that started before teardown and is caught after it is labelled
  `Stopped`. Its `error_code` and `error` are still recorded; on a server that is exiting the
  distinction is not actionable.
- A stop that lands inside advisory work the round swallows — the namespace janitor page
  (`runNamespaceJanitorPage`, `Gc/CasGc.cpp:352-379`, which the deferred path calls and then returns
  normally, `:605-611`) — produces `Deferred` or `Success`, not `Stopped`. The round did finish; the
  janitor's failure is advisory by design. `Stopped` is therefore "at least the rounds the stop cut
  short", never an exact count of teardowns.

The alternative — a distinct cancellation signal threaded from the engine to the scheduler and
through every advisory catch — is more mechanism than the problem needs, and is out of scope.

### Where {#classification-where}

The `catch (...)` of `CasGcScheduler::runRoundLogged` (`Gc/CasGcScheduler.cpp:266-277`), through
which background rounds and synchronous `SYSTEM CAS GC` rounds (`runOneRoundNow`) pass. The rule is
fail-closed, as today:

```cpp
fin.outcome = !isTransientGcRoundError(fin.error_code) ? Rec::Outcome::Failed
            : store->teardownBegun()                    ? Rec::Outcome::Stopped
                                                        : Rec::Outcome::Aborted;
```

`Stopped` takes only what would otherwise have been `Aborted`. A `LOGICAL_ERROR` or `CORRUPTED_DATA`
raised during teardown stays `Failed`: a bug that coincides with a restart is not masked. This is the
rule already stated at `:34-35` — an unrecognised code must read as a real failure, never as noise.

`GC REBUILD` does **not** pass through `runRoundLogged`: `runGcRebuildNow`
(`ContentAddressedMetadataStorage.cpp:646-680`) builds a one-shot `Gc` and calls `rebuildBaseline`
directly. A rebuild cut by teardown propagates its exception to the query and writes no Finish row —
the minimal behaviour, kept as is.

### The surfaces {#classification-surfaces}

1. **The row in `system.cas_gc_log`.** `Outcome::Stopped` in `GcRoundLogRecord`
   (`CasGcScheduler.h:34`); `STOPPED = 7` in `ContentAddressedGarbageCollectionLogElement`
   (`src/Interpreters/ContentAddressedGarbageCollectionLog.h:21`); the `Enum8` value (`.cpp:21-24`);
   the `case` in the storage's mapping (`ContentAddressedMetadataStorage.cpp:525`). The column
   comments for `outcome`, `error` and `error_code` (`.cpp:41`, `:54-55`) gain the definition above,
   verbatim. The system-log schema changes; there is no persisted data to migrate, and `SystemLog`
   renames a table whose schema differs at startup as it always does.
2. **The scheduler loop — observability hygiene, not part of the bound.** Without it, a round that
   starts after the arm emits a Start row, is refused at its first lease request
   (`Gc/CasGc.cpp:4659-4668`, before anything reaches the backend) and adds a `Stopped` row that says
   nothing; the latency bound holds either way. The secondary requirement "no new scheduled Start
   after teardown begins" is worth two lines: immediately after the loop takes `gc_round_mutex`
   (`CasGcScheduler.cpp:335`) and before it emits a Start row, it checks `store->teardownBegun()` and
   returns. This one check closes both **teardown** windows: the "accepted extra round" queued behind a manual round
   (`:331-334`), and the loop's own next tick between `beginTeardown` and `stop`. The same extra
   round on a plain `SYSTEM CAS GC STOP` — a race with the scheduler's own `stopping`, not with
   teardown — stays as the loop comment describes it; that verb is out of scope. Leaving the loop on a terminal
   condition is the pattern already at `:311-323`; `stop` then joins a finished thread and clears
   `i_am_leader` after the join, as today. The `catch` (`:373-391`) logs at INFO instead of through
   `tryLogCurrentException` when the outcome was `Stopped`.
3. **The heartbeat thread — the same hygiene.** `stop` already wakes and joins it; the one line
   `if (store->teardownBegun()) break;` in its `catch` only spares the ERROR line a refused pulse
   would log in the window.

Other catches that will log a refusal during the window, listed and **not** suppressed — a bounded
burst at WARNING/ERROR within a few seconds of a shutdown, none feeding a counter any test or
scenario asserts on: the meta-writer jobs (`Gc/CasGcMetaWriter.cpp:105-129`; **one record and one
`CASGCMetaWriteAnomaly` increment per refused job**, and several jobs can be queued when the arm
lands), the namespace janitor wrapper (`Gc/CasGc.cpp:352-379`), the leader-authority probe
(`Gc/CasGc.cpp:4619-4637`), and the acquire-time heartbeat (`CasGcScheduler.cpp:132-144`). This
observability cost is accepted. If a scenario ever asserts `CASGCMetaWriteAnomaly = 0` across a
restart, the meta-writer catch is the one to teach; not before.

A synchronous `SYSTEM CAS GC` caught by a shutdown propagates its exception to the query: the operator
sees an error, which is correct — the server is going down.

## Testing {#testing}

The "within one unit" bound is proven **structurally** in gtest — by counting what reaches the
backend — never with a stopwatch; the stopwatch appears only in the integration and soak tests, and
there it asserts independence from round length, not an absolute. No test sleeps to order threads.
Every suite is named `CAS…`; the gate filter is exactly `CAS*`. Gates are opened on every scope
exit (`GateOpenedOnExit`, `gtest_cas_detached_work.cpp:56`) so a failing assertion cannot strand a
parked thread.

| # | proves | where | how |
|---|---|---|---|
| T1 | the open plane's fence refuses after the arm, and a caller's `Liveness` is still honoured | `gtest_cas_requests.cpp`, `CASRequests` | a `CasRequests` built with the teardown fence over an atomic; after the flip the next `read` is refused **before** it is sent — `OrderedFaultBackend` sees no request. Caller liveness false with the fence open is refused too. |
| T2 | a round in flight unwinds at its next unit, not at a phase boundary — for the gated units | new `gtest_cas_gc_teardown_stop.cpp`, `CASGCTeardownStop` | a real `Pool` with the background scheduler. Three cases, one gate each: (a) between two requests (`BetweenRecoveryGetsBackend` shape, `gtest_cas_detached_work.cpp:67`); (b) **inside the body**: a backend whose `stream` returns a buffer that parks in `nextImpl` after the open returned; (c) a read-ahead worker's `resume`d `read`, with the fold **consuming** that future — an unconsumed one has its exception dropped by `~GcReadAhead` (`Gc/CasGcReadAhead.cpp:21-38`) and proves nothing. The test waits for "entered", calls `beginTeardown`, opens the gate. Asserts: no new request reached the backend after the gate opened; exactly one Finish row, `Stopped`; `stop` returned; `isQuiescent`. Heartbeat: the scheduler is configured so no pulse is in flight when the count starts — one already admitted before the arm is permitted by the design. |
| T3 | `shutdown` returns while a synchronous round holds `gc_scheduler_mutex` — **the plan's first failing-first test** | same file | two gates. The round parks at request k; `shutdown` runs in `std::async`; the test **waits until `pool->teardownBegun()` is true** (a state handshake — releasing k at once would race the shutdown thread's arm), then releases k. Today's code proceeds to request k+1 and parks there — `shutdown`'s future stays `timeout`; the fixed code never issues k+1 and the future is `ready`. Gate k+1 is released by RAII after the assertion so a failed run cannot hang teardown. That is the red-then-green shape; a single gate is not, because today's round simply completes once released. |
| T4 | the open plane's sleep is the interruptible one, and stays so after the test seam is cleared | new file, `CASGCTeardownStop` | a wiring test, not a race: arm teardown **first**, then call the plane's `pause(60000)` from a `std::async` and assert the future is `ready` within a short deadline — a predicate `wait_for` whose predicate already holds returns without waiting, so a plane still wired to the plain sleep cannot pass. No sleep orders any thread; the deadline is the assertion. First half with no seam installed; second half after `setCasRetrySleepForTest({})` cleared a previously installed one. "Woken mid-sleep" needs no separate proof: the engine samples the gate and then sleeps, and a predicate `wait_for` is correct in both interleavings. |
| T5 | the fail-closed classification rule | `gtest_cas_gc_log.cpp`, `CASGCLog` | twins of `TransientThrowIsClassifiedAborted` (`:331`): `NETWORK_ERROR` with teardown begun → `Stopped`; `BAD_ARGUMENTS` with teardown begun → **stays `Failed`**. The second matters more than the first. |
| T6 | the hygiene invariant: no scheduled Start after teardown begins | same file as T2 | a manual round holds `gc_round_mutex` behind a gate while the scheduled loop is queued on it; arm; release. Assert the loop emitted no Start row of its own. Secondary; drops with the post-lock check if that is ever removed. |
| T7 | a stop inside the janitor is swallowed as designed | same file as T2 | a deferred round with the stop parked inside the janitor page; assert the Finish row is `Deferred`, not `Stopped`, and no exception escaped — pinning the definition, so a later reader who "fixes" it does so on purpose. |
| T8 | black box, live server: a restart that **provably** cut a long round short | `tests/integration/test_cas_gc_s3`, a new case | in order: (1) `SYSTEM CAS GC STOP` while the pool is still small, so the stop itself is short; (2) populate, with GC paused, a pool large enough that one round takes tens of seconds; (3) restart and measure the **stop phase** — the paused baseline; (4) after restart GC starts by itself: let one full round finish and read its `duration_ms` — the calibration; (5) wait for the next `Start` row, record its `round_id`, and restart **within the first half of the calibrated duration**, measuring the stop phase. Assert `under_gc_stop <= paused_stop + one interval + slack`, **and** that the recorded `round_id`'s Finish row is `Stopped` — the witness that a round was actually cut. A `Success`/`Deferred` there fails the test as "no interruption witnessed" rather than passing it: the inequality alone is an upper bound a restart between rounds satisfies trivially. `system.cas_gc_log` for this disk and window has `Aborted = Error = 0`. The integration S3 mock (`helpers/s3_mocks/broken_s3.py`) can slow PUTs only, and no CAS test uses it; a GET-side hold would be a change to a shared non-CAS helper and is not taken — the calibrated window plus the witness is the CAS-local form. |
| T9 | the premise itself — the protocol survives death at any instant | `utils/ca-soak/scenarios/cards/s46_restart_under_gc.py` | probabilistic by design: phase 3 `--duration`, a large pool, N restarts of `ch1`, each taken after a `Start` row without its `Finish` and within the first half of the last calibrated round duration. Reports how many of the N were witnessed as `Stopped`; asserts every stop time within one interval plus slack of the same paused baseline, `Aborted = Error = 0`, and **fsck clean at the end** — every interrupted round left consistent state. The only test that checks the premise rather than the mechanism. |
| T10 | the wrapper's window handling | `gtest_cas_requests.cpp` | a backend whose `stream` returns a buffer with a preloaded first window and a distinct second: assert the exact byte sequence, EOF, that the admission check first fires on advancing past the first window, and that a refusal leaves the wrapper the consumer holds cancelled. |
| T11 | the mount-plane consumer keeps its mapping | `gtest_cas_requests.cpp` | a stream on a fence that answers `NoBudget` mid-body: the refusal is `throwCasWriteRetryLater`, as for a refused open. |

Not tested, by decision: the SDK's retries inside one refill and the CPU-only reduce — the two
units the gate does not enter (see [Decision](#decision)); T8/T9's slack covers them.

Order: T1–T7, T10, T11 first, failing-first where the defect allows it (T3 by construction; T2's
"zero new requests" in each of its three cases; T6); T8–T9 after the gate is green.

## Verification gate {#gate}

The open plane's fence and the stream wrapper sit under every CAS test. The gate is therefore not
this task's tests but the whole `CAS*` filter of `unit_tests_dbms`, plus the integration tests that
exercise teardown: `test_cas_gc_s3`, `test_cas_gc_sharded`, `test_cas_drop_pool_member`,
`test_cas_shared_pool`. Every test binary run must follow a build that succeeded — a green suite after
a failed build is the wrong binary.

## Consequences to accept {#consequences}

- **After `FORGET` the open plane is refused for good on that pool** — and nothing reaches it.
  `ContentAddressedMetadataStorage::store` goes through `poolAccess`, which throws for a terminal
  lifecycle before handing out the pool (`ContentAddressedMetadataStorage.cpp:1125-1144`);
  `system.content_addressed_mounts` catches that typed refusal and emits its lifecycle snapshot with
  no I/O (`StorageSystemContentAddressedMounts.cpp:128-176`); FSCK and `DROP POOL MEMBER` start with
  the same `store`. Only a query racing the transition can reach an open-plane refusal, and the table
  already catches it. No externally visible change.
- **`Stopped` is a correlation** — see [the honest definition](#classification-definition).
- **The `error` column on a `Stopped` row names the engine's refusal,** not the teardown.
- **Total teardown is additive.** GC's contribution is bounded to one unit; the detached-work drain,
  the ref-lane drain and the farewell keep their own bounds and run in sequence.

## Out of scope {#out-of-scope}

- **Releasing the durable `gc/state` lease on a clean stop** (`BACKLOG/gc.md`,
  `{#gc-lease-not-released-on-clean-stop}`). A peer still waits out the observation window,
  roughly `2 * gc_interval_sec`. Touching this touches the steal protocol and needs its own consult.
- **`SYSTEM CAS GC STOP`** (`ContentAddressedMetadataStorage.cpp:1042`). It is not a teardown: it must
  not refuse an unrelated `system.content_addressed_mounts` query or a running FSCK. It needs a
  round-scoped liveness — one `CasOperation` threaded through every phase — and stays on the backlog
  as `CAS-049`'s remaining half.
- **`SYSTEM CAS FORGET`**, moved here during implementation. Its GC join could be bounded the same
  way, but a self-remount already latched when FORGET starts completes one more step before the loop
  bails, and that step's pool-identity probe is admitted on the open plane. An arm early enough to
  bound the join (the GC join is step 3/4, the remount join step 5a) refuses that probe, so the
  reclaim FORGET's second fence trip exists to override could never happen —
  `CASForget.ForgetReLatchesFenceAfterAReclaimReachesArmMountFence` proves it. No arm placement
  satisfies both, and silently dropping a decommission step is a protocol change this design forbids.
  FORGET therefore keeps today's wait.
- **A single absolute teardown deadline** threaded through the drains and the farewell. The
  additive bound is accepted.
- **A causal cancellation signal** from the engine to the scheduler. The honest `Stopped` is
  accepted instead.
- **Reaching inside one SDK refill** (`ReadBufferFromS3`'s own retries and backoff) and **chunking
  the fold's in-memory sort** to gate CPU-only work. Both are declared units of the bound instead;
  the first would re-implement the SDK's read profile, the second would restructure the reducer for
  a delay that is seconds on the pools measured.
- **A time budget for the round itself.** Not needed once the round is interruptible.
- **Sub-second teardown.** Would require removing the join and letting the round's objects outlive
  the storage; rejected as re-opening the use-after-free class the current teardown closes.

## Review record {#review-record}

rev.3 → rev.4, from the resumed `codex` review (verdict: direction sound, first failing-first test
is T3). Accepted and folded in: the bound is restated as the remainder of one admission interval,
which can hold an in-flight I/O and the CPU work up to the next gate, with the capped-object decode
and the advisory page named; the census "no other wait under `Gc/`" is recorded; the wrapper's
captures are spelled out and the mapping moves to a free helper; the cancellation prose is exact;
the post-lock check, T6 and the heartbeat line are relabelled observability hygiene; T3 gains the
`teardownBegun` handshake; T4's deadline is named as the assertion; T8 requires the cut round's
`Stopped` row as a witness within a calibrated window, and T9 is declared probabilistic. Declined:
gating inside one SDK refill, chunking the sort, a sleep-entry seam (all accepted by the reviewer);
a GET-side hold in the shared S3 mock.

rev.2 → rev.3, from the resumed `codex` review. Accepted and folded in: the unit definition now
names the SDK's in-refill retries and the CPU-only reduce as ungated units and T8/T9 assert against
them; the wrapper's constructor adopts the preloaded first window; the wrapper reuses the request
gate's refusal mapping (the mount-plane staging stream also passes through it); the by-value capture
is justified by the staging stream, not the read-ahead, and the lifetime argument names the owners
that hold the pool; `setCasRetrySleepForTest` restores `openPlaneSleepFn`; T4 became a wiring test
with no timing; T8/T9 got a deterministic paused baseline and an in-flight proof; T2c consumes its
future; T10/T11 added; the post-lock check is scoped to the teardown windows; the meta-writer burst
is described as such. Declined as more mechanism than the problem needs: gating inside one SDK
refill; chunking the sort; a sleep-entry test seam.

rev.1 → rev.2, from the `codex` review (Occam's razor first). Accepted and folded in: the open
plane's `Fence` replaces a new `CasRequests` base liveness and the member reorder (simpler, no engine
change); the stream body is gated (`AdmittedBodyReadBuffer`); the retry sleep is interruptible
(`openPlaneSleepFn`); the post-lock teardown check replaces the return-in-`catch`; the bound is
restated as GC's additional wait, with total teardown additive; `Stopped` is defined honestly and
janitor-swallowed stops are documented; `REBUILD` is noted as bypassing `runRoundLogged`; the
post-`FORGET` consequence is corrected to "none visible"; T3 is reshaped to be red on today's code;
T2 gains the in-body and read-ahead cases; T4, T6, T7 are new. Declined as more mechanism than the
problem needs: a global teardown deadline; a causal cancellation signal; per-catch suppression by
cause across the advisory sites.

## Implementation record {#implementation-record}

Implemented on `cas-gc-teardown-stop` (branched from `cas-gc-rebuild`), six commits, gate `CAS*`
2367/2367. Three things the build changed against rev.4:

1. **FORGET left the scope**, for the reason now recorded under [out of scope](#out-of-scope). The
   plane census in [which plane](#which-plane) was incomplete: besides the ref ledger (mount plane)
   and the farewell (farewell plane), the mount runtime's **self-remount identity probe** is admitted
   on the open plane, and `Pool::forgetDisk` joins that worker. Shutdown and the storage destructor
   have no such step and keep their arms.

2. **A `Stopped` row does not survive the restart that produced it.** It is emitted while `SystemLog`
   is itself tearing down, so `system.cas_gc_log` is not a reliable witness across a restart; the
   server's own log line is (`CA GC round stopped by the disk's teardown`, observed on a live
   restart). The row remains correct and useful for a round cut by the storage destructor while the
   server keeps running.

3. **T8 was dropped and T9 was reshaped.** Measured: a leading GC round lasts a fraction of a second
   (466 ms worst observed) against a paced loop, so a restart meets one about a percent of the time;
   more namespaces do not lengthen a round because `GcRoundWorkBudget` caps its work; and driving
   rounds by hand does not help, because a manual round may not steal the lease and returns
   `NotALeader` in milliseconds (20 325 such rounds in one soak run). A hard witness at that
   frequency is a flaky test, so the integration case was removed and S46 asserts the timing bound,
   the absence of `Aborted`/`Error` rounds and a clean final fsck, recording the witness instead.
   The deterministic proof of the cut is `CASGCTeardownStop`.

What the unit suite pins (`src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`, eight tests): the arm is
idempotent and refuses new detached work; the open plane refuses after it while the mount plane does
not; its retry sleep wakes; a read-ahead worker is refused at its first gate; a background round in
flight is cut at its next request and recorded `Stopped` with no further backend request; `shutdown`
returns while a synchronous round is parked (red before the change); a tick queued behind a manual
round mints no Start row after the arm; and a stop swallowed by the janitor page stays `Deferred`.
