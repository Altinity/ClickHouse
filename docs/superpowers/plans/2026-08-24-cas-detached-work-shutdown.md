---
description: 'Implementation plan for tracked detached CAS dispatch, stop-token-aware recovery, teardown sequencing, weak Context event sinks, and a non-terminating destructor boundary.'
sidebar_label: 'CAS detached work at shutdown plan'
sidebar_position: 7
slug: /superpowers/plans/cas-detached-work-shutdown
title: 'CAS Detached Work At Shutdown Implementation Plan'
doc_type: 'plan'
---

# CAS detached work at shutdown — Implementation Plan {#cas-detached-work-shutdown-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop CAS detached background work from outliving the storage that owns it, stop event emission from dereferencing a released `Context`, and stop the teardown boundary from being able to terminate the process.

**Architecture:** Both detached CAS dispatches go through one tracked entry point on `Pool`, which hands each task a lease (releasing its `Pool` reference *before* decrementing the in-flight count) and a stop token that reaches every point where the work can park — including inside ref-table recovery and its backoff sleep. `ContentAddressedMetadataStorage` unpublishes its pool before bounded-waiting on that count, from both `shutdown` and a new destructor. The metadata storage stops holding a strong `ContextPtr`; its sinks resolve a weak one per event and the two CAS-owned `Context` accessors stop dereferencing a null `shared`. Every phase of `~Pool` is individually guarded, including its own logging.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`, `src/Interpreters/Context.cpp`), gtest via `unit_tests_dbms`, CMake + ninja.

**Spec:** `docs/superpowers/specs/2026-08-24-cas-detached-work-shutdown-design.md` (rev.7)

## Global Constraints {#global-constraints}

- **Branch:** the existing `cas-gc-rebuild`. No new branch, no rebase, no amend — new commits only. Do not push.
- **This checkout is shared with other sessions.** Stage named paths only, never `git add -A`, and `git log -1 --stat` after each commit to confirm what actually landed.
- **Test-suite naming is a gate, not a style rule.** The CAS gate is exactly `--gtest_filter='CAS*'`. Every suite this plan adds — death-test and exit-test variants included — MUST be named `CAS…`. A suite that does not match never runs in the gate; the fix is always renaming the suite, never widening the filter.
- **No durable-format, key-shape or protocol-step changes.**
- **C++ style:** Allman braces. **Comments:** state the reason, never cite plans, specs, backlogs or task numbers.
- **No `sleep` to order threads in tests.** Use a condition variable, a barrier, or a bounded wait whose expiry is itself the assertion.
- **Build output goes to a log file** in the build directory; summarize it with a subagent. No `-j`, no `nproc`.
- New files under `.../ContentAddressed/Pool/` and new `gtest*.cpp` files need no CMake edit: both are globbed with `CONFIGURE_DEPENDS`.

## File Structure {#file-structure}

| File | Responsibility |
|---|---|
| `.../ContentAddressed/Pool/CasDetachedWork.h` (create) | `DetachedRegistryState`, the copyable task lease, and the stop token. One header, because these three are one mechanism. |
| `.../ContentAddressed/Pool/CasDetachedWork.cpp` (create) | Their implementation, including the release ordering. |
| `.../ContentAddressed/Pool/CasPool.h` / `.cpp` (modify) | Owns the registry state; adds `tryDispatchDetached` and `stopAndDrainDetachedWork`; rewires `reportImpossibleInterference`; guards `~Pool`'s three phases. |
| `.../ContentAddressed/Pool/CasRefLedger.h` / `.cpp` (modify) | `pin_owner` replaced by the dispatcher callback; publisher dispatch gains a local rollback guard and unconditional settlement; recovery observes the stop token in four places. |
| `.../ContentAddressed/ContentAddressedMetadataStorage.h` / `.cpp` (modify) | Teardown sequencing, the idempotent teardown helper, a new destructor, and the weak `Context` member and sinks. |
| `src/Interpreters/Context.cpp` (modify) | The two CAS-owned accessors adopt `getZooKeeperLog`'s shape. |
| `src/Disks/tests/gtest_cas_detached_work.cpp` (create) | Tasks 1–3 tests. |
| `src/Disks/tests/gtest_cas_shutdown_context.cpp` (create) | Tasks 4–5 tests, including the two subprocess exit tests. |

---

### Task 1: The registry, the lease and one entry point {#task-1-registry-lease-dispatcher}

**Files:**
- Create: `.../Pool/CasDetachedWork.h`, `.../Pool/CasDetachedWork.cpp`
- Modify: `.../Pool/CasPool.h`, `.../Pool/CasPool.cpp` (`reportImpossibleInterference`)
- Modify: `.../Pool/CasRefLedger.h` (the `pin_owner` member and the constructor parameter), `.../Pool/CasRefLedger.cpp` (`dispatchSnapshotPublisher`)
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp` (create)

