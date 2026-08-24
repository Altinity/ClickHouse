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

**Architecture:** Both detached CAS dispatches go through one tracked entry point on `Pool`. Each task gets a lease that releases its `Pool` reference *before* decrementing the in-flight count, and a stop token that reaches every point the work can park — including a concurrent-recovery wait and a backoff sleep. `ContentAddressedMetadataStorage` unpublishes its pool before bounded-waiting on that count, from both `shutdown` and a new destructor. The storage stops holding a strong `ContextPtr`; its sinks resolve a weak one per event and the two CAS-owned `Context` accessors stop dereferencing a null `shared`. Every phase of `~Pool` is guarded, including its own logging.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`, `src/Interpreters/Context.cpp`, `src/Common/ProfileEvents.cpp`), gtest via `unit_tests_dbms`, CMake + ninja.

**Spec:** `docs/superpowers/specs/2026-08-24-cas-detached-work-shutdown-design.md` (rev.7)

## Global Constraints {#global-constraints}

- **Branch:** the existing `cas-gc-rebuild`. No new branch, no rebase, no amend — new commits only. **Do not push.**
- **This checkout is shared with other sessions.** Stage named paths only, never `git add -A`. Every commit step ends with a verification step that reads `git log -1 --stat`.
- **Suite naming is a gate, not a style rule.** The CAS gate is exactly `--gtest_filter='CAS*'`. Every suite added here — death-test and exit-test variants included — MUST be named `CAS…`. A suite that does not match never runs in the gate; the fix is renaming the suite, never widening the filter.
- **A new `ProfileEvent` needs three edits in one commit:** the `M(...)` line in `src/Common/ProfileEvents.cpp`, the `extern const Event` declaration in the using translation unit, and the `increment` call. Omitting the first is a link error, not a warning.
- **No durable-format, key-shape or protocol-step changes.**
- **C++ style:** Allman braces. **Comments:** state the reason; never cite this plan, the spec, a backlog or a task number.
- **No `sleep` to order threads in tests.** Use a condition variable, a barrier, an injected blocking seam, or a bounded wait whose expiry is itself the assertion.
- **Fail fast.** Never run a test binary after a failed build, and never commit after a failed test. Each build, each analysis, each test run and each commit is its own step.
- New files under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/` and new `gtest*.cpp` files under `src/` need no CMake edit: both are globbed with `CONFIGURE_DEPENDS`.

## Conventions {#conventions}

Three step templates, referenced by name from every task. Each occurrence names its own log file so parallel runs never overwrite each other.

**BUILD(`<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/<log> 2>&1
```

**No `; echo "$?"` after it.** Appending an `echo` makes the *command's* exit status that of the
`echo` — always `0` — so a failed build reports success to anything that gates on exit status. Let the
command's own status stand. A nonzero status means the next step is the analysis, never the test run.

**ANALYZE(`<log>`)** — dispatch a subagent with this prompt, and do not read the log yourself:

> Read `/home/mfilimonov/workspace/ClickHouse/master/build_debug/<log>`. Report, in at most ten lines: whether the build or test run succeeded; the first error or first failing test with its file and line; and the total counts (errors, or tests run/passed/failed). Quote no more than three lines of the log.

This applies to **test** logs as well as build logs, not only to build logs.

**TEST(`<filter>`, `<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_debug/unit_tests_dbms --gtest_filter='<filter>' > build_debug/<log> 2>&1
```

Same rule: no trailing `echo`. A nonzero status means the run failed, and the next step is the
analysis — never the commit.

**COMMIT(`<paths>`, `<message>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add <paths>
git commit -m "<message>"
git log -1 --stat
```

Read the `git log -1 --stat` output and confirm it lists exactly `<paths>` and nothing else. Another session commits into this checkout.

## File Structure {#file-structure}

| File | Responsibility |
|---|---|
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h` (create) | `DetachedRegistryState`, `DetachedStopToken`, `DetachedTaskLease` — one header, because they are one mechanism. |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp` (create) | The lease's completion, whose destructor holds the release ordering. |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` / `CasPool.cpp` (modify) | Owns the registry; `tryDispatchDetached`, `stopAndDrainDetachedWork`; rewires `reportImpossibleInterference`; guards `~Pool`'s three phases. |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` / `CasRefLedger.cpp` (modify) | Dispatcher callback replaces `pin_owner`; rollback guard and unconditional settlement; recovery observes the token in four places. |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` / `.cpp` (modify) | Teardown sequencing, idempotent helper, new destructor, weak `Context` member and sinks. |
| `src/Interpreters/Context.cpp` (modify) | The two CAS-owned accessors adopt `getZooKeeperLog`'s shape. |
| `src/Common/ProfileEvents.cpp` (modify) | `M(...)` definitions for the two new counters. |
| `src/Disks/tests/cas_test_helpers.h` (modify) | `OrderedFaultBackend` moved here from a test file, so more than one suite can fault a publish. |
| `src/Disks/tests/gtest_cas_detached_work.cpp` (create) | Tasks 1–3 and 5 tests. |
| `src/Disks/tests/gtest_cas_shutdown_context.cpp` (create) | Tasks 4 and 6 tests, including four subprocess exit tests. |

---

**Task order is a safety property, not a preference.** Task 4 makes `~Pool` unable to terminate the
process, and Task 5 is what makes explicit teardown the usual place `~Pool` runs. Landing them the
other way round would leave one intermediate commit in which a throwing teardown phase is reached more
predictably than before and is still unguarded. They may also be squashed into one commit; they may
not be reordered.

---

### Task 1: The registry, the lease, and one entry point {#task-1-registry-lease-dispatcher}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp` (create)

**Interfaces — Produces:**
- `DB::Cas::DetachedRegistryState`, `DB::Cas::DetachedStopToken` (`bool stopping() const`), `DB::Cas::DetachedTaskLease` (copyable, completes once, `void arm()`).
- `DB::Cas::DetachedDispatchFault` — `None`, `RefuseLaunch`, `ThrowBeforeLaunch`.
- `bool Pool::tryDispatchDetached(std::function<void(DetachedStopToken)> task)`.
- `bool Pool::stopAndDrainDetachedWork(uint64_t deadline_ms)` — `false` on expiry, idempotent.
- `uint64_t Pool::detachedWorkInFlightForTest() const`, `bool Pool::detachedWorkStoppingForTest() const`.
- `PoolConfig::detached_lease_release_hook_for_test`, `PoolConfig::detached_dispatch_fault_for_test`.

**Context.** Two sites detach today, both holding a strong `Pool` reference with nobody waiting: `Pool::reportImpossibleInterference` (captures `shared_from_this`) and `CasRefLedger::dispatchSnapshotPublisher` (calls the injected `pin_owner`, declared `std::function<std::shared_ptr<void>()>`, and captures raw `this`).

Three properties decide correctness, and each has a test:

1. **Release order.** The lease drops its `PoolPtr` *before* decrementing. A strong reference living in the task's captures would die with the `std::function` — after the count could already have reached zero — so a drain could report zero while `~Pool` still ran on the worker.
2. **Copyable, completes once.** `startThreadFromGlobalPool` takes `std::function<void()>`, so everything a task captures must be copy-constructible: a move-only lease will not compile, and a naively copyable one would release per copy.
3. **Exception-safe admission.** All allocation happens *before* the count is incremented. Incrementing and then failing to allocate strands a count nothing can decrement, and every later drain then runs to its full deadline.

**A note on the tests below.** Two of them must observe "the stop is latched" before releasing a worker. Opening a gate from another thread and hoping the drain got there first is not a test — a correct implementation would record `false` and a broken one would pass. `detachedWorkStoppingForTest` exists for exactly this handshake: the drain runs on its own thread, the test polls the latch with `std::this_thread::yield()`, and only then releases the worker. No sleeps anywhere.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_detached_work.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

using namespace DB::Cas;

namespace
{

/// A gate a test opens explicitly, so a task can be held in flight without a sleep.
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

/// Spin until the drain has latched `stopping`. Bounded so a broken implementation fails the test
/// instead of hanging it.
void awaitStopLatched(const PoolPtr & store)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!store->detachedWorkStoppingForTest())
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the drain never latched `stopping`";
        std::this_thread::yield();
    }
}

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend, PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    return Pool::open(backend, config);
}

}

