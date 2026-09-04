---
description: 'Design for a content-addressed disk teardown that no longer waits out a GC round: the open request plane carries a teardown liveness, so an in-flight round unwinds within one request attempt, and the round is recorded as Stopped rather than Aborted'
sidebar_label: 'CAS GC teardown stop'
sidebar_position: 10
slug: /superpowers/specs/cas-gc-teardown-stop-design
title: 'CAS GC does not hold up teardown'
doc_type: 'design'
---

# CAS GC does not hold up teardown {#cas-gc-teardown-stop-design}

**Status:** DRAFT for review, rev.1 (2026-09-04). Line references are against `86b60a23de4` on
`cas-gc-rebuild` and will drift; the symbol names will not.

## Decision {#decision}

A content-addressed disk's teardown — server `shutdown`, the storage destructor, `SYSTEM CAS FORGET`,
`~Pool` — arms one flag before it takes any lock or joins any thread. Every request admitted on the
pool's open plane (`Pool::openRequests`) carries that flag as a `Liveness`, so a GC round in flight
is refused at its next request attempt and unwinds within one attempt budget. The joins stay; the
ownership of every object stays; the round becomes short instead of the join becoming optional.

A round that unwinds this way is recorded with a new outcome, `Stopped`, distinct from `Aborted`,
so a clean restart never reads as a backend incident.

The budget this buys: teardown is bounded by `attempt_timeout_ms + lease_safety_margin_ms`
(5 s + 2 s at the defaults, `ContentAddressedSettings.cpp:80-81`) — the same bound the detached-work
drain and the ref-lane drain already apply. Sub-second teardown, which would need the join removed
and the round's objects to outlive the storage, is explicitly not the goal.

## What is wrong today {#defect}

`ContentAddressedMetadataStorage::shutdown` (`ContentAddressedMetadataStorage.cpp:899-905`) and the
teardown it delegates to (`stopAndDrainForTeardown`, `:908-967`) contain exactly two unbounded waits,
both on the GC round:

| step | waits for | bound |
|---|---|---|
| `std::lock_guard round_lock(gc_scheduler_mutex)` | a synchronous round (`SYSTEM CAS GC`, `GC REBUILD`), which holds the mutex for its whole duration (`:371`, `:617`, `:663`) | none |
| `old_scheduler->stop()` → `thread.join()` (`Gc/CasGcScheduler.cpp:100-112`) | the background round in flight | none |
| `stopAndDrainDetachedWork(deadline_ms)` | detached tasks | `attempt_timeout_ms + lease_safety_margin_ms`, then a warning and on |
| `~Pool` → `drainRefLanesForShutdown` | the ref lanes | the same budget; the farewell is written only if the drain was earned |

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
before each sleep and once more after a proven commit (`Backend/CasRequests.h:206-215`), and a
`Liveness` that returns false ends the operation exactly like a lost fence (`:95-97`). The pool
already uses this shape for a detached diagnostic: `gc_requests.admit([token] { return
!token.stopping(); })` (`Pool/CasPool.cpp:1723`). The gap is that the round's own operation
(`Gc/CasGc.cpp:430`) and the twenty other `admit` sites under `Gc/` admit with no liveness at all.

## The mechanism: a teardown liveness on the open plane {#mechanism}

### The flag already exists {#flag}

`DetachedRegistryState::stopping` (`Pool/CasDetachedWork.h:26`) already means "this pool is being
torn down": it is read through `DetachedStopToken`, it is already a liveness on the open plane at
`CasPool.cpp:1723`, and the ref-ledger's recovery consults it at every point it can park
(`Pool/CasRefLedger.cpp:241`, `:761`, `:823`, `:1432`). This design widens its reach, not its meaning.
No second flag.

Two changes to it, both forced:

1. **`stopping` becomes `std::atomic<bool>`.** `DetachedStopToken::stopping` (`CasDetachedWork.cpp:9-13`)
   takes the registry mutex — the mutex `in_flight` and its condition variable live under. The
   liveness below runs before every attempt and every sleep of every open-plane request, including
   the fold's read-ahead workers; a pool-wide mutex on that path is not acceptable. Writes stay under
   the mutex (the condition variable's predicate stays correct); the liveness reads the atomic.
2. **`detached_work` is declared before the request planes.** It sits at `CasPool.h:1154`, the planes
   at `:1150-1152`. A closure in `gc_requests`' initializer would capture a member that does not
   exist yet. Declaration order here is correctness, not style.

### Delivery: `CasRequests` composes a base liveness {#delivery}

`CasRequests` gains a `Liveness base_liveness`, set in the constructor and read afterwards. The
contract "`admit` and `resume` write no member and may run concurrently" (`CasRequests.h:146-148`)
holds. Both methods are one-liners today and become:

```cpp
CasOperation CasRequests::admit(Liveness liveness)
{
    return CasOperation(*this, fence.generation(), composeLiveness(base_liveness, std::move(liveness)));
}
```

`composeLiveness` returns the caller's liveness unchanged when there is no base one (tests, the
offline tools and `clickhouse-disks` build a `CasRequests` without a pool and change nothing), the
base one when the caller passed none, and an `&&` of the two otherwise.