**Interfaces:**
- Produces, for every later task:
  - `DB::Cas::DetachedRegistryState` — `std::mutex`, `std::condition_variable`, `uint64_t in_flight`, `bool stopping`.
  - `DB::Cas::DetachedStopToken` with `bool stopping() const`.
  - `DB::Cas::DetachedTaskLease` — **copyable**, completes once.
  - `bool Pool::tryDispatchDetached(std::function<void(DetachedStopToken)> task)`.
  - `bool Pool::stopAndDrainDetachedWork(uint64_t deadline_ms)` — returns false on expiry (Task 3 uses it).
  - `uint64_t Pool::detachedWorkInFlightForTest()`.

**Context an implementer needs.** Two sites detach today and both hold a strong `Pool` reference with nobody waiting on them: `Pool::reportImpossibleInterference`, which captures `shared_from_this`, and `CasRefLedger::dispatchSnapshotPublisher`, which calls the injected `pin_owner` callback (declared `std::function<std::shared_ptr<void>()>`) and also captures raw `this`. The publisher additionally carries a single-flight reservation: `admitSnapshotPublishUnderStateLock` increments `pending_snapshot_publishes` under `state_mutex` *before* the dispatch, and only `settleSnapshotPublish` decrements it — under a single lock hold that also re-admits, so observers never see a transient zero.

Three properties decide whether this task is correct, and each has a test below:

1. **Release order.** The lease drops its `PoolPtr` *before* decrementing. If the strong reference lived in the task's captures it would die with the `std::function`, after the count could already have reached zero — so a drain could observe zero while `~Pool` still ran on the worker.
2. **Copyable, completes once.** `startThreadFromGlobalPool` takes `std::function<void()>`, so everything a task captures must be copy-constructible; a move-only lease does not compile, and a naively copyable one would release per copy. Use a `shared_ptr` control block.
3. **Exactly one owner per decrement.** The caller's rollback guard fires only when no task was launched; a launched task's reservation is retired by settlement alone.

- [ ] **Step 1: Write the failing lease-ordering test**

Create `src/Disks/tests/gtest_cas_detached_work.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <condition_variable>
#include <mutex>

using namespace DB::Cas;

namespace
{

/// A gate a test opens explicitly. No sleeps: the task blocks until released.
struct Gate
{
    void wait()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return open_; });
    }
    void open()
    {
        std::lock_guard lock(m);
        open_ = true;
        cv.notify_all();
    }
    std::mutex m;
    std::condition_variable cv;
    bool open_ = false;
};

}

/// A zero in-flight count must mean no tracked task still holds the pool. A lease that decremented
/// before releasing its reference passes every other test in this file and fails this one.
TEST(CASDetachedWork, ZeroInFlightImpliesPoolReferenceReleased)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    std::weak_ptr<Pool> weak = store;

    auto gate = std::make_shared<Gate>();
    ASSERT_TRUE(store->tryDispatchDetached([gate](DetachedStopToken) { gate->wait(); }));

    gate->open();
    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);

    /// The drain reported zero, so the ONLY strong reference left must be this test's own.
    EXPECT_EQ(weak.use_count(), 1L)
        << "a tracked task still holds the pool although the in-flight count reached zero";
}

/// After stopping, no new detached work may be created.
TEST(CASDetachedWork, DispatchIsRefusedAfterStop)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {}));
}
```

Reconcile `InMemoryBackend`'s include and `PoolConfig`'s field names against an existing CAS gtest (for example `gtest_cas_gc_leak.cpp`) before writing — the sketch follows its idiom but is not copied from it.

