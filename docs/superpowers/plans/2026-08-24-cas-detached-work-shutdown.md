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
ninja -C build_debug unit_tests_dbms > build_debug/<log> 2>&1; echo "NINJA_EXIT=$?"
```

Read the printed `NINJA_EXIT`. Anything other than `0` means the next step is the analysis, not the test run.

**ANALYZE(`<log>`)** — dispatch a subagent with this prompt, and do not read the log yourself:

> Read `/home/mfilimonov/workspace/ClickHouse/master/build_debug/<log>`. Report, in at most ten lines: whether the build or test run succeeded; the first error or first failing test with its file and line; and the total counts (errors, or tests run/passed/failed). Quote no more than three lines of the log.

This applies to **test** logs as well as build logs, not only to build logs.

**TEST(`<filter>`, `<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_debug/unit_tests_dbms --gtest_filter='<filter>' > build_debug/<log> 2>&1; echo "TEST_EXIT=$?"
```

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
| `src/Disks/tests/gtest_cas_detached_work.cpp` (create) | Tasks 1–4 tests. |
| `src/Disks/tests/gtest_cas_shutdown_context.cpp` (create) | Tasks 5–6 tests, including three subprocess exit tests. |

---

### Task 1: The registry, the lease, and one entry point {#task-1-registry-lease-dispatcher}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`, `.../Pool/CasPool.cpp`
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp` (create)

**Interfaces — Produces:**
- `DB::Cas::DetachedRegistryState`, `DB::Cas::DetachedStopToken` (`bool stopping() const`), `DB::Cas::DetachedTaskLease` (copyable, completes once).
- `bool Pool::tryDispatchDetached(std::function<void(DetachedStopToken)> task)`.
- `bool Pool::stopAndDrainDetachedWork(uint64_t deadline_ms)` — `false` on expiry, idempotent.
- `uint64_t Pool::detachedWorkInFlightForTest() const`.
- `PoolConfig::detached_lease_release_hook_for_test` and `PoolConfig::detached_dispatch_fault_for_test`.

**Context.** Two sites detach today, both holding a strong `Pool` reference with nobody waiting: `Pool::reportImpossibleInterference` (captures `shared_from_this`) and `CasRefLedger::dispatchSnapshotPublisher` (calls the injected `pin_owner`, declared `std::function<std::shared_ptr<void>()>`, and captures raw `this`).

Three properties decide correctness here:

1. **Release order.** The lease drops its `PoolPtr` *before* decrementing. A strong reference living in the task's captures would die with the `std::function` — after the count could already have reached zero — so a drain could report zero while `~Pool` still ran on the worker.
2. **Copyable, completes once.** `startThreadFromGlobalPool` takes `std::function<void()>`, so everything a task captures must be copy-constructible: a move-only lease will not compile, and a naively copyable one would release per copy.
3. **Exception-safe admission.** All allocation happens *before* the count is incremented. Incrementing and then failing to allocate the completion strands a count that nothing can ever decrement, and every later drain then runs to its deadline.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_detached_work.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

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

}

/// A zero in-flight count must mean no tracked task still holds the pool. The hook fires at the exact
/// boundary between releasing the lease's pool reference and decrementing the count, so an
/// implementation that decrements first is caught HERE rather than by a racy post-hoc check.
TEST(CASDetachedWork, LeaseReleasesPoolBeforeDecrementing)
{
    auto backend = std::make_shared<InMemoryBackend>();

    std::weak_ptr<Pool> weak;
    std::atomic<long> use_count_at_boundary{-1};

    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.detached_lease_release_hook_for_test
        = [&use_count_at_boundary, &weak] { use_count_at_boundary.store(weak.use_count()); };

    auto store = Pool::open(backend, config);
    weak = store;

    ASSERT_TRUE(store->tryDispatchDetached([](DetachedStopToken) {}));
    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));

    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_EQ(use_count_at_boundary.load(), 1L)
        << "at the release boundary the task still held a pool reference: the lease decremented "
           "before releasing it, so a zero count does not imply the pool is free";
    EXPECT_EQ(weak.use_count(), 1L);
}

/// The drain must not return while a task is still running.
TEST(CASDetachedWork, DrainWaitsForWorkInFlight)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    auto gate = std::make_shared<Gate>();
    std::atomic<bool> finished{false};
    ASSERT_TRUE(store->tryDispatchDetached([gate, &finished](DetachedStopToken)
    {
        gate->wait();
        finished.store(true);
    }));

    std::thread opener([gate] { gate->open(); });
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    opener.join();
    EXPECT_TRUE(finished.load());
}

/// After stopping, no new detached work may be created.
TEST(CASDetachedWork, DispatchIsRefusedAfterStop)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {}));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
}

/// A dispatch that cannot allocate must leave the count untouched, not stranded above zero.
TEST(CASDetachedWork, FailedAdmissionLeavesNoStrandedCount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::ThrowBeforeLaunch;
    auto store = Pool::open(backend, config);

    EXPECT_ANY_THROW(store->tryDispatchDetached([](DetachedStopToken) {}));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000))
        << "a stranded count makes every later drain run to its deadline";
}

/// The token a task receives must report the stop that is already under way.
TEST(CASDetachedWork, TaskObservesStopToken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

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
    std::thread opener([release] { release->open(); });
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    opener.join();
    EXPECT_TRUE(saw_stop.load());
}
```