/// A zero in-flight count must mean no tracked task still holds the pool. The hook fires at the exact
/// boundary between releasing the lease's pool reference and decrementing the count, so an
/// implementation that decrements first is caught HERE rather than by a racy post-hoc check.
TEST(CASDetachedWork, LeaseReleasesPoolBeforeDecrementing)
{
    auto backend = std::make_shared<InMemoryBackend>();

    std::weak_ptr<Pool> weak;
    std::atomic<long> use_count_at_boundary{-1};

    PoolConfig config;
    config.detached_lease_release_hook_for_test
        = [&use_count_at_boundary, &weak] { use_count_at_boundary.store(weak.use_count()); };

    auto store = openPlainPool(backend, config);
    weak = store;

    ASSERT_TRUE(store->tryDispatchDetached([](DetachedStopToken) {}));
    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));

    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_EQ(use_count_at_boundary.load(), 1L)
        << "at the release boundary the task still held a pool reference: the lease decremented "
           "before releasing it, so a zero count does not imply the pool is free";
    EXPECT_EQ(weak.use_count(), 1L);
}

/// The drain must not RETURN while a task is still running. Asserted as "the drain is still blocked
/// while the task is held" -- the only formulation that does not race the task's completion.
TEST(CASDetachedWork, DrainDoesNotReturnWhileWorkIsInFlight)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    ASSERT_TRUE(store->tryDispatchDetached([entered, release](DetachedStopToken)
    {
        entered->open();
        release->wait();
    }));
    entered->wait();

    auto drain = std::async(std::launch::async,
                            [&store] { return store->stopAndDrainDetachedWork(/*deadline_ms=*/60000); });

    awaitStopLatched(store);
    EXPECT_EQ(drain.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the drain returned while a tracked task was still in flight";

    release->open();
    EXPECT_TRUE(drain.get());
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
}

/// A task must see the stop that is already latched. The handshake matters: releasing the task before
/// the drain latches would make a CORRECT implementation record `false`.
TEST(CASDetachedWork, TaskObservesStopTokenOnceLatched)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    std::atomic<bool> saw_stop{false};

    ASSERT_TRUE(store->tryDispatchDetached([entered, release, &saw_stop](DetachedStopToken token)
    {
        entered->open();
        release->wait();
        saw_stop.store(token.stopping());
    }));
    entered->wait();

    auto drain = std::async(std::launch::async,
                            [&store] { return store->stopAndDrainDetachedWork(/*deadline_ms=*/60000); });
    awaitStopLatched(store);
    release->open();

    EXPECT_TRUE(drain.get());
    EXPECT_TRUE(saw_stop.load());
}

/// After stopping, no new detached work may be created.
TEST(CASDetachedWork, DispatchIsRefusedAfterStop)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {}));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
}

/// A dispatch that cannot allocate must leave the count untouched, not stranded above zero.
TEST(CASDetachedWork, FailedAdmissionLeavesNoStrandedCount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::ThrowBeforeLaunch;
    auto store = openPlainPool(backend, config);

    EXPECT_ANY_THROW(store->tryDispatchDetached([](DetachedStopToken) {}));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000))
        << "a stranded count makes every later drain run to its full deadline";
}

/// A launch that fails after admission must roll the count back, and must not throw at its caller.
TEST(CASDetachedWork, FailedLaunchRollsBackAndDoesNotThrow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::RefuseLaunch;
    auto store = openPlainPool(backend, config);

    EXPECT_NO_THROW(EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {})));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000));
}
```

- [ ] **Step 2: BUILD(`build_t1_red.log`)**

Expected: the build **fails**. This task's first red is a build failure — none of the six names above exists yet. There is no test run in this step.

- [ ] **Step 3: ANALYZE(`build_t1_red.log`)**

Confirm the errors are "no member named …" / "unknown type name …" for exactly those six names. Any *other* compile error means the test file itself is wrong: fix it before implementing, so the red is for the intended reason.

- [ ] **Step 4: Create `CasDetachedWork.h`**

```cpp
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace DB::Cas
{

class Pool;

/// Accounting for detached CAS work. Held by `shared_ptr` and owned by the pool AND by every task
/// lease, so it outlives the pool it accounts for: a lease must be able to finish releasing after the
/// pool is already gone.
struct DetachedRegistryState
{
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t in_flight = 0;
    bool stopping = false;
};

/// Read-only view of the registry, handed to every task. The ONLY way a task asks whether teardown has
/// begun; there is deliberately no accessor that also takes a pin, because reading a flag must not
/// create the very in-flight work the reader is trying to avoid.
class DetachedStopToken
{
public:
    explicit DetachedStopToken(std::shared_ptr<DetachedRegistryState> state_) : state(std::move(state_)) {}
    bool stopping() const;

private:
    std::shared_ptr<DetachedRegistryState> state;
};

/// Copyable, completes ONCE -- on destruction of the last copy. The task travels through
/// `std::function<void()>` and is copied, so a move-only lease would not compile and a plainly
/// copyable one would release per copy.
///
/// Release order is load-bearing: the pool reference is dropped BEFORE the count is decremented, so a
/// zero count means no tracked task still holds the pool. A task body must therefore NOT capture a
/// pool reference of its own: such a capture dies with the `std::function`, outside this order.
///
/// Construction allocates; arming is separate and happens under the registry mutex, so an allocation
/// failure can never leave a count that nothing will decrement.
class DetachedTaskLease
{
public:
    DetachedTaskLease(std::shared_ptr<Pool> owner, std::shared_ptr<DetachedRegistryState> state,
                      std::function<void()> release_hook);

    /// Arm the completion. Until this is called, destruction releases the owner and touches no count.
    void arm();

private:
    struct Completion;
    std::shared_ptr<Completion> completion;
};

}
```

- [ ] **Step 5: Create `CasDetachedWork.cpp`**

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>

namespace DB::Cas
{

bool DetachedStopToken::stopping() const
{
    std::lock_guard lock(state->mutex);
    return state->stopping;
}

struct DetachedTaskLease::Completion
{
    std::shared_ptr<Pool> owner;
    std::shared_ptr<DetachedRegistryState> state;
    std::function<void()> release_hook;
    bool armed = false;

    ~Completion()
    {
        /// 1. Drop the pool reference FIRST. A waiter that sees the count reach zero below must be
        /// able to conclude that no tracked task holds the pool any more.
        owner.reset();
        if (!armed)
            return;
        if (release_hook)
            release_hook();
        /// 2. Only then account for the completion.
        {
            std::lock_guard lock(state->mutex);
            --state->in_flight;
        }
        state->cv.notify_all();
    }
};

DetachedTaskLease::DetachedTaskLease(std::shared_ptr<Pool> owner, std::shared_ptr<DetachedRegistryState> state,
                                     std::function<void()> release_hook)
    : completion(std::make_shared<Completion>())
{
    completion->owner = std::move(owner);
    completion->state = std::move(state);
    completion->release_hook = std::move(release_hook);
}

void DetachedTaskLease::arm()
{
    completion->armed = true;
}

}
```

- [ ] **Step 6: Wire `Pool`**

In `CasPool.h`, above `PoolConfig`:

```cpp
enum class DetachedDispatchFault
{
    None,
    RefuseLaunch,
    ThrowBeforeLaunch,
};
```

inside `PoolConfig`:

```cpp
    /// TEST SEAM: invoked by a detached task's lease at the exact boundary between releasing its pool
    /// reference and decrementing the in-flight count.
    std::function<void()> detached_lease_release_hook_for_test = {};

    /// TEST SEAM: fault injection for the detached dispatch. `ThrowBeforeLaunch` stands in for an
    /// allocation failure raised before the count is taken; `RefuseLaunch` for a launch that failed
    /// after it was.
    DetachedDispatchFault detached_dispatch_fault_for_test = DetachedDispatchFault::None;
```

and on `Pool`:

```cpp
    bool tryDispatchDetached(std::function<void(DetachedStopToken)> task);
    bool stopAndDrainDetachedWork(uint64_t deadline_ms);
    uint64_t detachedWorkInFlightForTest() const;
    bool detachedWorkStoppingForTest() const;

private:
    std::shared_ptr<DetachedRegistryState> detached_work = std::make_shared<DetachedRegistryState>();
```

In `CasPool.cpp`:

```cpp
bool Pool::tryDispatchDetached(std::function<void(DetachedStopToken)> task)
{
    /// Admission is exception-safe in this order: allocate FIRST, then check-and-count under the
    /// mutex, then arm. Counting before allocating would strand a count that nothing can decrement,
    /// and every later drain would then run to its full deadline.
    DetachedTaskLease lease(shared_from_this(), detached_work, config.detached_lease_release_hook_for_test);
    DetachedStopToken token(detached_work);

    if (config.detached_dispatch_fault_for_test == DetachedDispatchFault::ThrowBeforeLaunch)
        throw Exception(ErrorCodes::ABORTED, "CAS detached dispatch: injected pre-admission failure");

    {
        std::lock_guard lock(detached_work->mutex);
        if (detached_work->stopping)
            return false;
        ++detached_work->in_flight;
    }
    lease.arm();

    try
    {
        if (config.detached_dispatch_fault_for_test == DetachedDispatchFault::RefuseLaunch)
            throw Exception(ErrorCodes::ABORTED, "CAS detached dispatch: injected launch failure");

        ThreadFromGlobalPool([lease, token, task]() mutable { task(token); }).detach();
    }
    catch (...)
    {
        /// The launch failed. `lease` is destroyed as this scope unwinds and performs the release in
        /// order. Log best-effort under a nested guard: `tryLogCurrentException` allocates, and memory
        /// pressure is one of the conditions that brings us here.
        try
        {
            tryLogCurrentException(getLogger("CasPool"), "CAS detached dispatch failed to launch");
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
        return false;
    }
    return true;
}

bool Pool::stopAndDrainDetachedWork(uint64_t deadline_ms)
{
    {
        std::lock_guard lock(detached_work->mutex);
        detached_work->stopping = true;
    }
    detached_work->cv.notify_all();

    std::unique_lock lock(detached_work->mutex);
    return detached_work->cv.wait_for(lock, std::chrono::milliseconds(deadline_ms),
                                      [this] { return detached_work->in_flight == 0; });
}
```

The `lease` capture is by value into the `std::function` — which is why it had to be copyable — and `ThrowBeforeLaunch` is raised *before* the count is taken, so the test asserting an untouched count is meaningful. `ABORTED` is used rather than `LOGICAL_ERROR`: this is an injected environmental failure, and `LOGICAL_ERROR` aborts the process on debug and sanitizer builds.