- [ ] **Step 2: Run it to verify it fails**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_t1_red.log 2>&1; echo "NINJA_EXIT=$?"
```

Expected: the build **fails** — `tryDispatchDetached` does not exist. That is this step's red; there is nothing to run yet.

- [ ] **Step 3: Add the registry, the token and the lease**

Create `.../Pool/CasDetachedWork.h`:

```cpp
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace DB::Cas
{

class Pool;

/// Accounting for detached CAS work. Held by `shared_ptr` and owned by the `Pool` AND by every task
/// lease, so it outlives the pool it accounts for -- a lease must be able to finish releasing after
/// the pool is gone.
struct DetachedRegistryState
{
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t in_flight = 0;
    bool stopping = false;
};

/// Read-only view of the registry, handed to every task. This is the ONLY way a task asks whether
/// teardown has begun; there is deliberately no accessor that also takes a pin.
class DetachedStopToken
{
public:
    explicit DetachedStopToken(std::shared_ptr<DetachedRegistryState> state_) : state(std::move(state_)) {}
    bool stopping() const;

private:
    std::shared_ptr<DetachedRegistryState> state;
};

/// Copyable, completes ONCE -- on the destruction of the last copy. The task travels through
/// `std::function<void()>` and is copied, so a move-only lease would not compile and a plainly
/// copyable one would release per copy.
///
/// The release order is load-bearing: the pool reference is dropped BEFORE the count is decremented,
/// so a zero count means no tracked task still holds the pool. A task body must therefore NOT capture
/// a pool reference of its own -- such a capture dies with the `std::function`, outside this order.
class DetachedTaskLease
{
public:
    DetachedTaskLease(std::shared_ptr<Pool> owner, std::shared_ptr<DetachedRegistryState> state);

private:
    struct Completion;
    std::shared_ptr<Completion> completion;
};

}
```

`Completion`'s destructor is the whole mechanism: reset the `shared_ptr<Pool>` member, then take the registry mutex, decrement `in_flight`, and `notify_all`. Put it in the `.cpp` so this header does not need `CasPool.h`.

- [ ] **Step 4: Add the two `Pool` methods**

In `CasPool.h`, add a `std::shared_ptr<DetachedRegistryState> detached_work` member and:

```cpp
    /// Launch one tracked detached task. Refuses once teardown has begun.
    ///
    /// The DISPATCH STEP does not fail its caller -- but note that converting the callable into the
    /// by-value parameter allocates in the caller's own expression, before this body is entered, so a
    /// caller that must not propagate an allocation failure has to wrap the construction and the call
    /// together.
    bool tryDispatchDetached(std::function<void(DetachedStopToken)> task);

    /// Latch stopping, wake anything parked on the token, and wait up to `deadline_ms` for the
    /// in-flight count to reach zero. Returns false on expiry. Idempotent.
    bool stopAndDrainDetachedWork(uint64_t deadline_ms);

    uint64_t detachedWorkInFlightForTest() const;
```

`tryDispatchDetached` takes the registry mutex, returns `false` if `stopping`, otherwise increments `in_flight` and builds the lease; then launches `ThreadFromGlobalPool` with a lambda capturing the lease, the token and the task by value. If the launch throws, it releases the lease, logs best-effort inside a nested `try`/`catch(...)`, and returns `false` without rethrowing.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_t1_green.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CASDetachedWork.*' > build_debug/test_t1.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: both `0`.

- [ ] **Step 6: Rewire the diagnostic dispatch**

In `Pool::reportImpossibleInterference`, delete the `shared_from_this` capture — the lease owns the reference now — and dispatch through `tryDispatchDetached`, capturing raw `this`. Check the token before the `GET` and return early if stopping.

Keep the surrounding `try`/`catch`. It is not redundant: this function runs on a fail-closed path, and its existing comment says best-effort diagnostics must never block the caller's own throw. An allocation failure while building the task must not replace the exception the caller is already raising.

- [ ] **Step 7: Write the failing dispatch-failure tests**

Append to `gtest_cas_detached_work.cpp` two tests, using a test seam that forces `tryDispatchDetached` to fail (add a `PoolConfig` hook, mirroring the existing `*_for_test` hooks in that struct):

- the publisher path: with dispatch forced to fail, the mutation that triggered it still succeeds and `Pool::pendingSnapshotPublishesForTest` returns to zero;
- the diagnostic path: the caller's original fail-closed exception propagates, not the dispatch failure.

Name the suite `CASDetachedWork`.

- [ ] **Step 8: Rewire the publisher dispatch**

In `CasRefLedger`, replace the `pin_owner` member and its constructor parameter with a dispatcher callback wired to `Pool::tryDispatchDetached`. In `dispatchSnapshotPublisher`:

- arm a scope-local, **non-copyable** rollback guard whose release performs `decrement pending_snapshot_publishes; notify publish_settle_cv` — the same two operations the current `catch` performs;
- construct the task and call the dispatcher inside one `try`;
- on success, disarm the guard;
- on `false` or a throw, let the guard fire, log best-effort, and swallow.

Inside the task, replace the bare `settleSnapshotPublish` call with a scope guard so it runs on **every** exit, and wrap the error logging in a nested `try`/`catch(...)`. Check the token in `settleSnapshotPublish` before re-dispatching.

- [ ] **Step 9: Write the failing settlement test, then verify green**

A publish that throws with the error logger forced to throw as well: `pendingSnapshotPublishesForTest` must still reach zero. Today the bare settlement call is skipped and the count is stranded for the life of the process.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_debug/unit_tests_dbms --gtest_filter='CASDetachedWork.*' > build_debug/test_t1_full.log 2>&1; echo "TEST_EXIT=$?"
```