- [ ] **Step 2: BUILD(`build_t1_red.log`)**

Expected: `NINJA_EXIT` is **not** `0`. This task's first red is a **build failure** — `tryDispatchDetached`, `DetachedStopToken`, `DetachedDispatchFault` and the two hooks do not exist yet. There is no test run in this step.

- [ ] **Step 3: ANALYZE(`build_t1_red.log`)**

Confirm the errors are exactly "no member named …" / "unknown type name …" for the names above. Any *other* compile error means the test file itself is wrong — fix it before implementing, so the red is for the intended reason.

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

In `CasPool.h`, add to `PoolConfig`:

```cpp
    /// TEST SEAM: invoked by a detached task's lease at the exact boundary between releasing its pool
    /// reference and decrementing the in-flight count.
    std::function<void()> detached_lease_release_hook_for_test = {};

    /// TEST SEAM: fault injection for the detached dispatch. `ThrowBeforeLaunch` stands in for an
    /// allocation failure raised before the thread exists; `RefuseLaunch` for a launch that failed.
    DetachedDispatchFault detached_dispatch_fault_for_test = DetachedDispatchFault::None;
```

with, above `PoolConfig`:

```cpp
enum class DetachedDispatchFault
{
    None,
    RefuseLaunch,
    ThrowBeforeLaunch,
};
```

and to `Pool`:

```cpp
    bool tryDispatchDetached(std::function<void(DetachedStopToken)> task);
    bool stopAndDrainDetachedWork(uint64_t deadline_ms);
    uint64_t detachedWorkInFlightForTest() const;

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
        throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS detached dispatch: injected pre-launch failure");

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
            throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS detached dispatch: injected launch failure");

        ThreadFromGlobalPool([lease, token, task]() mutable { task(token); }).detach();
    }
    catch (...)
    {
        /// The launch failed: the lease's own destruction (this scope) performs the release, in order.
        /// Log best-effort under a nested guard -- `tryLogCurrentException` allocates, and memory
        /// pressure is one of the conditions that brought us here.
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

Note the `lease` capture is by value into the `std::function` — which is exactly why it had to be copyable — and that `ThrowBeforeLaunch` is raised *before* the count is taken, so the test asserting an untouched count is meaningful.

`ErrorCodes::LOGICAL_ERROR` here is a test-seam-only throw on a path no input can reach; if the surrounding file has no `LOGICAL_ERROR` extern yet, add one rather than reusing an unrelated code.

- [ ] **Step 7: BUILD(`build_t1_green.log`)** — then **ANALYZE(`build_t1_green.log`)**. Expected `NINJA_EXIT=0`.

- [ ] **Step 8: TEST(`CASDetachedWork.*`, `test_t1.log`)** — then **ANALYZE(`test_t1.log`)**. Expected `TEST_EXIT=0`, five tests passing.

- [ ] **Step 9: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: track CAS detached work through one dispatcher
```

---

### Task 2: Route both dispatch sites through it {#task-2-rewire-dispatch}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp` (`reportImpossibleInterference`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (the `pin_owner` member and its constructor parameter), `.../Pool/CasRefLedger.cpp` (`dispatchSnapshotPublisher`, `settleSnapshotPublish`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (one more test hook)
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces — Consumes** Task 1's dispatcher. **Produces** `PoolConfig::publish_error_handler_hook_for_test`.

**Context.** The publisher carries a single-flight reservation: `admitSnapshotPublishUnderStateLock` increments `pending_snapshot_publishes` under `state_mutex` *before* the dispatch, and only `settleSnapshotPublish` decrements it — under one lock hold that also re-admits, so observers never see a transient zero. Therefore:

- the reservation rollback must fire **only** when no task was launched. A guard that also fired for a launched task would double-decrement, or erase a reservation a re-admission had just taken;
- settlement inside a launched task must be **unconditional**. It is reached today by a bare call after a handler that calls `tryLogCurrentException`, which allocates and can itself throw; the reservation is then stranded forever and `quiesceRefTablesForRemount` and `dropNamespace` wait on a count that never reaches zero;
- the diagnostic dispatch has no reservation and needs no rollback guard, but it keeps its caller-side `try`: it runs on a fail-closed path, and an allocation failure while building the task must not replace the exception the caller was already raising.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_detached_work.cpp`. The fixture that makes any mutation publish a snapshot is `snapshot_log_count_threshold = 0`; `publishRef` below is the same shape as the one in `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp:174-187`.

```cpp
namespace
{

PoolPtr openPublishingPool(const std::shared_ptr<InMemoryBackend> & backend, PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.snapshot_log_count_threshold = 0;          /// any nonempty tail is over-threshold
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    return Pool::open(backend, config);
}

RefTxnId publishRef(const PoolPtr & store, const RootNamespace & ns, const String & ref, uint64_t ordinal)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [&ref, ordinal](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(namespaceBirthOp());
            for (const RefOp & op : publishCommittedOps(ref, ManifestRef{1, ordinal, 1}))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
}

}

/// A dispatch that fails must not fail the mutation that triggered it, and must not strand the
/// publisher's single-flight reservation.
TEST(CASDetachedWork, FailedPublisherDispatchKeepsMutationAndClearsReservation)
{
    auto backend = std::make_shared<InMemoryBackend>();
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
TEST(CASDetachedWork, SettlementSurvivesAThrowingErrorHandler)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.publish_error_handler_hook_for_test
        = [] { throw std::runtime_error("injected: the error handler itself throws"); };
    auto store = openPublishingPool(backend, config);
    const RootNamespace ns{"srv1/handler_throws"};

    ASSERT_NO_THROW(publishRef(store, ns, "ref_1", 1));
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(store->pendingSnapshotPublishesForTest(ns), 0);
}
```

For the handler hook to be reachable, the publisher task's `catch` must invoke it before logging — added in Step 4. To make the publish itself throw, the hook is invoked from the `catch` only; if the publish succeeds in this fixture, drive a faulted publish instead, using the `OrderedFaultBackend` idiom in `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp:164-172`.

Reconcile `appendRefOps`, `MutationScope::ref`, `namespaceBirthOp`, `publishCommittedOps`, `ManifestRef`, `RootMutationOrigin` and `RootMutationKind` against that same file before compiling; they are used there verbatim.

- [ ] **Step 2: BUILD(`build_t2_red.log`)** — then **ANALYZE(`build_t2_red.log`)**. Expected: a build failure naming `publish_error_handler_hook_for_test`.

- [ ] **Step 3: Rewire the diagnostic dispatch**

In `Pool::reportImpossibleInterference`, delete the `auto self = shared_from_this();` capture — the lease owns the reference now — and dispatch through `tryDispatchDetached`, capturing raw `this`, with the token checked before the `GET`:

```cpp
    const bool dispatched = tryDispatchDetached([this, key](DetachedStopToken token)
    {
        setThreadName(ThreadName::CAS_ANOMALY_DIAG);
        if (token.stopping())
            return;
        ...   /// the existing body, unchanged
    });
    (void)dispatched;   /// best-effort: a refused diagnostic is not an error for the caller
```

Keep the surrounding `try`/`catch`. It is not redundant: this runs on a fail-closed path, and an allocation failure while building the task must not replace the exception the caller is already raising.

- [ ] **Step 4: Rewire the publisher dispatch**

In `CasRefLedger.h`, replace the `std::function<std::shared_ptr<void>()> pin_owner` member and its constructor parameter with:

```cpp
    std::function<bool(std::function<void(DetachedStopToken)>)> dispatch_detached;