### Which plane {#which-plane}

Only `gc_requests` — the open plane — at its construction (`CasPool.cpp:199`). The three planes are
already split along exactly the line this needs:

| plane | admits | needed by teardown itself |
|---|---|---|
| `gc_requests` (open) | GC, `Tools/CasFsck.cpp:428`, `system.content_addressed_mounts` (`StorageSystemContentAddressedMounts.cpp:166`), the storage's own probe (`confirmPoolIdentityForEmptyEnumeration`, `:1249`) | no — all of it can and should stop |
| `mount_requests` | the whole ref ledger (`CasRefLedger.h:93-96`), the mount runtime | yes — `drainRefLanesForShutdown` decides whether the farewell is earned |
| `farewell_requests` | the mount runtime only, by reference from its constructor; never reached from outside | yes — it is the farewell |

The teardown flag on the open plane therefore cannot reach the two pieces of I/O teardown depends on.
The mount and farewell planes carry no base liveness.

What this buys: nothing under `Gc/` changes. Every `admit` there, the read-ahead workers through
`resume`, the meta-writer's jobs (`Gc/CasGcMetaWriter.cpp:154`, `:164`), FSCK and the probe all
become interruptible through the one place they already pass through. Object ownership is untouched.

## Sequencing: arm first, then lock, then join {#sequencing}

`Pool::beginTeardown` — new, `noexcept`, idempotent — sets `stopping` under the registry mutex and
notifies its condition variable. `Pool::stopAndDrainDetachedWork(deadline_ms)`
(`CasPool.cpp:1038-1049`) becomes `beginTeardown` followed by the wait it already performs; its
behaviour is unchanged, the flag merely gains the right to be armed earlier.

One arm per wait it bounds, and no more: four waits (`shutdown`'s mutex, `forgetDisk`'s mutex,
`stopAndDrainForTeardown`'s join, `Pool::forgetDisk`'s injected join), four arms. `~Pool` gets
none: by the time it runs the storage has joined the scheduler, and the workers it joins itself
are on the mount plane, which the flag does not reach.

The invariant that makes early arming safe: the flag frees nothing, nulls nothing and swaps nothing.
Every pointer swap stays under `gc_scheduler_mutex` and `pointer_mutex` exactly as today. Arming is a
pure "no further open-plane request will be admitted"; it cannot be armed too early with respect to
any object's lifetime.

| entry | today | after |
|---|---|---|
| `shutdown` (`:899`) | `lock(gc_scheduler_mutex)` → `stopAndDrainForTeardown` | **`pool->beginTeardown()`** (pool snapshot under `pointer_mutex`, taken alone) → `lock(gc_scheduler_mutex)` → as before |
| `~ContentAddressedMetadataStorage` (`:314`) | `stopAndDrainForTeardown` | unchanged; covered by the next row |
| `stopAndDrainForTeardown` (`:908`) | swap pointers → `old_scheduler->stop()` → … | swap pointers → **`old_pool->beginTeardown()`** → `old_scheduler->stop()` → … |
| `forgetDisk` (`:985`) | `lock(lifecycle_mutex)` → `lock(gc_scheduler_mutex)` → `pool->forgetDisk(join_gc)` | **`pool->beginTeardown()`** → the same locks → as before |
| `Pool::forgetDisk` (`CasPool.cpp:1069`) | `publishVanishedIntent` → `tripMountLost` → `stop_and_join_gc` → … | **`beginTeardown()`** as step 0, then as before |

Lock order stays clean: `pointer_mutex` is taken on its own before `gc_scheduler_mutex`, never inside
it, so the rule at `:414` ("`pointer_mutex` must never wait behind `gc_scheduler_mutex`") is kept.

What happens to the holders of `gc_scheduler_mutex`: the synchronous round and `GC REBUILD` are
refused at their next request, throw, and their `lock_guard` releases — `shutdown` proceeds within
one attempt. `REBUILD` admits through the same `openRequests().admit()` (`Gc/CasGc.cpp:4539`,
`:4625`) and needs no separate handling.