- [ ] **Step 10: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
        src/Disks/tests/gtest_cas_detached_work.cpp
git commit -m "fix: track CAS detached work through one dispatcher"
git log -1 --stat
```

---

### Task 2: The stop token reaches into recovery {#task-2-recovery-stop-token}

**Files:**
- Modify: `.../Pool/CasRefLedger.cpp` (`ensureRefTableRecovered`, `recovery_retry_sleep_fn`, `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime`)
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces:**
- Consumes `DetachedStopToken` from Task 1.
- Produces nothing new; the token is threaded through existing signatures.

**Context an implementer needs.** `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` calls `ensureRefTableRecovered` **before** it constructs `runtime_still_admitted`, so a publisher whose runtime needs recovery sits below every checkpoint Task 1 added. Four places inside recovery must observe the token, and two of them are above all the others:

| Place | Why the others do not cover it |
|---|---|
| the wait for a concurrent recovery | `rt.recovery_cv.wait(lock)` is called with **no predicate and no deadline**; a second publisher parks there without reaching any I/O |
| before the walk's **first** backend request | a fresh recovery reads the checkpoint before its first admission check |
| the per-attempt I/O and retry checkpoints | the ordinary case |
| the slice loop of `recovery_retry_sleep_fn` | it already slices at 200 ms and exits early — but only on `!fence_ok_fn()`, and a terminal shutdown does not drop the mount fence. With `recovery_retry_max_backoff_ms` at 30 s against a drain deadline of seconds, this is what makes the drain expire |

Do **not** reuse `recovery_cancel_requested`. `cancelRecoveriesAndAwaitQuiescence` waits without a deadline and then unconditionally clears that flag, and the remount worker is still live during the drain — a cancellation latched by shutdown can be silently cleared.

- [ ] **Step 1: Write the failing tests**

Three cases, all asserting the drain completes inside its deadline and `CASDetachedWorkDrainTimeouts` stayed zero:

- a publisher **sleeping in backoff** (not merely between attempts — that is the case a fence-only predicate misses);
- a publisher parked in `recovery_cv` behind another runtime's in-flight recovery;
- a publisher stalled in the walk's first backend request, before any admission check.

Use the backend seams already in `cas_test_helpers.h` to stall a specific request; read that header before choosing one.

- [ ] **Step 2: Run them to verify they fail**

Expected: each times out and the counter is nonzero.

- [ ] **Step 3: Thread the token through**

Pass the token from the publisher task into `ensureRefTableRecovered` and add the four checks. In the sleep, add the token to the slice loop's predicate — the loop is already built to be interruptible; it simply polls the wrong condition for this case. In the concurrent-recovery wait, convert the bare `wait` into a predicated wait that also wakes on the token.

- [ ] **Step 4: Run them to verify they pass, then commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_t2.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CASDetachedWork.*' > build_debug/test_t2.log 2>&1; echo "TEST_EXIT=$?"
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
        src/Disks/tests/gtest_cas_detached_work.cpp
git commit -m "fix: let CAS teardown interrupt ref-table recovery"
```

---

### Task 3: Teardown sequencing and the destructor {#task-3-teardown-sequencing}

**Files:**
- Modify: `.../ContentAddressedMetadataStorage.h`, `.../ContentAddressedMetadataStorage.cpp` (`shutdown`, plus a new private helper and destructor)
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces:**
- Consumes `Pool::stopAndDrainDetachedWork` from Task 1.
- Produces the `ProfileEvent` `CASDetachedWorkDrainTimeouts`.

**Context an implementer needs.** `poolAccess` copies `cas_store` into a strong `PoolPtr` under `pointer_mutex`, so a drain that runs while the member still holds the pool proves nothing: a caller can take a fresh reference right after the wait succeeds. And `ContentAddressedMetadataStorage` has no destructor at all today, so an instance destroyed without `shutdown` drops its pool with no drain.