```

wired at construction to `Pool::tryDispatchDetached`. In `dispatchSnapshotPublisher`:

```cpp
    ProfileEvents::increment(ProfileEvents::CASRefSnapshotPublishDispatched);

    /// The reservation was taken under `state_mutex` before this call, and only a LAUNCHED task's
    /// settlement retires it. This guard therefore covers exactly the paths on which no task runs --
    /// including an allocation failure while constructing the task, which happens in this expression,
    /// before the dispatcher's own body.
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
            /// Settlement runs on EVERY exit: a handler that throws must not be able to strand the
            /// reservation, which nothing would ever report.
            SCOPE_EXIT({ settleSnapshotPublish(ns, rt, token); });
            try
            {
                tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime(ns, rt, token);
            }
            catch (...)
            {
                if (config.publish_error_handler_hook_for_test)
                    config.publish_error_handler_hook_for_test();
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

The old hand-written `catch` that decremented and notified is deleted: the guard now covers that path and every sibling. In `settleSnapshotPublish`, take the token and skip the re-admission when it reports stopping — the decrement still happens under the same single lock hold, so no observer sees a transient zero.

`config` here must be reachable from the ledger; if it is not, pass the hook in at construction the way `pin_owner` was.

- [ ] **Step 5: BUILD(`build_t2_green.log`)** — then **ANALYZE(`build_t2_green.log`)**.

- [ ] **Step 6: TEST(`CASDetachedWork.*`, `test_t2.log`)** — then **ANALYZE(`test_t2.log`)**.

- [ ] **Step 7: TEST(`CASRef*`, `test_t2_ref.log`)** — then **ANALYZE(`test_t2_ref.log`)**. The publisher's own suites must stay green: this step changed how every snapshot publish is dispatched.

- [ ] **Step 8: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: dispatch CAS detached work through the tracked entry point
```

---

### Task 3: The stop token reaches into recovery {#task-3-recovery-stop-token}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h` (the `ensureRefTableRecovered` declaration, `recovery_retry_sleep_fn`'s signature), `.../Pool/CasRefLedger.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (one test hook)
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces — Produces** `PoolConfig::recovery_pre_first_request_hook_for_test`.

**Context.** `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` calls `ensureRefTableRecovered` **before** it constructs `runtime_still_admitted`, so a publisher whose runtime needs recovery sits below every checkpoint Task 2 added. Four places must observe the token, and two of them are above all the others:

| Place | Why the others do not cover it |
|---|---|
| the wait for a concurrent recovery | `rt.recovery_cv.wait(lock)` is called with **no predicate and no deadline** while `recovery_in_progress`; a second publisher parks there without reaching any I/O |
| before the walk's **first** backend request | a fresh recovery reads the checkpoint before its first admission check |
| the per-attempt I/O and retry checkpoints | the ordinary case |
| the slice loop of `recovery_retry_sleep_fn` | it slices at 200 ms but exits early only on `!fence_ok_fn()`, and a terminal shutdown does not drop the mount fence; with `recovery_retry_max_backoff_ms` at 30 s against a deadline of seconds, this alone expires the drain |

**Two mechanics that are easy to get wrong:**

1. **A predicate does not wake anyone.** `stopAndDrainDetachedWork` notifies the *registry's* condition variable; a thread parked on `rt.recovery_cv` never sees it. The concurrent-recovery wait therefore becomes a **bounded slice wait** — `wait_for` in 200 ms slices, re-checking the token each time — mirroring the sliced backoff that already exists a few lines away. Do not try to notify every runtime's `recovery_cv` from the registry: that would make the pool-level registry reach into ledger internals for no gain.
2. **Token-stop must be terminal for the retry loop.** The loop's existing terminal latch is `cancelled`, which belongs to remount cancellation. Add a separate local terminal condition for the token so `isTransientRecoveryError` cannot re-drive the walk: a token-stopped attempt rethrows and does **not** retry.

`ensureRefTableRecovered` gains a token parameter. It has many callers that are not detached work, so the parameter is `std::optional<DetachedStopToken>`, and `std::nullopt` means "no detached-work stop applies" — the existing behaviour, unchanged, for every non-publisher caller.

- [ ] **Step 1: Write the failing tests**

Append three tests. Each asserts that `stopAndDrainDetachedWork` **returns true** well inside a short deadline. Do not assert `CASDetachedWorkDrainTimeouts` here: that counter is introduced in Task 4, by the caller that consumes this return value.

```cpp
/// A publisher asleep in recovery backoff must be woken by the stop, not waited out. The injected
/// sleep stands in for a long backoff without spending wall-clock time.
TEST(CASDetachedWork, StopWakesRecoveryBackoffSleep)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPublishingPool(backend);
    const RootNamespace ns{"srv1/backoff"};

    auto sleeping = std::make_shared<Gate>();
    store->setRecoveryRetrySleepForTest([sleeping](uint64_t, const std::optional<DetachedStopToken> & token)
    {
        sleeping->open();
        while (!(token && token->stopping()))
            std::this_thread::yield();
    });

    /// Drive a publish whose recovery fails transiently, so the loop enters its backoff.
    ...   /// see Step 2 for the fault fixture
    sleeping->wait();

    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/2000));
}

/// A publisher parked behind another runtime's in-flight recovery is below every I/O checkpoint.
TEST(CASDetachedWork, StopWakesAConcurrentRecoveryWaiter)
{
    ...   /// hold one recovery in flight, poll `recoveryWaitersForTest(ns)` until it reports 1
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/2000));
}

/// The token is checked BEFORE the walk's first backend request, so a stop that arrives while the
/// publisher is at that boundary means the request is never issued at all.
///
/// Note what this does NOT claim: a request already in flight is not interrupted. That case is a
/// bounded drain timeout by design, not a bug -- so the test parks the publisher at the pre-request
/// hook rather than inside a stalled GET.
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
    auto store = openPublishingPool(backend, config);
    ...   /// drive a publish that needs recovery

    at_boundary->wait();
    const uint64_t gets_before = backend->getTotal();
    std::thread opener([release] { release->open(); });
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/2000));
    opener.join();
    EXPECT_EQ(backend->getTotal(), gets_before)
        << "the walk issued its first request after the stop was already latched";
}
```

The three `...` markers are fixture bodies, not logic: each needs a runtime driven into `NeedsRecovery`. Copy the fault fixture from `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp:164-172` (`OrderedFaultBackend` plus `budget.max_attempts = 1`) and the recovery-driving setup from the nearest test in `src/Disks/tests/gtest_cas_ref_writer.cpp` that reaches `RefLaneState::NeedsRecovery`. `CountingBackend` and its `getTotal` are in `src/Disks/tests/cas_test_helpers.h:1440`; confirm the accessor's exact name there before use.

- [ ] **Step 2: BUILD(`build_t3_red.log`)**, **ANALYZE**, then **TEST(`CASDetachedWork.Stop*`, `test_t3_red.log`)**, **ANALYZE**

Expected: all three fail by returning `false` from the drain — the deadline expires. A failure of any other shape means the fixture did not reach recovery; fix the fixture before implementing.

- [ ] **Step 3: Thread the token through**

- `void ensureRefTableRecovered(const RootNamespace & ns, RefTableRuntime & rt, const std::optional<DetachedStopToken> & token = std::nullopt);` — every existing caller keeps its current behaviour by omitting the argument; the publisher passes its token.
- `recovery_retry_sleep_fn` becomes `std::function<void(uint64_t, const std::optional<DetachedStopToken> &)>`, and its default implementation adds the token to the slice loop's condition:
  `while (slept < total_ms && fence_ok_fn() && !(token && token->stopping()))`.
- The concurrent-recovery wait becomes a bounded slice wait:
  ```cpp
  while (rt.recovery_in_progress)
  {
      if (token && token->stopping())
          throw Exception(ErrorCodes::ABORTED, "CAS ref recovery: teardown began while waiting for a concurrent recovery");
      ++rt.recovery_waiters_for_test;
      rt.recovery_cv.wait_for(lock, std::chrono::milliseconds(200));
      --rt.recovery_waiters_for_test;
  }
  ```
  A predicate alone would not help: nothing notifies `recovery_cv` when the token flips.
- Immediately before the walk's first backend request, invoke `config.recovery_pre_first_request_hook_for_test` if set, then check the token and throw `ABORTED` if stopping.
- In the retry loop's fail-closed condition, add the token beside `cancelled` so a token-stopped attempt is **terminal** and is never reclassified as transient.

- [ ] **Step 4: BUILD(`build_t3_green.log`)** → **ANALYZE** → **TEST(`CASDetachedWork.*`, `test_t3.log`)** → **ANALYZE** → **TEST(`CASRef*`, `test_t3_ref.log`)** → **ANALYZE**

- [ ] **Step 5: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: let CAS teardown interrupt ref-table recovery
```

---

### Task 4: Teardown sequencing and the destructor {#task-4-teardown-sequencing}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`, `.../ContentAddressedMetadataStorage.cpp`
- Modify: `src/Common/ProfileEvents.cpp`
- Test: `src/Disks/tests/gtest_cas_detached_work.cpp`

**Interfaces — Produces** the `ProfileEvent` `CASDetachedWorkDrainTimeouts`.

**Context.** `poolAccess` copies `cas_store` into a strong `PoolPtr` under `pointer_mutex`, so a drain that runs while the member still holds the pool proves nothing: a caller can take a fresh reference right after the wait succeeds. And `ContentAddressedMetadataStorage` has no destructor today, so an instance destroyed without `shutdown` drops its pool with no drain at all.

The production deadline is `config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms` — the same one `CasRefLedger::drainRefLanesForShutdown` already uses.

- [ ] **Step 1: Add the counter**

In `src/Common/ProfileEvents.cpp`, beside the other `CAS…` entries:

```cpp
    M(CASDetachedWorkDrainTimeouts, "Counts CAS storage teardowns whose bounded wait for detached background work expired with work still in flight. The teardown proceeds, but the guarantee that no tracked task still holds the pool could not be established for that teardown. Expected to stay at zero.", ValueType::Number) \
```

and an `extern const Event CASDetachedWorkDrainTimeouts;` in `ContentAddressedMetadataStorage.cpp`'s `ProfileEvents` block.

- [ ] **Step 2: Write the failing tests**

```cpp
/// `shutdown` must not return while tracked detached work is in flight.
TEST(CASDetachedWork, ShutdownWaitsForDetachedWork) { ... }

/// A storage destroyed WITHOUT `shutdown` must still drain: nothing today prevents that path.
TEST(CASDetachedWork, ImplicitDestructionDrains) { ... }

/// And a storage destroyed AFTER `shutdown` must find nothing to do, rather than waiting a second
/// deadline on a pool that is already gone.
TEST(CASDetachedWork, TeardownHelperIsIdempotent) { ... }
```

Each needs a `ContentAddressedMetadataStorage` rather than a bare `Pool`. Build it the way `src/Disks/tests/gtest_cas_pool.cpp` builds one — read that file for the construction idiom and the arguments its constructor takes — and use the `detached_lease_release_hook_for_test` gate from Task 1 to hold a task in flight. For idempotence, assert the second teardown returns promptly by measuring against a deliberately long configured deadline.

- [ ] **Step 3: BUILD(`build_t4_red.log`)** → **ANALYZE** → **TEST(`CASDetachedWork.*`, `test_t4_red.log`)** → **ANALYZE**

- [ ] **Step 4: Implement the sequencing**

Extract a private, idempotent helper on `ContentAddressedMetadataStorage`:

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
    /// Everything below runs OUTSIDE `pointer_mutex`. That is required, not incidental: waiting under
    /// it would block snapshot readers for the whole wait, and it is also what keeps a `~Pool`
    /// triggered by the last local reference from running under that mutex.
    try
    {
        if (old_scheduler)
            old_scheduler->stop();
        old_part_access.reset();
        if (old_pool)
        {
            const uint64_t deadline_ms = old_pool->poolConfig().cas_request_budget.attempt_timeout_ms
                + old_pool->poolConfig().cas_request_budget.lease_safety_margin_ms;
            if (!old_pool->stopAndDrainDetachedWork(deadline_ms))
            {
                ProfileEvents::increment(ProfileEvents::CASDetachedWorkDrainTimeouts);
                LOG_WARNING(log, "CAS storage teardown: {} detached background task(s) still in flight "
                                 "after {} ms; proceeding",
                            old_pool->detachedWorkInFlightForTest(), deadline_ms);
            }
        }
    }
    catch (...)
    {
        try
        {
            tryLogCurrentException(log, "CAS storage teardown");
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }
    /// `old_pool` is released here, at the end of the function and outside every lock.
}
```

`shutdown` keeps its `gc_scheduler_mutex` round lock and its `shutdown_called` latch, then calls this helper in place of its current body. Add `~ContentAddressedMetadataStorage()` that calls it too. Idempotence follows from the members already being empty on a second call — assert that in a comment rather than adding a second flag.

The `…ForTest` accessor is used in a warning message here; if that is objectionable, add a non-test-named accessor and use it in both places rather than duplicating the counter.

- [ ] **Step 5: BUILD(`build_t4_green.log`)** → **ANALYZE** → **TEST(`CASDetachedWork.*`, `test_t4.log`)** → **ANALYZE**

- [ ] **Step 6: COMMIT**

```
paths:   src/Common/ProfileEvents.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
         src/Disks/tests/gtest_cas_detached_work.cpp
message: fix: drain CAS detached work before the pool is released
```

---

### Task 5: The weak `Context` and null-safe accessors {#task-5-weak-context}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (the `context` member), `.cpp` (`startup`, both sink builders)
- Modify: `src/Interpreters/Context.cpp`
- Modify: `src/Common/ProfileEvents.cpp`
- Test: `src/Disks/tests/gtest_cas_shutdown_context.cpp` (create)

**Interfaces — Produces** the `ProfileEvent` `CASEventDroppedContextExpired`.

**Context.** The member is the only long-lived strong `ContextPtr` in `src/Disks`, read in four places: two nullness checks in `startup` and the two sink builders. It becomes `std::optional<std::weak_ptr<const Context>>`: `nullopt` means the integration is deliberately off; an engaged-but-expired reference during `startup` is an **error**, not the disabled path.

`startup` resolves it **once**, at the top, and holds the strong pointer through the single publish step, deriving both decisions from that one local — two separate resolutions could straddle an expiry and leave a half-configured mount. The sinks capture the **weak** reference, never that local.

Three outcomes stay distinct and only the first is counted:

| State | What the sink sees | Counted |
|---|---|---|
| the weak reference has expired | `lock` returns null | **yes** |
| `resetSharedContext` ran, `Context` still referenced | `lock` succeeds; the accessor returns nothing | no |
| no system log configured | the accessor returns nothing | no |

The accessors adopt `Context::getZooKeeperLog`'s shape — `mutex_shared_context` held from the `shared` test through the nested `shared->mutex` and the `shared_ptr` copy. A bare `shared ? … : nullptr` would leave the race it appears to remove.

- [ ] **Step 1: Add the counter** — the `M(...)` line in `src/Common/ProfileEvents.cpp` plus an `extern const Event` in `ContentAddressedMetadataStorage.cpp`.

- [ ] **Step 2: Write the failing tests**

Create `src/Disks/tests/gtest_cas_shutdown_context.cpp`. Suite `CASShutdownContext`, plus one exit-test suite `CASShutdownContextExitTest`.

- **subprocess exit test** — emit after `resetSharedContext` on a still-referenced `Context`. Pre-change this dereferences a null `shared` and takes the whole binary down, so it cannot be an ordinary in-process test:

```cpp
TEST(CASShutdownContextExitTest, EmitAfterResetSharedContextExitsCleanly)
{
    EXPECT_EXIT(
        {
            ...   /// build a storage with a context, resetSharedContext, emit one event
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}
```

- an expired weak reference advances `CASEventDroppedContextExpired`;
- `nullopt` (integration off) emits nothing and advances **no** counter;
- a context whose system log is not configured emits nothing and advances **no** counter;
- holding the storage does not keep the `Context` alive: release every other reference, assert it was destroyed;
- an expired reference supplied at `startup` fails startup with an exception rather than taking the disabled path.

- [ ] **Step 3: BUILD(`build_t5_red.log`)** → **ANALYZE** → **TEST(`CASShutdownContext*`, `test_t5_red.log`)** → **ANALYZE**

- [ ] **Step 4: Implement** the member change, the single `startup` resolution, the two sinks, and the two accessors.

- [ ] **Step 5: BUILD(`build_t5_green.log`)** → **ANALYZE** → **TEST(`CASShutdownContext*`, `test_t5.log`)** → **ANALYZE**

- [ ] **Step 6: COMMIT**

```
paths:   src/Common/ProfileEvents.cpp
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
         src/Interpreters/Context.cpp
         src/Disks/tests/gtest_cas_shutdown_context.cpp
```

with this commit message body, because a reviewer scanning the diff will see a shared upstream file:

```
fix: CAS event sinks survive a released Context

The two `Context` accessors touched here are CAS-owned -- added by CAS work,
absent from `master` -- so this changes no upstream contract. They adopt
`getZooKeeperLog`'s shape rather than a bare null check, because releasing
`mutex_shared_context` before the nested lock would leave the race it appears
to remove.
```

---

### Task 6: A destructor boundary that cannot terminate {#task-6-failsoft-destructor}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp` (`~Pool`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h` (three test hooks)
- Test: `src/Disks/tests/gtest_cas_shutdown_context.cpp`

**Context.** `~Pool` has three phases and only the keeper's `release` inside `CasMountRuntime::finishTeardown` is guarded today:

1. `mount_runtime.stopBackgroundWorkers()`
2. the ref-lane drain, from which `drained` is computed with `writerCleanupDutiesPending`
3. `mount_runtime.finishTeardown(drained)` — which calls `stopBackgroundWorkers` **again**, so guarding only phase 1 leaves that same call unguarded on its second invocation

`drained` must be **initialized to `false`** before phase 2 rather than assigned from it, and a throw in phase 2 must still reach phase 3. That default is load-bearing: `finishTeardown(true)` writes the clean-release marker, which a successor reads as proof that no in-flight conditional `PUT` from this incarnation can still land. A guard that swallowed a throw while leaving `drained` at a partial value could forge that proof.

Each guard's own logging goes in a nested `try`/`catch(...)`: `tryLogCurrentException` allocates, and memory pressure is exactly when a teardown phase throws.

- [ ] **Step 1: Add three hooks** to `PoolConfig` — `teardown_phase1_throw_for_test`, `teardown_phase2_throw_for_test`, `teardown_phase3_throw_for_test`, each `std::function<void()>`, invoked at the top of its phase.

- [ ] **Step 2: Write the failing tests** — three subprocess exit tests, one per phase:

```cpp
TEST(CASShutdownContextExitTest, PoolTeardownPhase2ThrowExitsCleanlyAndSkipsTheMarker)
{
    EXPECT_EXIT(
        {
            ...   /// open a pool with `teardown_phase2_throw_for_test` set, destroy it,
                  /// then assert no clean-release marker was written
            std::exit(0);
        },
        ::testing::ExitedWithCode(0), "");
}
```

The marker assertion is the point, not the exit code: a guard that survived the throw but forged the marker would pass an exit-code-only test. For phases 1 and 3, assert the clean exit and that the remaining phases still ran.

- [ ] **Step 3: BUILD(`build_t6_red.log`)** → **ANALYZE** → **TEST(`CASShutdownContextExitTest.Pool*`, `test_t6_red.log`)** → **ANALYZE**. Expected: the subprocess terminates abnormally.

- [ ] **Step 4: Implement** the three guards and the `drained = false` default.

- [ ] **Step 5: BUILD(`build_t6_green.log`)** → **ANALYZE** → **TEST(`CASShutdownContext*`, `test_t6.log`)** → **ANALYZE**

- [ ] **Step 6: COMMIT**

```
paths:   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
         src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
         src/Disks/tests/gtest_cas_shutdown_context.cpp
message: fix: no CAS teardown phase can terminate the process
```

---

### Task 7: Gate {#task-7-gate}

- [ ] **Step 1: Confirm every new suite is inside the gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -hoE "TEST(_F|_P)?\([A-Za-z0-9_]+" \
    src/Disks/tests/gtest_cas_detached_work.cpp \
    src/Disks/tests/gtest_cas_shutdown_context.cpp | sort -u
```

Every printed suite name must begin with `CAS`. One that does not is invisible to the gate, and the fix is renaming the suite — never widening the filter.

- [ ] **Step 2: BUILD(`build_gate10.log`)** → **ANALYZE(`build_gate10.log`)**

- [ ] **Step 3: TEST(`CAS*`, `test_gate10.log`)** → **ANALYZE(`test_gate10.log`)**

The analysis must state the build marker explicitly: a green suite after a failed build is evidence about a different binary.

- [ ] **Step 4: ASan gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_asan unit_tests_dbms > build_asan/build_gate10.log 2>&1; echo "NINJA_EXIT=$?"
```

Then, only on `NINJA_EXIT=0`:

```bash
./build_asan/unit_tests_dbms --gtest_filter='CAS*' > build_asan/test_gate10.log 2>&1; echo "TEST_EXIT=$?"
grep -c "ERROR: AddressSanitizer" build_asan/test_gate10.log
```

The `grep` must print `0`. Analyze both logs with a subagent as above, reading `build_asan/…` paths.

- [ ] **Step 5: The real teardown order**

No unit test exercises `Server.cpp`'s actual teardown order, which is where the original defect lives.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
mkdir -p tmp/shutdown-gate
```

Start a server whose default disk is CAS-backed with `content_addressed_log` and
`content_addressed_garbage_collection_log` enabled — reuse the config the CA-default praktika job
uses rather than writing a new one; find it under `ci/` and copy it into `tmp/shutdown-gate`. Then:

1. run an `INSERT` large enough to produce at least one ref mutation, which auto-dispatches a snapshot publish;
2. stop the server with `SIGTERM`;
3. assert the process exit code is `0`;
4. `grep -c "detached background task(s) still in flight" <server log>` prints `0`;
5. `grep -ci "Segmentation fault\|Address not mapped" <server log>` prints `0`.

- [ ] **Step 6: Confirm the commits**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git log --oneline -8
git log -6 --stat
```

Six new commits on `cas-gc-rebuild`, each touching only the paths its task named. This checkout is shared; verify rather than assume. **Do not push.**

## Self-Review {#self-review}

**Spec coverage.** Task 1 the registry, lease, dispatcher and exception-safe admission; Task 2 the rollback guard, unconditional settlement and both dispatch sites; Task 3 the four recovery observation points; Task 4 sequencing, both teardown paths, the timeout counter and its warning; Task 5 the whole `Context` section including the three-outcome table; Task 6 the destructor boundary; Task 7 the gate, including the server start/stop the spec requires.

**Placeholders.** Four fixture bodies are marked `...` and each names the exact file and line range to copy from: the fault backend and publish driver (`gtest_cas_ref_snapshot_publish_ordering.cpp:164-187`), a `NeedsRecovery` setup (`gtest_cas_ref_writer.cpp`), the storage construction idiom (`gtest_cas_pool.cpp`), and `CountingBackend` (`cas_test_helpers.h:1440`). These are fixtures, not logic: transcribing them here would freeze a copy that goes stale, whereas every assertion, hook and expectation around them is written out in full.

**Ordering.** No task asserts a counter another task introduces. `CASDetachedWorkDrainTimeouts` appears first in Task 4, which is also where it is incremented; Task 3 asserts the drain's **return value** instead. Both `ProfileEvents.cpp` edits are in the same commit as their first use, so no commit is left with a link error.

**Fail-fast.** Every build, analysis, test run and commit is a separate step; no step runs a binary after a failed build, and no commit follows a failed run. Test logs are analyzed by a subagent exactly as build logs are.

**Type consistency.** `DetachedRegistryState`, `DetachedStopToken`, `DetachedTaskLease`, `DetachedDispatchFault`, `tryDispatchDetached`, `stopAndDrainDetachedWork` and `detachedWorkInFlightForTest` are spelled identically in Task 1's header, its tests and every later task. The five test hooks are named `*_for_test`, matching `PoolConfig`'s existing convention.