The double GC join — `stopAndDrainForTeardown` stops the scheduler, and `Pool::forgetDisk` invokes
the injected `stop_and_join_gc` — is unchanged: `stop` on already-joined threads is a no-op.

The round's exit tail closes by itself. `SCOPE_EXIT({ meta_writer->drainOnExitNoThrow(); })` waits
for the meta jobs, but those admit on the open plane and are refused at their first request, so the
drain is short. The heartbeat thread already observes `stopping` through its `wait_for` at
`hb_interval`.

## Classification: `Stopped` {#classification}

### Where {#classification-where}

The `catch (...)` of `CasGcScheduler::runRoundLogged` (`Gc/CasGcScheduler.cpp:266-277`) — the one
point every round passes through: background rounds, synchronous rounds via `runOneRoundNow`, and
`REBUILD`. One edit covers all three.

### By the flag, not by the exception {#classification-by-flag}

The engine deliberately does not distinguish a refused liveness from a lost fence ("the engine does
not need to know which of the two refused", `CasRequests.h:95-97`), and this design does not teach
it to. The scheduler asks the pool: `Pool::teardownBegun`, a new public accessor that reads the
atomic. The existing `detachedWorkStoppingForTest` takes the mutex and is test-only; it stays as it
is.

The rule is fail-closed, as today:

```cpp
fin.outcome = !isTransientGcRoundError(fin.error_code) ? Rec::Outcome::Failed
            : store->teardownBegun()                    ? Rec::Outcome::Stopped
                                                        : Rec::Outcome::Aborted;
```

`Stopped` takes only what would otherwise have been `Aborted`. A `LOGICAL_ERROR` or `CORRUPTED_DATA`
raised during teardown stays `Failed`: a bug that coincides with a restart is not masked. This is the
rule already stated at `:34-35` — an unrecognised code must read as a real failure, never as noise.

### The three places `Aborted` is loud today {#classification-surfaces}

1. **The row in `system.cas_gc_log`.** `Outcome::Stopped` in `GcRoundLogRecord`
   (`CasGcScheduler.h:34`); `STOPPED = 7` in `ContentAddressedGarbageCollectionLogElement`
   (`src/Interpreters/ContentAddressedGarbageCollectionLog.h:21`); the `Enum8` value (`.cpp:21-24`);
   the `case` in the storage's mapping (`ContentAddressedMetadataStorage.cpp:525`). The column
   comments for `outcome`, `error` and `error_code` (`.cpp:41`, `:54-55`) gain: `Stopped` — the round
   was interrupted by the disk's teardown; `error` carries the engine's refusal. The system-log
   schema changes; there is no persisted data to migrate, and `SystemLog` renames a table whose
   schema differs at startup as it always does.
2. **The loop's error line.** The `catch` in `CasGcScheduler::loop` (`:373-391`) logs every failure
   through `tryLogCurrentException("CA GC round failed (will retry next tick)")`. When the pool's
   teardown has begun and the code is transient, it instead logs at INFO — "CA GC round stopped by
   disk teardown after {} ms" — and **returns from the loop**. The return matters: between
   `beginTeardown` in `shutdown` and `stop` in `stopAndDrainForTeardown` there is a window in which
   the loop would otherwise start one more round, refused at its first request — a second `Stopped`
   row that says nothing. Leaving the loop on a terminal condition is the pattern already at
   `:311-323`; `stop` then joins a finished thread. `i_am_leader` is left alone in this branch —
   `stop` clears it after the join, as today.
3. **The heartbeat thread.** `pulseHeartbeat` runs on the open plane and is refused during the
   teardown window; its `catch` logs at ERROR. It gains `if (store->teardownBegun()) break;`.

A synchronous `SYSTEM CAS GC` caught by a shutdown propagates its exception to the query: the operator
sees an error, which is correct — the server is going down.

The text in `error` will be the engine's refusal ("fence lost" / "no budget"), not the word
"stopped". That is deliberate: the classification is in `outcome`, the cause in `error`, and the
engine keeps not knowing which of its two gates refused.

## Testing {#testing}

The "within one attempt" bound is proven **structurally** in gtest — by counting requests at the
backend — never with a stopwatch; the stopwatch appears only in the integration and soak tests, with
slack. No test sleeps to order threads. Every suite is named `CAS…`; the gate filter is exactly
`CAS*`.

| # | proves | where | how |
|---|---|---|---|
| T1 | the base liveness composes with the caller's; with no base, behaviour is unchanged | `gtest_cas_requests.cpp`, `CASRequests` | a `CasRequests` whose base liveness reads an atomic; after the flip the next `read` is refused **before** it is sent — `OrderedFaultBackend` sees no request. Three cases: base only, both, neither. |
| T2 | a round in flight unwinds at its next round-trip, not at a phase boundary | new `gtest_cas_gc_teardown_stop.cpp`, `CASGCTeardownStop` | a real `Pool` with the background scheduler; the backend parks one GET inside the fold behind a `Gate` (`gtest_cas_detached_work.cpp:34`, `BetweenRecoveryGetsBackend` at `:67`). The test waits for "entered", calls `beginTeardown`, opens the gate. Asserts: no new request reached the backend after the gate opened; the Finish row is `Stopped`; `stop` returned; `isQuiescent`. |
| T3 | `shutdown` returns while a synchronous round holds `gc_scheduler_mutex` | same file | a synchronous round parked behind a gate; `shutdown` in `std::async`; `future.wait_for` in the shape of `ShutdownDoesNotReturnWhileWorkIsInFlight` (`:434`) but expecting `ready` once the gate opens. **On today's HEAD this test hangs** until the gate's 60 s `ADD_FAILURE` — it is the failing-first test that names the defect. |
| T4 | the fail-closed classification rule | `gtest_cas_gc_log.cpp`, `CASGCLog` | twins of `TransientThrowIsClassifiedAborted` (`:331`): `NETWORK_ERROR` with teardown begun → `Stopped`; `BAD_ARGUMENTS` with teardown begun → **stays `Failed`**. The second matters more than the first. |
| T5 | one `Stopped` row per teardown; the heartbeat is quiet | inside T2 | between `beginTeardown` and `stop`: exactly one Finish row, zero heartbeat requests at the backend (counted by key prefix). |
| T6 | black box, live server: restart under continuous GC | `tests/integration/test_cas_gc_s3`, a new case | `gc_interval_sec=1` plus continuous inserts, so GC is almost always mid-round. `restart_clickhouse` with the stop wall-clock measured: ≤ `attempt_timeout_ms + lease_safety_margin_ms` plus slack. Afterwards `system.cas_gc_log` has `Stopped ≥ 1` and `Aborted = Error = 0`. |
| T7 | the premise itself — the protocol survives death at any instant | `utils/ca-soak/scenarios/cards/s46_restart_under_gc.py` | phase 3 `--duration`, a large pool, N restarts of `ch1` under GC. Asserts: p100 stop time ≤ the budget; `Stopped = N`; `Aborted = Error = 0`; **fsck clean at the end** — every interrupted round left consistent state. The only test that checks the premise rather than the mechanism. |

Order: T1–T5 first, failing-first where the defect allows it (T3, and T2's "zero new requests"
assertion, fail on today's HEAD); T6–T7 after the gate is green.

## Verification gate {#gate}

The edit in `CasRequests::admit` sits under every CAS test. The gate is therefore not this task's
tests but the whole `CAS*` filter of `unit_tests_dbms`, plus the integration tests that exercise
teardown: `test_cas_gc_s3`, `test_cas_gc_sharded`, `test_cas_drop_pool_member`,
`test_cas_shared_pool`. Every test binary run must follow a build that succeeded — a green suite after
a failed build is the wrong binary.

## Consequences to accept {#consequences}

- **After `FORGET` the open plane is dead for good.** The pool outlives `FORGET` as
  `Vanished(forgotten)` until `DROP` or restart, with the flag armed. The one external caller that
  will still reach the open plane is `system.content_addressed_mounts`; it has a `catch (...)` and a
  `list_ok` flag, and still produces the disk's row. FSCK on a forgotten disk was already a not-live
  path. This is what `FORGET` means — taken out of service — but it is the one place the change is
  visible from outside, so it is named here.
- **The `error` column on a `Stopped` row names the engine's refusal,** not the teardown. See
  [classification](#classification-by-flag).

## Out of scope {#out-of-scope}

- **Releasing the durable `gc/state` lease on a clean stop** (`BACKLOG/gc.md`,
  `{#gc-lease-not-released-on-clean-stop}`). A peer still waits out the observation window,
  roughly `2 * gc_interval_sec`. Touching this touches the steal protocol and needs its own consult.
- **`SYSTEM CAS GC STOP`** (`ContentAddressedMetadataStorage.cpp:1042`). It is not a teardown: it must
  not refuse an unrelated `system.content_addressed_mounts` query or a running FSCK. It needs a
  round-scoped liveness — one `CasOperation` threaded through every phase — and stays on the backlog
  as `CAS-049`'s remaining half.
- **A time budget for the round itself.** Not needed once the round is interruptible.
- **Sub-second teardown.** Would require removing the join and letting the round's objects outlive
  the storage; rejected as re-opening the use-after-free class the current teardown closes.