- [ ] **Step 7: BUILD(`build_t1_green.log`)**
- [ ] **Step 8: ANALYZE(`build_t1_green.log`)**
- [ ] **Step 9: TEST(`CASDetachedWork.*`, `test_t1.log`)**
- [ ] **Step 10: ANALYZE(`test_t1.log`)** — six tests, all passing.
- [ ] **Step 11: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: track CAS detached work through one dispatcher
```

- [ ] **Step 12: Verify the commit** — `git log -1 --stat` lists exactly those five paths.

---
### Task 2: Route both dispatch sites through it {#task-2-rewire-dispatch}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp` (`reportImpossibleInterference`, the ledger's construction)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
- Modify: `src/Disks/tests/cas_test_helpers.h`
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces — Consumes** Task 1's dispatcher. **Produces** two injected `CasRefLedger` constructor callbacks, `dispatch_detached` and `publish_error_hook`, and a shared `OrderedFaultBackend`.

**Context.** The publisher carries a single-flight reservation: `admitSnapshotPublishUnderStateLock` increments `pending_snapshot_publishes` under `state_mutex` *before* the dispatch, and only `settleSnapshotPublish` decrements it — under one lock hold that also re-admits, so observers never see a transient zero. Therefore:

- the rollback must fire **only** when no task was launched. A guard that also fired for a launched task would double-decrement, or erase a reservation a re-admission had just taken;
- settlement inside a launched task must be **unconditional**. It is reached today by a bare call after a handler that calls `tryLogCurrentException`, which allocates and can itself throw; the reservation is then stranded forever, and `quiesceRefTablesForRemount` and `dropNamespace` wait on a count that never reaches zero;
- the diagnostic dispatch has no reservation and needs no rollback guard, but it keeps its caller-side `try`: it runs on a fail-closed path, and an allocation failure while building the task must not replace the exception the caller was already raising.

**Two decisions this task makes rather than leaves open.**

1. **The error hook is a ledger constructor callback, not a `PoolConfig` field.** `CasRefLedger`'s own `config` member is a `RefLedgerConfig`, not a `PoolConfig`, so a `PoolConfig` hook is simply not reachable from the publisher task. It is injected the same way `pin_owner` is today.
2. **The publish must be *made* to fail.** With a plain `InMemoryBackend` the publish succeeds, the `catch` is never entered, and a test of the throwing handler would pass without exercising anything. `OrderedFaultBackend` — today file-local in `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp` (its class and the `openPool` helper are at lines 120–172) — moves **verbatim** into `src/Disks/tests/cas_test_helpers.h`, next to the other fault backends, and that file keeps using it from there. This is a move, not a rewrite: do not adjust its behaviour, and re-run its original suite afterwards.

- [ ] **Step 1: Move `OrderedFaultBackend` into the shared header**

Cut the class from `gtest_cas_ref_snapshot_publish_ordering.cpp` into `src/Disks/tests/cas_test_helpers.h` inside `namespace DB::Cas::tests`, add whatever includes it needs, and leave a `using` or fully-qualified references behind so the original file compiles unchanged in behaviour.

- [ ] **Step 2: BUILD(`build_t2_move.log`)**
- [ ] **Step 3: ANALYZE(`build_t2_move.log`)**
- [ ] **Step 4: TEST(`CASRefSnapshot*`, `test_t2_move.log`)**
- [ ] **Step 5: ANALYZE(`test_t2_move.log`)** — the moved backend's original suite must be unchanged and green before anything is built on top of it.

- [ ] **Step 6: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_detached_work.cpp`. `publishRef` is the same shape as the one at `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp:174-187`; copy it verbatim into the anonymous namespace.

```cpp
namespace
{

/// A pool where any nonempty tail is over-threshold, so every mutation auto-dispatches a publish.
PoolPtr openPublishingPool(const std::shared_ptr<DB::Cas::tests::OrderedFaultBackend> & backend,
                           PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.snapshot_log_count_threshold = 0;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    /// One attempt, so a faulted PUT resolves to a definite non-committed outcome with no internal
    /// retry loop and no wall-clock wait -- the same budget the snapshot-ordering suite uses.
    config.cas_request_budget.max_attempts = 1;
    config.cas_request_budget.attempt_timeout_ms = 100;
    config.cas_request_budget.operation_deadline_ms = 5000;
    config.cas_request_budget.lease_safety_margin_ms = 100;
    return Pool::open(backend, config);
}

}

/// A dispatch that fails must not fail the mutation that triggered it, and must not strand the
/// publisher's single-flight reservation.
TEST(CASDetachedWork, FailedPublisherDispatchKeepsMutationAndClearsReservation)
{
    auto backend = std::make_shared<DB::Cas::tests::OrderedFaultBackend>();
    PoolConfig config;
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::ThrowBeforeLaunch;
    auto store = openPublishingPool(backend, config);
    const RootNamespace ns{"srv1/dispatch_fail"};

    EXPECT_NO_THROW(publishRef(store, ns, "ref_1", 1))
        << "a best-effort maintenance dispatch must never fail an otherwise-successful mutation";
    EXPECT_EQ(store->pendingSnapshotPublishesForTest(ns), 0)
        << "the reservation was stranded: quiescence and dropNamespace would wait on it forever";
}

/// Settlement must survive a throwing error handler. Today it is a bare call after the handler, so a
/// handler that throws skips it and strands the reservation for the life of the process.
///
/// The publish is FAULTED deliberately: with a healthy backend it would succeed, the handler would
/// never run, and this test would pass while exercising nothing.
TEST(CASDetachedWork, SettlementSurvivesAThrowingErrorHandler)
{
    auto backend = std::make_shared<DB::Cas::tests::OrderedFaultBackend>();
    PoolConfig config;
    config.publish_error_hook_for_test
        = [] { throw std::runtime_error("injected: the error handler itself throws"); };
    auto store = openPublishingPool(backend, config);
    const RootNamespace ns{"srv1/handler_throws"};

    /// Arm the fault so the publisher's own PUT fails and its `catch` is entered. Use the same arming
    /// call the snapshot-ordering suite uses against this backend.
    backend->armSnapshotPutFault();

    ASSERT_NO_THROW(publishRef(store, ns, "ref_1", 1));
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(store->pendingSnapshotPublishesForTest(ns), 0);
}
```

`armSnapshotPutFault` stands for whatever the moved backend's own arming API is; read it while moving the class in Step 1 and use its real name here. `publish_error_hook_for_test` is a `PoolConfig` field that `Pool` forwards into the ledger's constructor — a `PoolConfig` field is fine as the *source*; what is impossible is reading it from inside the ledger.

- [ ] **Step 7: BUILD(`build_t2_red.log`)**
- [ ] **Step 8: ANALYZE(`build_t2_red.log`)** — expected: a build failure naming `publish_error_hook_for_test`.

- [ ] **Step 9: Rewire the diagnostic dispatch**

In `Pool::reportImpossibleInterference`, delete the `auto self = shared_from_this();` capture — the lease owns the reference now — and dispatch through `tryDispatchDetached`, capturing raw `this`:

```cpp
    /// The lease owns the pool reference for this task's lifetime; capturing one here as well would
    /// put it outside the lease's release ordering.
    const bool dispatched = tryDispatchDetached([this, key](DetachedStopToken token)
    {
        setThreadName(ThreadName::CAS_ANOMALY_DIAG);
        if (token.stopping())
            return;
        ...   /// the existing body, unchanged
    });
    (void)dispatched;   /// best-effort: a refused diagnostic is not an error for the caller
```

Keep the surrounding `try`/`catch`: this runs on a fail-closed path, and an allocation failure while building the task must not replace the exception the caller is already raising.

- [ ] **Step 10: Rewire the publisher dispatch**

In `CasRefLedger.h`, replace the `std::function<std::shared_ptr<void>()> pin_owner` member and its constructor parameter with two injected callbacks:

```cpp
    /// Launch one tracked detached task through the owning pool. Returns false when the dispatch was
    /// refused or failed -- never throws out of the launch itself.
    std::function<bool(std::function<void(DetachedStopToken)>)> dispatch_detached;

    /// TEST SEAM: invoked inside the publisher task's error handler, before its logging, so a test can
    /// make the handler itself throw. Empty in production.
    std::function<void()> publish_error_hook;
```

`Pool` wires the first to `tryDispatchDetached` and the second from `PoolConfig::publish_error_hook_for_test` where it constructs the ledger.

In `dispatchSnapshotPublisher`:

```cpp
    ProfileEvents::increment(ProfileEvents::CASRefSnapshotPublishDispatched);

    /// The reservation was taken under `state_mutex` before this call, and only a LAUNCHED task's
    /// settlement retires it. This guard therefore covers exactly the paths on which no task runs --
    /// including an allocation failure while constructing the task, which happens in the expression
    /// below, before the dispatcher's own body is entered.
    bool launched = false;
    SCOPE_EXIT({
        if (launched)
            return;
        {
            std::lock_guard lock(rt->state_mutex);
            rt->pending_snapshot_publishes.fetch_sub(1, std::memory_order_relaxed);
        }
        rt->publish_settle_cv.notify_all();
    });

    try
    {
        launched = dispatch_detached([this, ns, rt](DetachedStopToken token)
        {
            setThreadName(ThreadName::CAS_REF_SNAPSHOT_PUBLISH);
            /// Settlement runs on EVERY exit. A handler that throws must not be able to strand the
            /// reservation -- nothing would ever report that, and quiescence would wait forever.
            SCOPE_EXIT({ settleSnapshotPublish(ns, rt, token); });
            try
            {
                tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime(ns, rt, token);
            }
            catch (...)
            {
                if (publish_error_hook)
                    publish_error_hook();
                try
                {
                    tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot publish attempt failed");
                }
                catch (...) // NOLINT(bugprone-empty-catch)
                {
                }
            }
        });
    }
    catch (...)
    {
        try
        {
            tryLogCurrentException(getLogger("CasPool"), "CAS background snapshot-publish dispatch failed");
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }
```

Delete the old hand-written `catch` that decremented and notified: the guard now covers that path and its siblings. In `settleSnapshotPublish`, take the token and skip the re-admission when it reports stopping — the decrement still happens under the same single lock hold, so no observer sees a transient zero.

- [ ] **Step 11: BUILD(`build_t2_green.log`)**
- [ ] **Step 12: ANALYZE(`build_t2_green.log`)**
- [ ] **Step 13: TEST(`CASDetachedWork.*`, `test_t2.log`)**
- [ ] **Step 14: ANALYZE(`test_t2.log`)**
- [ ] **Step 15: TEST(`CASRef*`, `test_t2_ref.log`)**
- [ ] **Step 16: ANALYZE(`test_t2_ref.log`)** — this step changed how *every* snapshot publish is dispatched; the publisher's own suites must stay green.
- [ ] **Step 17: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
         src/Disks/tests/cas_test_helpers.h
         src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: dispatch CAS detached work through the tracked entry point
```

- [ ] **Step 18: Verify the commit**

---

### Task 3: The stop token reaches into recovery {#task-3-recovery-stop-token}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces — Produces** `PoolConfig::recovery_pre_first_request_hook_for_test` and `Pool::setRefRecoveryRetrySleepForTest`.

**Context.** `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` calls `ensureRefTableRecovered` **before** it constructs `runtime_still_admitted`, so a publisher whose runtime needs recovery sits below every checkpoint Task 2 added.

The token must be observed at **every** point recovery can park or start new I/O:

| Place | Why the others do not cover it |
|---|---|
| the wait for a concurrent recovery | `rt.recovery_cv.wait(lock)` is called with **no predicate and no deadline** while `recovery_in_progress`; a second publisher parks there without reaching any I/O |
| before the walk's **first** backend request | a fresh recovery reads the checkpoint before its first admission check |
| `checkRecoveryStillAdmitted` and every I/O boundary inside `runRecoveryWalkOnce` | the walk issues **more** requests after the first check; a stop observed only once still lets the next request start |
| the slice loop of `recovery_retry_sleep_fn` | it slices at 200 ms but exits early only on `!fence_ok_fn()`, and a terminal shutdown does not drop the mount fence; with `recovery_retry_max_backoff_ms` at 30 s against a deadline of seconds, this alone expires the drain |
| the retry loop's fail-closed condition | a token-stopped attempt must be **terminal**, not reclassified as transient by `isTransientRecoveryError` and re-driven |

**Three mechanics that are easy to get wrong.**

1. **A predicate does not wake anyone.** `stopAndDrainDetachedWork` notifies the *registry's* condition variable; a thread parked on `rt.recovery_cv` never sees it. The concurrent-recovery wait therefore becomes a bounded slice wait — `wait_for` in 200 ms slices, re-checking the token — mirroring the sliced backoff that already exists nearby. Do not notify every runtime's `recovery_cv` from the registry: that would make a pool-level registry reach into ledger internals for no gain.
2. **The existing sleep seam is shared and cannot simply be reused.** `Pool::setCasRetrySleepForTest` takes `std::function<void(uint64_t)>` and feeds *both* the request controller and recovery, with a dozen call sites. Recovery gets its **own**, token-aware seam — `setRefRecoveryRetrySleepForTest(std::function<void(uint64_t, const std::optional<DetachedStopToken> &)>)`, forwarded from `Pool` to the ledger — and the existing single-parameter setter keeps working unchanged for everyone else.
3. **Existing callers must be unaffected.** `ensureRefTableRecovered` has many non-publisher callers, so the parameter is `std::optional<DetachedStopToken>` defaulting to `std::nullopt`, meaning "no detached-work stop applies". Thread it onward through `runRecoveryWalkOnce` and `checkRecoveryStillAdmitted`, which already takes a `bool & cancelled` and is the natural place for it.

- [ ] **Step 1: Add the seams only, with no behaviour change**

Add `PoolConfig::recovery_pre_first_request_hook_for_test` (`std::function<void()>`, invoked immediately before the walk's first backend request), the new `setRefRecoveryRetrySleepForTest` on both `CasRefLedger` and `Pool`, and the token parameters — all defaulted, so nothing observable changes yet.

- [ ] **Step 2: BUILD(`build_t3_seam.log`)**
- [ ] **Step 3: ANALYZE(`build_t3_seam.log`)** — this must be **green**. The behaviour tests come next, and a red build here would make their red unreadable.

- [ ] **Step 4: Write the failing tests**

```cpp
/// A publisher asleep in recovery backoff must be woken by the stop, not waited out. The injected
/// sleep stands in for a long backoff without spending wall-clock time.
TEST(CASDetachedWork, StopWakesRecoveryBackoffSleep)
{
    auto backend = std::make_shared<DB::Cas::tests::OrderedFaultBackend>();
    auto store = openPublishingPool(backend);
    const RootNamespace ns{"srv1/backoff"};

    auto sleeping = std::make_shared<Gate>();
    store->setRefRecoveryRetrySleepForTest(
        [sleeping](uint64_t, const std::optional<DetachedStopToken> & token)
        {
            sleeping->open();
            while (!(token && token->stopping()))
                std::this_thread::yield();
        });

    ...   /// drive a publish whose recovery fails transiently, so the loop enters its backoff
    sleeping->wait();

    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/5000));
}

/// A publisher parked behind another runtime's in-flight recovery is below every I/O checkpoint.
TEST(CASDetachedWork, StopWakesAConcurrentRecoveryWaiter)
{
    auto backend = std::make_shared<DB::Cas::tests::OrderedFaultBackend>();
    auto store = openPublishingPool(backend);
    const RootNamespace ns{"srv1/concurrent_recovery"};

    ...   /// hold one recovery in flight against `ns`
    /// Wait until a SECOND caller has provably reached the wait, rather than assuming it has.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (store->refRecoveryWaitersForTest(ns) == 0)
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "no second caller reached the wait";
        std::this_thread::yield();
    }

    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/5000));
}

/// The token is checked BEFORE the walk's first backend request, so a stop latched while the
/// publisher is at that boundary means the request is never issued.
///
/// Note what this does NOT claim: a request already in flight is not interrupted. That case is a
/// bounded drain timeout by design, so the publisher is parked at the pre-request hook -- not inside a
/// stalled GET -- and the drain is latched before the hook is released.
TEST(CASDetachedWork, StopIsObservedBeforeTheFirstRecoveryRequest)
{
    auto backend = std::make_shared<CountingBackend>();
    auto at_boundary = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();

    PoolConfig config;
    config.recovery_pre_first_request_hook_for_test = [at_boundary, release]
    {
        at_boundary->open();
        release->wait();
    };
    ...   /// open a publishing pool over `backend` with this config and drive a publish that recovers

    at_boundary->wait();
    const uint64_t gets_before = backend->getTotal();

    auto drain = std::async(std::launch::async,
                            [&store] { return store->stopAndDrainDetachedWork(/*deadline_ms=*/5000); });
    awaitStopLatched(store);
    release->open();

    EXPECT_TRUE(drain.get());
    EXPECT_EQ(backend->getTotal(), gets_before)
        << "the walk issued its first request after the stop was already latched";
}
```

The three `...` markers are the same fixture in three shapes: a runtime driven into `NeedsRecovery`. Take the fault arming from the moved `OrderedFaultBackend` (Task 2 Step 1) and the recovery-driving setup from the nearest test in `src/Disks/tests/gtest_cas_ref_writer.cpp` that reaches `RefLaneState::NeedsRecovery`. `CountingBackend` and `getTotal` are at `src/Disks/tests/cas_test_helpers.h:1440` and `:1611`; `refRecoveryWaitersForTest` is `Pool`'s, at `CasPool.h:997`.

- [ ] **Step 5: BUILD(`build_t3_red.log`)**
- [ ] **Step 6: ANALYZE(`build_t3_red.log`)** — must be green: the seams exist from Step 1.
- [ ] **Step 7: TEST(`CASDetachedWork.Stop*`, `test_t3_red.log`)**
- [ ] **Step 8: ANALYZE(`test_t3_red.log`)** — expected: all three fail because the drain returns `false`, its deadline expired. A failure of any other shape means the fixture never reached recovery; fix the fixture before implementing.

- [ ] **Step 9: Thread the token through**

- `ensureRefTableRecovered(ns, rt, token = std::nullopt)`; the publisher passes its token, every other caller keeps its current behaviour.
- The concurrent-recovery wait becomes a bounded slice wait, because nothing notifies `recovery_cv` when the token flips:
  ```cpp
  while (rt.recovery_in_progress)
  {
      if (token && token->stopping())
          throw Exception(ErrorCodes::ABORTED,
              "CAS ref recovery: teardown began while waiting for a concurrent recovery");
      ++rt.recovery_waiters_for_test;
      rt.recovery_cv.wait_for(lock, std::chrono::milliseconds(200));
      --rt.recovery_waiters_for_test;
  }
  ```
- Immediately before the walk's first backend request: invoke `recovery_pre_first_request_hook_for_test` if set, then check the token and throw `ABORTED` if stopping.
- Pass the token into `runRecoveryWalkOnce` and `checkRecoveryStillAdmitted`, and check it at **every** I/O boundary inside the walk, not only the first: the walk issues further requests after the first check.
- The default `recovery_retry_sleep_fn` gains the token in its slice condition:
  `while (slept < total_ms && fence_ok_fn() && !(token && token->stopping()))`.
- In the retry loop's fail-closed condition, put the token beside `cancelled` so a token-stopped attempt is terminal and is never reclassified as transient.

- [ ] **Step 10: BUILD(`build_t3_green.log`)**
- [ ] **Step 11: ANALYZE(`build_t3_green.log`)**
- [ ] **Step 12: TEST(`CASDetachedWork.*`, `test_t3.log`)**
- [ ] **Step 13: ANALYZE(`test_t3.log`)**
- [ ] **Step 14: TEST(`CASRef*`, `test_t3_ref.log`)**
- [ ] **Step 15: ANALYZE(`test_t3_ref.log`)**
- [ ] **Step 16: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: let CAS teardown interrupt ref-table recovery
```

- [ ] **Step 17: Verify the commit**

---
### Task 4: A destructor boundary that cannot terminate {#task-4-failsoft-destructor}

**This task lands BEFORE Task 5, deliberately.** Task 5 makes explicit teardown the usual place `~Pool`
runs. Landing it first would leave an intermediate commit in which a throwing teardown phase is reached
more predictably than before and is still unguarded — moving a crash rather than fixing one. The two
may be squashed; they may not be swapped.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Test: `src/Disks/tests/gtest_cas_shutdown_context.cpp` (create)

**Interfaces — Produces** `PoolConfig::teardown_phase1_throw_for_test`, `…phase2…`, `…phase3…`.

**Context.** `~Pool` has three phases, and only the keeper's `release` inside
`CasMountRuntime::finishTeardown` is guarded today:

1. `mount_runtime.stopBackgroundWorkers()`
2. the ref-lane drain, from which `drained` is computed together with `writerCleanupDutiesPending`
3. `mount_runtime.finishTeardown(drained)` — which calls `stopBackgroundWorkers` **again**, the
   belt-and-suspenders re-join, so guarding only phase 1 leaves that same call unguarded on its second
   invocation

`drained` must be **initialized to `false`** before phase 2 rather than assigned from it, and a throw in
phase 2 must still reach phase 3. That default is load-bearing: `finishTeardown(true)` writes the
clean-release marker, which a successor reads as proof that no in-flight conditional `PUT` from this
incarnation can still land. A guard that swallowed a throw while leaving `drained` at a partial value
could forge that proof.

Each guard's own logging goes in a nested `try`/`catch(...)`: `tryLogCurrentException` allocates, and
memory pressure is exactly when a teardown phase throws.

- [ ] **Step 1: Add the three hooks** to `PoolConfig` — `teardown_phase1_throw_for_test`,
      `teardown_phase2_throw_for_test`, `teardown_phase3_throw_for_test`, each `std::function<void()>`,
      invoked at the top of its phase in `~Pool`.

- [ ] **Step 2: Write the failing tests**

Create `src/Disks/tests/gtest_cas_shutdown_context.cpp`. These must run in a subprocess: pre-change a
throwing phase terminates the process, and an in-process assertion would take the whole binary down and
hide every test behind it. The post-change expectation is a **clean exit**, which `EXPECT_DEATH` cannot
express — it expects abnormal termination — so this is an `EXPECT_EXIT` test.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <cstdlib>

using namespace DB::Cas;

namespace
{

/// Open a pool, arm one teardown phase to throw, destroy it, and report whether the clean-release
/// marker was written. Runs inside the subprocess of each exit test below.
[[noreturn]] void tearDownWithThrowingPhase(int phase)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.pool_prefix = "p";
    config.server_root_id = "test";
    auto thrower = [] { throw std::runtime_error("injected teardown phase failure"); };
    if (phase == 1)
        config.teardown_phase1_throw_for_test = thrower;
    else if (phase == 2)
        config.teardown_phase2_throw_for_test = thrower;
    else
        config.teardown_phase3_throw_for_test = thrower;

    {
        auto store = Pool::open(backend, config);
        (void)store;
    }   /// ~Pool runs here

    /// Report what the teardown left behind, so the parent asserts on the MARKER and not merely on the
    /// exit code: a guard that survived the throw but forged the marker would pass an exit-code-only
    /// assertion, and forging it is the worse of the two failures.
    std::exit(...);   /// see below
}

}
```

For the marker check, read the mount object from `backend` after destruction and decide from its
contents whether a clean release was recorded; the accessor and the decoded field are the ones
`src/Disks/tests/gtest_cas_mount.cpp` already uses when it asserts a clean versus unclean end. Exit `0`
only when the process survived **and** the marker is absent; exit `1` otherwise, so the parent's
`ExitedWithCode(0)` carries both facts.

```cpp
TEST(CASShutdownExitTest, TeardownPhase1ThrowExitsCleanly)
{
    EXPECT_EXIT(tearDownWithThrowingPhase(1), ::testing::ExitedWithCode(0), "");
}

TEST(CASShutdownExitTest, TeardownPhase2ThrowExitsCleanlyAndSkipsTheMarker)
{
    EXPECT_EXIT(tearDownWithThrowingPhase(2), ::testing::ExitedWithCode(0), "");
}

TEST(CASShutdownExitTest, TeardownPhase3ThrowExitsCleanly)
{
    EXPECT_EXIT(tearDownWithThrowingPhase(3), ::testing::ExitedWithCode(0), "");
}
```

- [ ] **Step 3: BUILD(`build_t4_red.log`)**
- [ ] **Step 4: ANALYZE(`build_t4_red.log`)**
- [ ] **Step 5: TEST(`CASShutdownExitTest.*`, `test_t4_red.log`)**
- [ ] **Step 6: ANALYZE(`test_t4_red.log`)** — expected: the subprocesses terminate abnormally rather than exiting `0`.

- [ ] **Step 7: Guard all three phases**

```cpp
Pool::~Pool()
{
    /// Nothing here may escape: a destructor is `noexcept` by default, so ANY throw is
    /// `std::terminate` -- on the path explicit teardown is about to make routine. Each phase is
    /// guarded separately, because `finishTeardown` re-enters `stopBackgroundWorkers` and a single
    /// guard around the first call would leave the second one bare.
    const auto guarded = [](auto && phase, const char * what)
    {
        try
        {
            phase();
        }
        catch (...)
        {
            /// `tryLogCurrentException` allocates; under memory pressure -- one of the conditions that
            /// brought us here -- it can throw on the way to reporting the throw.
            try
            {
                tryLogCurrentException(getLogger("CasPool"), what);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
    };

    guarded([this] { ... phase 1 ... }, "CAS pool teardown: stopping background workers");

    /// FALSE by default, not assigned from the drain: `finishTeardown(true)` writes the clean-release
    /// marker, which a successor reads as proof that no in-flight conditional PUT from this
    /// incarnation can still land. A swallowed drain failure must never be able to forge that proof.
    bool drained = false;
    guarded([this, &drained] { ... phase 2, assigning `drained` only on success ... },
            "CAS pool teardown: draining ref lanes");

    guarded([this, drained] { mount_runtime.finishTeardown(drained); },
            "CAS pool teardown: finishing mount teardown");
}
```

- [ ] **Step 8: BUILD(`build_t4_green.log`)**
- [ ] **Step 9: ANALYZE(`build_t4_green.log`)**
- [ ] **Step 10: TEST(`CASShutdownExitTest.*`, `test_t4.log`)**
- [ ] **Step 11: ANALYZE(`test_t4.log`)**
- [ ] **Step 12: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/tests/gtest_cas_shutdown_context.cpp
message: fix: no CAS teardown phase can terminate the process
```

- [ ] **Step 13: Verify the commit**

---

### Task 5: Teardown sequencing and the destructor {#task-5-teardown-sequencing}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Common/ProfileEvents.cpp`
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces — Produces** the `ProfileEvent` `CASDetachedWorkDrainTimeouts` and
`Cas::PoolPtr ContentAddressedMetadataStorage::poolForTest() const`.

**Context.** `poolAccess` copies `cas_store` into a strong `PoolPtr` under `pointer_mutex`, so a drain
that runs while the member still holds the pool proves nothing: a caller can take a fresh reference
right after the wait succeeds. And `ContentAddressedMetadataStorage` has no destructor today, so an
instance destroyed without `shutdown` drops its pool with no drain at all.

The production deadline is
`cas_request_budget.attempt_timeout_ms + cas_request_budget.lease_safety_margin_ms` — the one
`CasRefLedger::drainRefLanesForShutdown` already uses.

The tests build a storage the way `openGateStorage` does at
`src/Disks/tests/gtest_cas_operation_gate.cpp:49-58` — `Cas::tests::makeSettingsForTest`,
`Cas::tests::makeLocalObjectStorageForTest`, then
`ContentAddressedMetadataStorage(object_storage, "pool", "srv1", "", nullptr, settings)` and
`startup()`. They reach its pool through the new `poolForTest` accessor, which belongs beside the
several `*ForTest` members the class already carries.

- [ ] **Step 1: Add the counter** — the `M(...)` line in `src/Common/ProfileEvents.cpp`:

```cpp
    M(CASDetachedWorkDrainTimeouts, "Counts CAS storage teardowns whose bounded wait for detached background work expired with work still in flight. The teardown proceeds, but for that teardown it could not be established that no tracked task still holds the pool. Expected to stay at zero.", ValueType::Number) \
```

plus `extern const Event CASDetachedWorkDrainTimeouts;` in `ContentAddressedMetadataStorage.cpp`.

- [ ] **Step 2: Write the failing tests**

```cpp
/// `shutdown` must not RETURN while tracked detached work is in flight.
TEST(CASDetachedWork, ShutdownDoesNotReturnWhileWorkIsInFlight)
{
    auto storage = openTestStorage();          /// the `openGateStorage` shape, see above
    auto pool = storage->poolForTest();

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    ASSERT_TRUE(pool->tryDispatchDetached([entered, release](DetachedStopToken)
    {
        entered->open();
        release->wait();
    }));
    entered->wait();

    auto done = std::async(std::launch::async, [&storage] { storage->shutdown(); });
    awaitStopLatched(pool);
    EXPECT_EQ(done.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "shutdown returned while tracked detached work was still in flight";

    release->open();
    done.get();
}

/// A storage destroyed WITHOUT `shutdown` must still drain: nothing prevents that path today.
TEST(CASDetachedWork, ImplicitDestructionDrains)
{
    auto storage = openTestStorage();
    auto pool = storage->poolForTest();

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    std::atomic<bool> finished{false};
    ASSERT_TRUE(pool->tryDispatchDetached([entered, release, &finished](DetachedStopToken)
    {
        entered->open();
        release->wait();
        finished.store(true);
    }));
    entered->wait();

    auto destroyed = std::async(std::launch::async, [&storage] { storage.reset(); });
    awaitStopLatched(pool);
    release->open();
    destroyed.get();

    EXPECT_TRUE(finished.load());
    EXPECT_EQ(pool->detachedWorkInFlightForTest(), 0u);
}

/// A storage destroyed AFTER `shutdown` must find nothing to do rather than waiting a second deadline.
TEST(CASDetachedWork, TeardownHelperIsIdempotent)
{
    auto storage = openTestStorage();
    storage->shutdown();

    const auto started = std::chrono::steady_clock::now();
    storage.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1))
        << "the second teardown repeated the wait instead of finding nothing to do";
}

/// The timeout path must be OBSERVABLE. Without this the increment could be missing entirely and every
/// other test here would stay green, because they all exercise successful drains.
TEST(CASDetachedWork, ExpiredDrainIncrementsTheTimeoutCounter)
{
    auto storage = openTestStorage(/*tiny_budget=*/true);
    auto pool = storage->poolForTest();

    /// A task that deliberately IGNORES its token, standing in for work that cannot be interrupted.
    auto release = std::make_shared<Gate>();
    auto entered = std::make_shared<Gate>();
    ASSERT_TRUE(pool->tryDispatchDetached([entered, release](DetachedStopToken)
    {
        entered->open();
        release->wait();
    }));
    entered->wait();

    const auto before = ProfileEvents::global_counters[ProfileEvents::CASDetachedWorkDrainTimeouts]
                            .load(std::memory_order_relaxed);
    storage->shutdown();                       /// expires: the task is still held
    const auto after = ProfileEvents::global_counters[ProfileEvents::CASDetachedWorkDrainTimeouts]
                           .load(std::memory_order_relaxed);
    EXPECT_EQ(after - before, 1u);

    release->open();
}
```

`openTestStorage(bool tiny_budget = false)` is a local helper: the `openGateStorage` body above, with
the request-budget settings set small when `tiny_budget` is true so the drain expires in well under a
second. Read the budget field names in `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h`
and the settings they are populated from in `Cas::tests::makeSettingsForTest`
(`src/Disks/tests/cas_test_helpers.h:134`).

Confirm the `ProfileEvents::global_counters` access idiom against an existing CAS test that asserts a
counter delta before copying it.

- [ ] **Step 3: BUILD(`build_t5_red.log`)**
- [ ] **Step 4: ANALYZE(`build_t5_red.log`)**
- [ ] **Step 5: TEST(`CASDetachedWork.*`, `test_t5_red.log`)**
- [ ] **Step 6: ANALYZE(`test_t5_red.log`)**

- [ ] **Step 7: Implement the sequencing**

```cpp
void ContentAddressedMetadataStorage::stopAndDrainForTeardown() noexcept
{
    std::shared_ptr<Cas::CasGcScheduler> old_scheduler;
    PartAccessPtr old_part_access;
    Cas::PoolPtr old_pool;
    {
        std::lock_guard ptr_lock(pointer_mutex);
        old_scheduler = std::move(gc_scheduler);
        gc_scheduler.reset();
        old_part_access = std::move(part_access);
        part_access.reset();
        old_pool = std::move(cas_store);
        cas_store.reset();
    }
    /// Everything below runs OUTSIDE `pointer_mutex`. Required, not incidental: waiting under it would
    /// block snapshot readers for the whole wait, and it is also what keeps a `~Pool` triggered by the
    /// last local reference from running under that mutex.
    ///
    /// Each phase is guarded SEPARATELY. One `try` around all of them would let a throw from stopping
    /// the scheduler skip the part-access release and the drain itself -- the two steps this function
    /// exists for.
    const auto guarded = [this](auto && phase, const char * what)
    {
        try
        {
            phase();
        }
        catch (...)
        {
            try
            {
                tryLogCurrentException(log, what);
            }
            catch (...) // NOLINT(bugprone-empty-catch)
            {
            }
        }
    };

    guarded([&] { if (old_scheduler) old_scheduler->stop(); }, "CAS storage teardown: stopping GC");
    guarded([&] { old_part_access.reset(); }, "CAS storage teardown: releasing part access");
    guarded([&]
    {
        if (!old_pool)
            return;
        const auto & budget = old_pool->poolConfig().cas_request_budget;
        const uint64_t deadline_ms = budget.attempt_timeout_ms + budget.lease_safety_margin_ms;
        if (old_pool->stopAndDrainDetachedWork(deadline_ms))
            return;
        ProfileEvents::increment(ProfileEvents::CASDetachedWorkDrainTimeouts);
        LOG_WARNING(log, "CAS storage teardown: {} detached background task(s) still in flight after "
                         "{} ms; proceeding",
                    old_pool->detachedWorkInFlightForTest(), deadline_ms);
    }, "CAS storage teardown: draining detached work");
    /// `old_pool` is released here, at the end of the function and outside every lock.
}
```

`shutdown` keeps its `gc_scheduler_mutex` round lock and its `shutdown_called` latch, then calls this
helper in place of its current body. Add `~ContentAddressedMetadataStorage()` calling it too.
Idempotence needs no second flag: a second call finds the members already empty and the local
`old_pool` null, so it does no waiting.

If `detachedWorkInFlightForTest` reads oddly in a production warning, add a non-test-named accessor and
use it in both places rather than duplicating the count.

- [ ] **Step 8: BUILD(`build_t5_green.log`)**
- [ ] **Step 9: ANALYZE(`build_t5_green.log`)**
- [ ] **Step 10: TEST(`CASDetachedWork.*`, `test_t5.log`)**
- [ ] **Step 11: ANALYZE(`test_t5.log`)**
- [ ] **Step 12: COMMIT**

```
paths:   src/Common/ProfileEvents.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: drain CAS detached work before the pool is released
```

- [ ] **Step 13: Verify the commit**

---
### Task 6: The weak `Context` and null-safe accessors {#task-6-weak-context}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Interpreters/Context.cpp`
- Modify: `src/Common/ProfileEvents.cpp`
- Test: `src/Disks/tests/gtest_cas_shutdown_context.cpp`

**Interfaces — Produces** the `ProfileEvent` `CASEventDroppedContextExpired`.

**Context.** The member is the only long-lived strong `ContextPtr` in `src/Disks`, read in four places:
two nullness checks in `startup` and the two sink builders. It becomes
`std::optional<std::weak_ptr<const Context>>`, derived in the constructor from the `ContextPtr`
parameter: a null argument gives `nullopt`.

That default already has users. `openGateStorage`
(`src/Disks/tests/gtest_cas_operation_gate.cpp:49-58`) constructs the storage with `nullptr` for the
context, so "the integration is deliberately off" is the existing behaviour of several suites and must
not change.

`startup` resolves the reference **once**, at the top, and holds the strong pointer through the single
publish step, deriving both decisions from that one local — two separate resolutions could straddle an
expiry and leave a half-configured mount. An engaged-but-expired reference at `startup` is an **error**,
not the disabled path: throwing there turns a lifetime bug into a loud failure instead of a silently
missing feature. The sinks capture the **weak** reference, never that local.

Three outcomes stay distinct, and only the first is counted:

| State | What the sink sees | Counted |
|---|---|---|
| the weak reference has expired | `lock` returns null | **yes** |
| `resetSharedContext` ran, `Context` still referenced | `lock` succeeds; the accessor returns nothing | no |
| no system log configured | the accessor returns nothing | no |

The second and third are indistinguishable at the sink, and the third is the normal state of any server
that has not enabled the CAS logs; counting them would make the counter fire constantly and mean
nothing.

The accessors adopt `Context::getZooKeeperLog`'s shape — `mutex_shared_context` held from the `shared`
test through the nested `shared->mutex` and the `shared_ptr` copy:

```cpp
std::lock_guard lock(mutex_shared_context);
if (!shared)
    return {};
SharedLockGuard lock2(shared->mutex);
if (!shared->system_logs)
    return {};
return shared->system_logs-><the log>;
```

A `shared ? … : nullptr` that released the mutex before dereferencing would leave exactly the race it
appears to remove.

- [ ] **Step 1: Add the counter** — the `M(...)` line in `src/Common/ProfileEvents.cpp` plus an
      `extern const Event` in `ContentAddressedMetadataStorage.cpp`.

- [ ] **Step 2: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_shutdown_context.cpp`.

The reset-context case is a **subprocess exit test** for the same reason as Task 4's: pre-change the
emit dereferences a null `shared` and takes the whole binary down, so running the red in-process would
hide every test behind it.

```cpp
/// `Server.cpp` calls `resetSharedContext` immediately before releasing the context. An event emitted
/// in that window must be skipped safely, not dereference a null `shared`.
TEST(CASShutdownExitTest, EmitAfterResetSharedContextExitsCleanly)
{
    EXPECT_EXIT(
        {
            ...   /// build a storage WITH a real context, emit one event to prove the sink works,
                  /// call `resetSharedContext` on that still-referenced context, emit again
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}

/// An EXPIRED weak reference is the one case that is counted.
TEST(CASShutdownContext, ExpiredContextDropsTheEventAndCountsIt)
{
    ...   /// build a storage with a context, release every other reference to it, emit
    EXPECT_EQ(after - before, 1u);
}

/// `nullptr` at construction means the integration is off. Nothing is emitted and NOTHING is counted --
/// several existing suites construct the storage this way.
TEST(CASShutdownContext, DisabledIntegrationCountsNothing)
{
    auto storage = openTestStorage();          /// context argument `nullptr`
    ...   /// drive an operation that would emit
    EXPECT_EQ(after - before, 0u);
}

/// A live context whose system log is not configured is ordinary steady state: no emit, no count.
TEST(CASShutdownContext, MissingSystemLogCountsNothing)
{
    ...
    EXPECT_EQ(after - before, 0u);
}

/// The storage must no longer keep the context alive. This is the property `Server.cpp` relies on when
/// it destroys the context explicitly.
TEST(CASShutdownContext, StorageDoesNotExtendContextLifetime)
{
    ...   /// build a storage with a context, take a weak_ptr, release every other strong reference
    EXPECT_EQ(weak_context.use_count(), 0L);
}

/// An expired reference supplied at `startup` is an error, not the disabled path.
TEST(CASShutdownContext, ExpiredContextAtStartupFails)
{
    ...   /// construct with a context, release it before calling `startup`
    EXPECT_ANY_THROW(storage->startup());
}
```

The `...` markers are context construction, which the CAS test tree does not do today — every existing
suite passes `nullptr`. Build a minimal `Context` the way `src/Interpreters/tests/` does, or use
`Context::createGlobal` with a `ContextSharedPart`; read one existing `src/Interpreters` gtest for the
exact idiom before writing these, and put the shared construction into one local helper used by all six.

- [ ] **Step 3: BUILD(`build_t6_red.log`)**
- [ ] **Step 4: ANALYZE(`build_t6_red.log`)**
- [ ] **Step 5: TEST(`CASShutdown*`, `test_t6_red.log`)**
- [ ] **Step 6: ANALYZE(`test_t6_red.log`)**

- [ ] **Step 7: Implement** the member change, the single `startup` resolution, both sinks, and the two
      accessors.

- [ ] **Step 8: BUILD(`build_t6_green.log`)**
- [ ] **Step 9: ANALYZE(`build_t6_green.log`)**
- [ ] **Step 10: TEST(`CASShutdown*`, `test_t6.log`)**
- [ ] **Step 11: ANALYZE(`test_t6.log`)**
- [ ] **Step 12: COMMIT**

```
paths:   src/Common/ProfileEvents.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
         src/Interpreters/Context.cpp
         src/Disks/tests/gtest_cas_shutdown_context.cpp
```

with this message, because a reviewer scanning the diff will see a shared upstream file:

```
fix: CAS event sinks survive a released Context

The two `Context` accessors touched here are CAS-owned -- added by CAS work,
absent from `master` -- so this changes no upstream contract. They adopt
`getZooKeeperLog`'s shape rather than a bare null check, because releasing
`mutex_shared_context` before the nested lock would leave the race it appears
to remove.
```

- [ ] **Step 13: Verify the commit**

---

### Task 7: Gate {#task-7-gate}

- [ ] **Step 1: Confirm every new suite is inside the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -hoE "TEST(_F|_P)?\([A-Za-z0-9_]+" \
    src/Disks/tests/gtest_cas_detached_work.cpp \
    src/Disks/tests/gtest_cas_shutdown_context.cpp | sort -u
```

Every printed suite name must begin with `TEST(CAS`. One that does not is invisible to the gate, and the
fix is renaming the suite — never widening the filter.

- [ ] **Step 2: BUILD(`build_gate10.log`)**
- [ ] **Step 3: ANALYZE(`build_gate10.log`)**
- [ ] **Step 4: TEST(`CAS*`, `test_gate10.log`)**
- [ ] **Step 5: ANALYZE(`test_gate10.log`)** — the summary must state the build marker: a green suite
      after a failed build is evidence about a different binary.

- [ ] **Step 6: ASan build**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_asan unit_tests_dbms > build_asan/build_gate10.log 2>&1
```

- [ ] **Step 7: ANALYZE** — as above, reading `build_asan/build_gate10.log`.

- [ ] **Step 8: ASan run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_asan/unit_tests_dbms --gtest_filter='CAS*' > build_asan/test_gate10.log 2>&1
grep -c "ERROR: AddressSanitizer" build_asan/test_gate10.log
```

The `grep` must print `0`. An exit code of zero alone is not enough: a sanitizer report can accompany a
passing suite.

- [ ] **Step 9: ANALYZE** — reading `build_asan/test_gate10.log`.

- [ ] **Step 10: The real teardown order**

No unit test exercises `Server.cpp`'s actual teardown order, which is where the original defect lives.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
mkdir -p tmp/shutdown-gate
```

Start a server whose default disk is CAS-backed with `content_addressed_log` and
`content_addressed_garbage_collection_log` enabled. Reuse the config the CA-default praktika job uses
rather than writing a new one — find it under `ci/` and copy it into `tmp/shutdown-gate`. Then:

1. run an `INSERT` large enough to produce at least one ref mutation, which auto-dispatches a snapshot publish;
2. stop the server with `SIGTERM`;
3. the process exit code is `0`;
4. `grep -c "detached background task(s) still in flight" tmp/shutdown-gate/clickhouse-server.log` prints `0`;
5. `grep -ciE "Segmentation fault|Address not mapped" tmp/shutdown-gate/clickhouse-server.log` prints `0`.

- [ ] **Step 11: Confirm the commits**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git log --oneline -8
git log -6 --stat
```

Six new commits on `cas-gc-rebuild`, each touching only the paths its task named. This checkout is
shared; verify rather than assume. **Do not push.**

## Self-Review {#self-review}

**Spec coverage.** Task 1 the registry, lease, dispatcher and exception-safe admission; Task 2 the
rollback guard, unconditional settlement and both dispatch sites; Task 3 the five recovery observation
points; Task 4 the destructor boundary; Task 5 sequencing, both teardown paths, the timeout counter and
its warning; Task 6 the whole `Context` section including the three-outcome table; Task 7 the gate and
the server start/stop the spec requires.

**Ordering.** Task 4 precedes Task 5 deliberately, and the reason is stated in Task 4's own header:
Task 5 is what makes `~Pool` routine, so guarding it afterwards would ship one commit that moves a
crash rather than fixing it. No task asserts a counter another task introduces —
`CASDetachedWorkDrainTimeouts` appears first in Task 5, where it is also incremented and tested, and
Task 3 asserts the drain's **return value** instead. Each `ProfileEvents.cpp` edit is in the same commit
as its first use, so no commit is left with a link error.

**Fail-fast.** Every build, analysis, test run and commit is a separate checkbox. No template appends
`; echo "$?"`, which would mask a failure behind the `echo`'s own success. Test logs are analyzed by a
subagent exactly as build logs are.

**Determinism.** No test opens a gate and hopes the drain got there first. `detachedWorkStoppingForTest`
and `refRecoveryWaitersForTest` are the two handshakes: the drain runs on its own thread, the test
polls the observable state with `yield`, and only then releases the worker. "The drain must not return"
is asserted as a bounded `wait_for` that is expected to time out — the only formulation that does not
race the work's completion.

**Remaining `...` markers, and why they are not placeholders.** Nine remain, and each is a *fixture*,
not logic: a runtime driven into `NeedsRecovery` (three shapes, Task 3), the storage-with-a-real-context
construction (six shapes, Task 6), and the marker read-back in Task 4. Each names the file — and where
it exists, the line range — that already contains the idiom. Every assertion, hook, handshake and
expected value around them is written out. Transcribing a 60-line fixture into a plan freezes a copy
that goes stale; naming its location does not.

**Type consistency.** `DetachedRegistryState`, `DetachedStopToken`, `DetachedTaskLease`,
`DetachedDispatchFault`, `tryDispatchDetached`, `stopAndDrainDetachedWork`,
`detachedWorkInFlightForTest` and `detachedWorkStoppingForTest` are spelled identically in Task 1's
header, its tests and every later task. Symbols this plan reuses from the tree were each read before
being written down: `refRecoveryWaitersForTest` (`CasPool.h:997`), `setCasRetrySleepForTest`
(`CasPool.h:983`, single-parameter — which is why Task 3 adds its own), `pendingSnapshotPublishesForTest`
and `waitForSnapshotPublishSettleForTest` (`CasPool.h:885`, `:889`), `runRecoveryWalkOnce` and
`checkRecoveryStillAdmitted` (`CasRefLedger.h:1047`, `:1059`), `CountingBackend::getTotal`
(`cas_test_helpers.h:1611`), and the storage constructor's `ContextPtr` parameter
(`ContentAddressedMetadataStorage.h:150-157`).