- [ ] **Step 1: Write the failing tests**

Three, suite `CASDetachedWork`:

- `shutdown` with tracked detached work in flight must not return while it is in flight — the only formulation that does not race the work's completion;
- a storage destroyed **without** `shutdown`, with work in flight, still drains;
- a storage destroyed **after** `shutdown` does not drain twice and does not wait a second deadline.

- [ ] **Step 2: Run them to verify they fail**

Expected: the first returns immediately; the second never drains; the third has nothing to observe yet.

- [ ] **Step 3: Implement the sequencing**

Extract a private, **idempotent** helper. Under `pointer_mutex`, move `gc_scheduler`, `part_access` and `cas_store` into locals and leave the members empty; then, outside the mutex, stop the scheduler, release the local `part_access`, call `stopAndDrainDetachedWork`, and release the local `PoolPtr`. On expiry, log a warning naming the number of dispatches still in flight and increment `CASDetachedWorkDrainTimeouts`; then proceed.

Call the helper from `shutdown` and from a new destructor. No exception may escape the destructor.

Waiting outside `pointer_mutex` is required, not incidental: it is also what moves a possible `~Pool` out from under that mutex, where it would block snapshot readers for the length of a teardown.

- [ ] **Step 4: Verify green and commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_cas_detached_work.cpp
git commit -m "fix: drain CAS detached work before the pool is released"
```

---

### Task 4: The weak `Context` and null-safe accessors {#task-4-weak-context}

**Files:**
- Modify: `.../ContentAddressedMetadataStorage.h` (the `context` member), `.cpp` (`startup` and both sinks)
- Modify: `src/Interpreters/Context.cpp` (`getContentAddressedLog`, `getContentAddressedGarbageCollectionLog`)
- Test: `src/Disks/tests/gtest_cas_shutdown_context.cpp` (create)

**Interfaces:**
- Produces the `ProfileEvent` `CASEventDroppedContextExpired`.

**Context an implementer needs.** The member is the only long-lived strong `ContextPtr` in `src/Disks`. It is read in four places: two nullness checks in `startup` and the two sink builders. It becomes `std::optional<std::weak_ptr<const Context>>` — `nullopt` means the integration is deliberately off; an engaged-but-expired reference during `startup` is an error, not the disabled path.

`startup` resolves it **once**, at the top, and holds the resulting strong pointer through the single publish step, deriving both decisions from that one local. Two separate resolutions could straddle an expiry and leave a half-configured mount. The sinks capture the **weak** reference, not that local.

Three outcomes must stay distinct, and only the first is counted: an expired weak reference (counted); a `resetSharedContext` on a live `Context`, where the accessor simply returns nothing (not counted); and no system log configured at all, which is ordinary steady state (not counted).

The accessors adopt `Context::getZooKeeperLog`'s shape — `mutex_shared_context` held from the `shared` test through the nested `shared->mutex` and the `shared_ptr` copy. A bare `shared ? … : nullptr` would leave the race it appears to remove.

- [ ] **Step 1: Write the failing tests**

Suite `CASShutdownContext`, plus one exit-test suite:

- **subprocess exit test** — emit after `resetSharedContext` on a still-referenced `Context`. Pre-change this dereferences null and takes the binary down, so it cannot be an in-process test; use `EXPECT_EXIT` with a clean-exit predicate. Assert the emit is skipped safely, **not** a counter;
- an expired weak reference advances `CASEventDroppedContextExpired`;
- holding the metadata storage does not keep the `Context` alive: release every other reference and assert it was destroyed;
- an expired reference supplied at `startup` fails startup with an exception rather than taking the disabled path.

- [ ] **Step 2: Run them, implement, run them again**

Build and run as in earlier tasks, redirecting to `build_debug/test_t4*.log`.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Interpreters/Context.cpp \
        src/Disks/tests/gtest_cas_shutdown_context.cpp
git commit -m "fix: CAS event sinks survive a released Context"
```

`src/Interpreters/Context.cpp` is a shared file. The two functions touched are CAS-owned — added by CAS work and absent from `master` — but say so in the commit message so a later reader does not mistake this for an upstream contract change.

---

### Task 5: A destructor boundary that cannot terminate {#task-5-failsoft-destructor}

**Files:**
- Modify: `.../Pool/CasPool.cpp` (`~Pool`)
- Test: `src/Disks/tests/gtest_cas_shutdown_context.cpp`

**Context an implementer needs.** `~Pool` has three phases and only the keeper's `release` inside `CasMountRuntime::finishTeardown` is guarded today: `stopBackgroundWorkers`; the ref-lane drain from which `drained` is computed; and `finishTeardown(drained)` — which calls `stopBackgroundWorkers` **again**, so guarding only phase 1 leaves that same call unguarded on its second invocation.

`drained` must be initialized to `false` before phase 2 rather than assigned from it, and a throw in phase 2 must still reach phase 3. That default is load-bearing: `finishTeardown(true)` writes the clean-release marker, which a successor reads as proof that no in-flight conditional `PUT` from this incarnation can still land. A guard that swallowed a throw while leaving `drained` at a partial value could forge that proof.

Each guard's own logging goes in a nested `try`/`catch(...)`: `tryLogCurrentException` allocates, and memory pressure is exactly when a teardown phase throws.

- [ ] **Step 1: Write the failing test**

A subprocess exit test — `~Pool` whose phase 2 throws. Pre-change the process terminates. Post-change it exits cleanly **and** no clean-release marker was written; assert both, because a guard that survived the throw but forged the marker would pass an exit-code-only assertion.

- [ ] **Step 2: Implement, verify, commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
        src/Disks/tests/gtest_cas_shutdown_context.cpp
git commit -m "fix: no CAS teardown phase can terminate the process"
```

---

### Task 6: Gate {#task-6-gate}

- [ ] **Step 1: Debug gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_gate10.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CAS*' > build_debug/test_gate10.log 2>&1; echo "TEST_EXIT=$?"
```

Both must be `0`. The summary must state the build marker: a green suite after a failed build is evidence about a different binary.

- [ ] **Step 2: ASan gate**

```bash
ninja -C build_asan unit_tests_dbms > build_asan/build_gate10.log 2>&1; echo "NINJA_EXIT=$?"
./build_asan/unit_tests_dbms --gtest_filter='CAS*' > build_asan/test_gate10.log 2>&1; echo "TEST_EXIT=$?"
```

Search the log for `ERROR: AddressSanitizer` explicitly rather than trusting the exit code.

- [ ] **Step 3: Confirm every new suite is inside the gate**

```bash
grep -ho "TEST(\w*" src/Disks/tests/gtest_cas_detached_work.cpp src/Disks/tests/gtest_cas_shutdown_context.cpp | sort -u
```

Every name must start with `TEST(CAS`. A suite that does not is invisible to the gate, and the fix is renaming the suite.

- [ ] **Step 4: The real teardown order**

Start a server on a CAS-backed disk with the CAS logs enabled, run a workload that produces at least one snapshot publish, and stop it. No crash, no `CASDetachedWorkDrainTimeouts` in the shutdown log. No unit test exercises `Server.cpp`'s actual teardown order, which is where the original defect lives.

- [ ] **Step 5: Confirm the commits**

```bash
git log --oneline -6
git log -5 --stat
```

Five new commits on `cas-gc-rebuild`, each touching only the paths its task named. This checkout is shared; verify rather than assume. Do not push.

## Self-Review {#self-review}

**Spec coverage.** Task 1 implements the registry, lease and dispatcher sections plus the rollback and unconditional settlement; Task 2 the recovery section; Task 3 the sequencing, expiry and both-paths sections; Task 4 the whole `Context` section; Task 5 the destructor boundary; Task 6 the gate. The observability section is satisfied by the counters introduced in Tasks 3 and 4.

**Placeholders.** Four steps send the implementer to read real signatures before writing: the `PoolConfig` test hooks (Task 1 Step 7), the backend stall seams in `cas_test_helpers.h` (Task 2 Step 1), and the `InMemoryBackend`/`PoolConfig` idiom (Task 1 Step 1). Each names the file to read. Inventing those signatures here would read as authoritative and be wrong.

**Type consistency.** `DetachedRegistryState`, `DetachedStopToken`, `DetachedTaskLease`, `tryDispatchDetached`, `stopAndDrainDetachedWork` and `detachedWorkInFlightForTest` are spelled identically in Task 1's header, its tests, and every later task.

**Which steps have a real red.** Task 1 Step 2 is a build failure, not a test failure, and says so. Tasks 2, 3 and 5 have genuine failing-first tests. Task 4's subprocess test is red in the strong sense — pre-change it crashes the process — which is exactly why it is a subprocess test.
