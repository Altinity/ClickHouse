---
description: 'Implementation plan for the CAS GC teardown stop: the open request plane carries the pool teardown flag as its fence, a streamed body is re-admitted per refill, the retry sleep wakes, and a round cut by teardown is recorded as Stopped'
sidebar_label: 'CAS GC teardown stop plan'
sidebar_position: 8
slug: /superpowers/plans/cas-gc-teardown-stop
title: 'CAS GC Teardown Stop Implementation Plan'
doc_type: 'plan'
---

# CAS GC teardown stop — Implementation Plan {#cas-gc-teardown-stop-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A content-addressed disk's teardown no longer waits out a GC round: the round is refused at its next request, its next retry sleep or its next streamed-body refill, and is recorded as `Stopped`.

**Architecture:** `Pool::beginTeardown` arms the pool's existing `DetachedRegistryState::stopping` (now atomic) before any teardown lock or join. The open request plane (`gc_requests`) is built with a `Fence` whose `admit` refuses once that flag is set and with an interruptible sleep woken by the same condition variable; `CasOperation::stream` wraps the SDK body in a delegating `ReadBuffer` that re-admits per refill. `CasGcScheduler` classifies a transient failure after the arm as `Stopped` and refuses to start a round after it. Every join and every object's ownership stay exactly as they are.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`, `src/Interpreters/ContentAddressedGarbageCollectionLog.*`), gtest via `unit_tests_dbms`, CMake + ninja, pytest integration tests (`tests/integration`), the ca-soak scenario framework (`utils/ca-soak`).

**Spec:** `docs/superpowers/specs/2026-09-04-cas-gc-teardown-stop-design.md` (rev.4). The plan argues from the spec; read both.

## Global Constraints {#global-constraints}

- **Worktree and branch.** All code work happens in the `lane-g` worktree at `/home/mfilimonov/workspace/ClickHouse/lane-g`, on a NEW branch `cas-gc-teardown-stop` created from `cas-gc-rebuild` (Task 0). It has a configured `build/` (`build/src/unit_tests_dbms`, `build/programs/clickhouse`) and populated `contrib/`, so incremental builds are cheap. The plan document itself lives in the `master` worktree on `cas-gc-rebuild` and is not edited from `lane-g`. **Merge back into `cas-gc-rebuild` only in Task 9, only after the full gate is green.** No rebase, no amend, new commits only. **Do not push.**
- **Shared checkouts.** Stage named paths only, never `git add -A`. Every commit is `git add <paths> && git commit -m "..." -- <paths>` followed by `git log -1 --stat`, whose output must list exactly those paths. Before the merge in Task 9, `git -C master diff --cached --stat` must be empty.
- **Suite naming is a gate.** The CAS gate is exactly `--gtest_filter='CAS*'`. Every new suite MUST be named `CAS…` (`CASGCTeardownStop`, `CASRequests`, `CASGCLog`); a suite that does not match never runs in the gate.
- **No sleep to order threads.** A gate (condition variable), a state handshake (`teardownBegun()`), or a bounded `wait_for` whose expiry is itself the assertion. `std::this_thread::yield()` inside a bounded poll of a state flag is allowed, as `awaitStopLatched` in `gtest_cas_detached_work.cpp` already does.
- **No durable-format, key-shape or protocol-step change.** The `system.cas_gc_log` `Enum8` gains one value; nothing persisted by the pool changes.
- **Nothing outside `ContentAddressed/` except:** `src/Interpreters/ContentAddressedGarbageCollectionLog.{h,cpp}` (the log element, CAS-owned), `src/Disks/tests/*.cpp`, `tests/integration/test_cas_gc_s3/test.py`, `utils/ca-soak/scenarios/cards/`. No edit to `src/IO`, `ReadBufferFromS3`, `ObjectStorageBackend::stream`, the fold or the read-ahead.
- **C++ style:** Allman braces. **Comments:** state the reason; never cite this plan, the spec, a review, a backlog or a task number. Function names in prose as `f`, not `f()`.
- **Fail fast.** Never run a test binary after a failed build; never commit after a failed test. Each build, each analysis, each test run and each commit is its own step.
- **Build and test logs** go to the build directory and are analysed by a subagent, never read directly (see Conventions).
- New `gtest_*.cpp` files under `src/Disks/tests/` need no CMake edit (globbed with `CONFIGURE_DEPENDS`).

## Conventions {#conventions}

Three step templates, referenced by name from every task. Each occurrence names its own log file so parallel runs never overwrite each other. `LG` is `/home/mfilimonov/workspace/ClickHouse/lane-g`.

**BUILD(`<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
ninja -C build unit_tests_dbms > build/<log> 2>&1
```

No `; echo "$?"` after it — that would report the `echo`'s status. A nonzero status means the next step is the analysis, never the test run.

**ANALYZE(`<log>`)** — dispatch a subagent (`ca-review-lite` or `general-purpose`, sonnet, medium effort) with this prompt, and do not read the log yourself:

> Read `/home/mfilimonov/workspace/ClickHouse/lane-g/build/<log>`. Report, in at most ten lines: whether the build or test run succeeded; the first error or first failing test with its file and line; and the total counts (errors, or tests run/passed/failed). Quote no more than three lines of the log.

**TEST(`<filter>`, `<log>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
./build/src/unit_tests_dbms --gtest_filter='<filter>' > build/<log> 2>&1
```

Same rule: a nonzero status means the run failed, and the next step is the analysis — never the commit. A test run is valid only if the BUILD that preceded it succeeded: a green suite after a failed build is the wrong binary.

**COMMIT(`<paths>`, `<message>`)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
git add <paths>
git commit -m "<message>

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VhphEVZSskrNzFKUGkHyPi" -- <paths>
git log -1 --stat
```

Read the `git log -1 --stat` output and confirm it lists exactly `<paths>` and nothing else.

## File Structure {#file-structure}

| File | Responsibility |
|---|---|
| `Pool/CasDetachedWork.h` / `.cpp` (modify) | `DetachedRegistryState::stopping` becomes `std::atomic<bool>`; the token reads it without the mutex. |
| `Pool/CasPool.h` / `.cpp` (modify) | `beginTeardown`, `teardownBegun`, `openPlaneSleepFn`; the open plane's fence and sleep at construction; `stopAndDrainDetachedWork` arms through `beginTeardown`; `setCasRetrySleepForTest` restores the open plane's sleep; `Pool::forgetDisk` arms as step 0. |
| `Backend/CasRequests.cpp` (modify) | `throwReadRefused` (the one refusal mapping), `AdmittedBodyReadBuffer`, `CasOperation::stream` wraps. No header change. |
| `Gc/CasGcScheduler.h` / `.cpp` (modify) | `Outcome::Stopped`; the classification rule in `runRoundLogged`; the post-lock check and the INFO line in `loop`; the heartbeat's early exit. |
| `src/Interpreters/ContentAddressedGarbageCollectionLog.h` / `.cpp` (modify) | `STOPPED = 7`, the `Enum8` value, the column comments. |
| `ContentAddressedMetadataStorage.h` / `.cpp` (modify) | `shutdown` and `forgetDisk` arm before their locks; `stopAndDrainForTeardown` arms before the scheduler join; `Outcome::Stopped` mapped; `setGcRoundRowHookForTest` seam in `makeGcRoundLogger`. |
| `src/Disks/tests/gtest_cas_gc_teardown_stop.cpp` (create) | Suite `CASGCTeardownStop`: the arm, the plane wiring, the sleep wiring, the read-ahead worker, the background round cut mid-flight, the storage-level shutdown, the queued tick, the janitor swallow. |
| `src/Disks/tests/gtest_cas_requests.cpp` (modify) | Suite `CASRequests`: the body wrapper's windows, EOF, refusal and `NoBudget` mapping. |
| `src/Disks/tests/gtest_cas_gc_log.cpp` (modify) | Suite `CASGCLog`: the `Stopped`/`Failed` classification twins. |
| `tests/integration/test_cas_gc_s3/test.py` (modify) | `test_restart_under_gc_cuts_the_round_short`: paused baseline, calibrated round, witnessed `Stopped`. |
| `utils/ca-soak/scenarios/cards/s46_restart_under_gc.py` (create) | Scenario S46: N graceful restarts under GC, stop-time bound, final fsck. |

---

### Task 0: Branch and baseline in `lane-g` {#task-0}

**Files:** none edited.

- [ ] **Step 1: Confirm `lane-g` has no tracked changes**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
git status --short | grep -v '^??' | grep -v '^ ? contrib'
```

Expected: no output. If there is output, stop and report it; do not stash or discard anything.

- [ ] **Step 2: Create the branch from `cas-gc-rebuild`**

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
git switch -c cas-gc-teardown-stop cas-gc-rebuild
git log --oneline -1
```

Expected: the branch is created and the head is `cas-gc-rebuild`'s current head (the commit whose subject starts with `ca-docs: GC teardown-stop design rev.4` or a later one).

- [ ] **Step 3: Baseline build** — BUILD(`build_t0_baseline.log`)

- [ ] **Step 4: Analyse** — ANALYZE(`build_t0_baseline.log`). Expected: success.

- [ ] **Step 5: Baseline run of the suites this plan touches** — TEST(`CASDetachedWork*:CASGCLog*:CASRequests*:CASGCReadAhead*`, `test_t0_baseline.log`)

- [ ] **Step 6: Analyse** — ANALYZE(`test_t0_baseline.log`). Expected: all pass. This is the reference the later runs are compared against.

---

### Task 1: The arm — atomic `stopping`, `Pool::beginTeardown`, `Pool::teardownBegun` {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h` (the struct at the top)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp:7-11`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:412-416` (the detached-work declarations)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1038-1062` (`stopAndDrainDetachedWork`, `detachedWorkStoppingForTest`)
- Create: `src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`

**Interfaces:**
- Produces: `void Pool::beginTeardown() noexcept` — idempotent; sets the flag under the registry mutex and notifies `detached_work->cv`. `bool Pool::teardownBegun() const noexcept` — an acquire load of the flag. Every later task calls these two.

- [ ] **Step 1: Write the failing test** — create `src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

/// A disk's teardown must not wait out a GC round. The pool's teardown flag is the open request
/// plane's fence, so a round in flight is refused at its next request, its next retry sleep or its
/// next streamed refill; the joins stay and the round becomes short. These tests pin the arm, the
/// plane wiring, the sleep wiring, and the scheduler's behaviour around a round that was cut.

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

namespace CurrentMetrics
{
extern const Metric LocalThread;
extern const Metric LocalThreadActive;
extern const Metric LocalThreadScheduled;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::expectThrowsCode;

namespace
{

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend, PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    return Pool::open(backend, config);
}

}

/// The arm is idempotent, observable, and closes the door to new detached work; a drain after an
/// early arm has nothing to wait for.
TEST(CASGCTeardownStop, BeginTeardownIsIdempotentAndRefusesNewDetachedWork)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    EXPECT_FALSE(store->teardownBegun());
    store->beginTeardown();
    EXPECT_TRUE(store->teardownBegun());
    store->beginTeardown();
    EXPECT_TRUE(store->teardownBegun()) << "a second arm changes nothing";
    EXPECT_TRUE(store->detachedWorkStoppingForTest());

    EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {}))
        << "no detached task is accepted once teardown began";
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000))
        << "the drain after an early arm finds nothing in flight and returns at once";
}
```

- [ ] **Step 2: Build and confirm the test does not compile** — BUILD(`build_t1_red.log`), then ANALYZE(`build_t1_red.log`). Expected: FAIL — `Pool` has no member `beginTeardown` / `teardownBegun`.

- [ ] **Step 3: Make `stopping` atomic** — in `Pool/CasDetachedWork.h`, add `#include <atomic>` to the includes and replace the struct:

```cpp
/// Accounting for detached CAS work. Held by `shared_ptr` and owned by the pool AND by every task
/// lease, so it outlives the pool it accounts for: a lease must be able to finish releasing after the
/// pool is already gone.
struct DetachedRegistryState
{
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t in_flight = 0;
    /// Written under `mutex`, so the dispatch-side check-and-count and the drain's `in_flight == 0`
    /// wait stay serialized against the stop; read WITHOUT it by the open request plane's fence before
    /// every attempt and every sleep of every request -- a pool-wide mutex on that path is not
    /// acceptable, and the readers need only the flag's current truth.
    std::atomic<bool> stopping{false};
};
```

In `Pool/CasDetachedWork.cpp`, replace `DetachedStopToken::stopping`:

```cpp
bool DetachedStopToken::stopping() const
{
    return state->stopping.load(std::memory_order_acquire);
}
```

- [ ] **Step 4: Declare and implement the arm** — in `Pool/CasPool.h`, directly after `bool tryDispatchDetached(std::function<void(DetachedStopToken)> task);` insert:

```cpp
    /// Marks this pool as being torn down. The open request plane refuses every further admission and
    /// wakes a retry sleep on it, and no new detached task is accepted. Idempotent, and it frees,
    /// nulls and swaps nothing: it can be called before any lock a teardown takes, so a GC round
    /// holding such a lock is refused at its next request instead of being waited out.
    void beginTeardown() noexcept;
    bool teardownBegun() const noexcept;
```

In `Pool/CasPool.cpp`, replace `stopAndDrainDetachedWork` and `detachedWorkStoppingForTest` and add the two new functions in front of them:

```cpp
void Pool::beginTeardown() noexcept
{
    {
        std::lock_guard lock(detached_work->mutex);
        detached_work->stopping.store(true, std::memory_order_release);
    }
    detached_work->cv.notify_all();
}

bool Pool::teardownBegun() const noexcept
{
    return detached_work->stopping.load(std::memory_order_acquire);
}

bool Pool::stopAndDrainDetachedWork(uint64_t deadline_ms)
{
    beginTeardown();
    std::unique_lock lock(detached_work->mutex);
    return detached_work->cv.wait_for(lock, std::chrono::milliseconds(deadline_ms),
                                      [this] { return detached_work->in_flight == 0; });
}

uint64_t Pool::detachedWorkInFlight() const
{
    std::lock_guard lock(detached_work->mutex);
    return detached_work->in_flight;
}

bool Pool::detachedWorkStoppingForTest() const
{
    return teardownBegun();
}
```

`tryDispatchDetached`'s `if (detached_work->stopping)` under the mutex needs no edit: the implicit load of an atomic is fine there.

- [ ] **Step 5: Build** — BUILD(`build_t1.log`), then ANALYZE(`build_t1.log`). Expected: success.

- [ ] **Step 6: Run the new test and the detached-work suite** — TEST(`CASGCTeardownStop.*:CASDetachedWork.*`, `test_t1.log`), then ANALYZE(`test_t1.log`). Expected: all pass.

- [ ] **Step 7: Commit** — COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`, `ca-pool: beginTeardown arms the pool's teardown flag on its own, and the flag is an atomic`)

---

### Task 2: The open plane carries the flag — fence, interruptible sleep, seam restoration {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h:1134-1141` (next to `mountPlaneSleepFn`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:186-200` (the constructor's plane initializers) and `:1883-1893` (`setCasRetrySleepForTest`)
- Test: `src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`

**Interfaces:**
- Consumes: `Pool::beginTeardown`, `Pool::teardownBegun` (Task 1).
- Produces: `gc_requests` refuses every `admit`/`resume` once `teardownBegun()` is true; its sleep returns as soon as the flag is set. `std::function<void(uint64_t)> Pool::openPlaneSleepFn()` (private).

- [ ] **Step 1: Write the failing tests** — append to `gtest_cas_gc_teardown_stop.cpp`:

```cpp
/// The open plane -- GC, FSCK, the probe -- refuses after the arm, before anything reaches the
/// backend; the mount plane, which the ref-lane drain and the farewell need alive, does not.
TEST(CASGCTeardownStop, OpenPlaneRefusesAfterTeardownBeganAndTheMountPlaneDoesNot)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPlainPool(backend);
    {
        CasOperation op = store->openRequests().admit();
        orThrow(op.create("p/probe", "v", Retry::once()), "create");
    }
    backend->resetCounts();

    store->beginTeardown();

    CasOperation refused = store->openRequests().admit();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)refused.read("p/probe", Retry::standard()); });
    CasOperation resumed = store->openRequests().resume(/*admitted_generation=*/0);
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)resumed.read("p/probe", Retry::standard()); });
    EXPECT_EQ(backend->getTotal(), 0u) << "a refused admission never reaches the backend";

    CasOperation mount = store->mountRequests().admit();
    ASSERT_TRUE(mount.read("p/probe", Retry::once()).has_value())
        << "the mount plane is not the open plane: teardown's own drain and farewell run on it";
    EXPECT_EQ(backend->getTotal(), 1u);
}

/// The open plane's sleep is the interruptible one, in production wiring and after the test seam is
/// cleared. Arming FIRST makes this a wiring test: a predicate `wait_for` whose predicate already
/// holds returns without waiting, so a plane still wired to the plain sleep cannot pass. The deadline
/// is the assertion; no sleep orders any thread.
TEST(CASGCTeardownStop, OpenPlaneSleepReturnsAtOnceOnceTeardownBegan)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    store->beginTeardown();

    auto paused = std::async(std::launch::async, [&store] { store->openRequests().pause(60'000); });
    EXPECT_EQ(paused.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the open plane's sleep must observe the arm; a plain sleep holds for the full minute";

    /// Clearing the seam must put the interruptible sleep back, not the engine's plain one.
    store->setCasRetrySleepForTest([](uint64_t) {});
    store->setCasRetrySleepForTest({});
    auto paused_again = std::async(std::launch::async, [&store] { store->openRequests().pause(60'000); });
    EXPECT_EQ(paused_again.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "resetting the retry-sleep seam left the open plane on the plain sleep";
}

/// A read-ahead worker resumes under the plane's generation and is refused at its first gate; the
/// fold learns it at the take site. An unconsumed future has its exception dropped by the read-ahead's
/// destructor, so the test consumes it.
TEST(CASGCTeardownStop, ReadAheadWorkerIsRefusedAndTheTakeSiteSeesIt)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPlainPool(backend);
    {
        CasOperation op = store->openRequests().admit();
        orThrow(op.create("p/k1", "one", Retry::once()), "create");
    }
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 2, /*max_free_threads*/ 2, /*queue_size*/ 0};
    CasOperation op = store->openRequests().admit();
    GcReadAhead reads(op, store->openRequests(), pool, /*concurrency=*/2);

    store->beginTeardown();
    backend->resetCounts();
    reads.hintRead("p/k1");
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)reads.takeRead("p/k1"); });
    EXPECT_EQ(backend->getTotal(), 0u) << "the worker was refused before it reached the backend";
}
```

- [ ] **Step 2: Build and run to see them fail** — BUILD(`build_t2_red.log`), ANALYZE(`build_t2_red.log`) (expected: success — nothing new is referenced), then TEST(`CASGCTeardownStop.*`, `test_t2_red.log`), ANALYZE(`test_t2_red.log`). Expected: the three new tests FAIL — the open plane still admits after the arm (`getTotal` is 1, no throw), and both `pause` futures time out.

- [ ] **Step 3: Add the open plane's sleep** — in `Pool/CasPool.h`, directly after `mountPlaneSleepFn`'s definition (the block ending with `return [this](uint64_t ms) { mount_runtime.sleepInterruptibly(ms); };` and its closing brace) insert:

```cpp
    /// The open plane's inter-attempt sleep: woken by `beginTeardown`, so a retry backing off on the
    /// GC plane cannot hold a teardown for a whole capped backoff. A predicate wait, so the detached
    /// tasks' own completions -- which notify the same variable -- do not cut a sleep short. Named
    /// for the same reason as `mountPlaneSleepFn`: the test seam has to be able to put it back.
    std::function<void(uint64_t)> openPlaneSleepFn()
    {
        return [this](uint64_t ms)
        {
            std::unique_lock lock(detached_work->mutex);
            detached_work->cv.wait_for(lock, std::chrono::milliseconds(ms),
                                       [this] { return detached_work->stopping.load(std::memory_order_acquire); });
        };
    }
```

`detached_work` is declared after the planes, and that is fine: the closure captures `this` and runs only after construction, exactly as `mountPlaneSleepFn` reaches `mount_runtime`.

- [ ] **Step 4: Build the open plane with the teardown fence** — in `Pool/CasPool.cpp`, replace the line `, gc_requests(pool_backend, Fence::open(), config.boot_ms_fn)` with:

```cpp
    /// The open plane's fence is the pool's teardown flag: generation 0 forever, exactly like
    /// `Fence::open`, but `admit` refuses once `beginTeardown` ran. A GC round, an FSCK or a probe in
    /// flight is then refused at its next request instead of running to completion under a disk that
    /// is being torn down. The ref ledger and the farewell live on the other two planes, so
    /// teardown's own I/O never meets this fence. A committed write stays committed: `check_or_throw`
    /// does not turn a landed `gc/state` into a failure, the next request refuses instead.
    , gc_requests(pool_backend, Fence{
          [] { return uint64_t{0}; },
          [this](uint64_t, uint64_t) { return teardownBegun() ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
          [](uint64_t) {}},
          config.boot_ms_fn,
          openPlaneSleepFn())
```

- [ ] **Step 5: Restore the open plane's sleep on seam reset** — in `Pool/CasPool.cpp`, replace the body of `setCasRetrySleepForTest`:

```cpp
void Pool::setCasRetrySleepForTest(std::function<void(uint64_t)> sleep_fn)
{
    /// All three planes, not just the ledger's: a test that replaces the retry sleep must not be left
    /// with a real one on the plane the site under test happens to use.
    farewell_requests.setSleepFnForTest(sleep_fn);
    ref_ledger.setCasRetrySleepForTest(sleep_fn);
    /// `CasRequests` falls back to the engine's plain sleep for an empty argument -- which is neither
    /// the mount plane's nor the open plane's default. Re-install both, so clearing the seam cannot
    /// leave a parked renewal held for a whole capped backoff, or the open plane deaf to a teardown.
    gc_requests.setSleepFnForTest(sleep_fn ? sleep_fn : openPlaneSleepFn());
    mount_requests.setSleepFnForTest(sleep_fn ? std::move(sleep_fn) : mountPlaneSleepFn());
}
```

- [ ] **Step 6: Build** — BUILD(`build_t2.log`), then ANALYZE(`build_t2.log`). Expected: success.

- [ ] **Step 7: Run the suite and its neighbours** — TEST(`CASGCTeardownStop.*:CASDetachedWork.*:CASGCReadAhead.*:CASRequests.*`, `test_t2.log`), then ANALYZE(`test_t2.log`). Expected: all pass.

- [ ] **Step 8: Commit** — COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`, `ca-pool: the open request plane's fence is the teardown flag, and its retry sleep wakes on it`)

---

### Task 3: The streamed body is re-admitted per refill {#task-3}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp` (includes; an anonymous-namespace helper and class; `giveUpReadFenceLost`, `giveUpReadNoBudget` at `:354-366`; `CasOperation::stream`)
- Test: `src/Disks/tests/gtest_cas_requests.cpp`

**Interfaces:**
- Consumes: `Fence::Admit`, `Liveness`, `throwCasTransientUnavailable`, `throwCasWriteRetryLater` (all existing, `CasRequests.h`).
- Produces: `CasOperation::stream` returns a buffer that throws `NETWORK_ERROR` (the code both refusal helpers use) from `next` once the plane's fence or the caller's liveness refuses. No signature changes.

- [ ] **Step 1: Write the failing tests** — append to `src/Disks/tests/gtest_cas_requests.cpp` (after `ForEachListedKeyStopsEarlyAndBudgetsPerPage`):

```cpp
/// The body of a streamed object is read at the consumer's pace, long after the opening attempt
/// returned; the wrapper re-admits it at every refill. The window the open already loaded is served
/// first -- the SDK buffer arrives with pending data -- and the check first fires on advancing past it.
TEST(CASRequests, StreamBodyKeepsThePreloadedWindowAndRefusesOnTheNextRefill)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    std::atomic<bool> torn_down{false};
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t) { return torn_down.load() ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; },
                [](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);
    auto op = requests.admit();
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);   /// the body arrives as "0123", "4567", "89"

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String first(4, '\0');
    body->readStrict(first.data(), 4);
    EXPECT_EQ(first, "0123") << "the window the open already loaded is served, not skipped";

    torn_down.store(true);
    char c;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { body->readStrict(&c, 1); });
    EXPECT_TRUE(body->isCanceled()) << "a refused refill leaves the buffer the consumer holds unusable";
}

TEST(CASRequests, StreamBodyServesEveryWindowThenEof)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    auto op = requests.admit();
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String all;
    readStringUntilEOF(all, *body);
    EXPECT_EQ(all, "0123456789");
    EXPECT_TRUE(body->eof());
    EXPECT_FALSE(op.stream("absent", Retry::once())) << "an absent object is still the open's answer";
}

/// The mount plane's fence can answer `NoBudget`; a body refused for that reason must read like a
/// refused open on the same plane -- the retry-later class, not a tripped fence.
TEST(CASRequests, StreamBodyRefusalKeepsTheNoBudgetMapping)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    std::atomic<bool> out_of_budget{false};
    Fence fence{[] { return uint64_t{0}; },
                [&](uint64_t, uint64_t) { return out_of_budget.load() ? Fence::Admit::NoBudget : Fence::Admit::Ok; },
                [](uint64_t) {}};
    auto requests = makeRequests(backend, clock, fence);
    auto op = requests.admit();
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String first(4, '\0');
    body->readStrict(first.data(), 4);
    out_of_budget.store(true);
    char c;
    try
    {
        body->readStrict(&c, 1);
        FAIL() << "the refill must be refused";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_NE(e.message().find("no lease budget"), String::npos) << e.message();
    }
}

/// The caller's liveness is the second half of admission for the body too, in the gate's order.
TEST(CASRequests, StreamBodyHonoursTheCallersLiveness)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    auto requests = makeRequests(backend, clock);
    std::atomic<bool> alive{true};
    auto op = requests.admit([&] { return alive.load(); });
    orThrow(op.create("k", "0123456789", Retry::once()), "create");
    backend->setStreamChunkForTest(4);

    auto body = op.stream("k", Retry::once());
    ASSERT_TRUE(body);
    String first(4, '\0');
    body->readStrict(first.data(), 4);
    alive.store(false);
    char c;
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { body->readStrict(&c, 1); });
}
```

Add `#include <IO/ReadHelpers.h>` and `#include <atomic>` to the file's includes (for `readStringUntilEOF` and `std::atomic`).

- [ ] **Step 2: Build and run to see them fail** — BUILD(`build_t3_red.log`), ANALYZE(`build_t3_red.log`) (expected: success), TEST(`CASRequests.StreamBody*`, `test_t3_red.log`), ANALYZE(`test_t3_red.log`). Expected: `StreamBodyServesEveryWindowThenEof` passes (today's stream already serves the body); the other three FAIL — the body is never refused.

- [ ] **Step 3: One refusal mapping for request and body** — in `Backend/CasRequests.cpp`, add `#include <IO/ReadBuffer.h>` to the includes. Inside `namespace DB::Cas`, before the first `CasOperation::` member definition, add an anonymous namespace:

```cpp
namespace
{

/// The one mapping from a refused admission to the exception a read-class caller sees. The request
/// gate and the streamed body below both refuse through it, so a body refused mid-transfer reads
/// exactly like an open refused before it: `NoBudget` is the retry-later class, everything else the
/// tripped-fence class. Free, not a member: the body's copy must not reference an operation.
[[noreturn]] void throwReadRefused(Fence::Admit admit, std::string_view verb, const String & subject, std::string_view when)
{
    if (admit == Fence::Admit::NoBudget)
        throwCasWriteRetryLater(fmt::format("{} of '{}': no lease budget {}", verb, subject, when));
    throwCasTransientUnavailable(fmt::format("CAS {} of '{}'", verb, subject),
                                 fmt::format("mount fence tripped {}", when));
}

/// The body of a streamed object, re-admitted at every refill. `Backend::stream` bounds only the
/// open; the SDK reads the body at the consumer's pace, under the storage's ordinary settings, long
/// after the attempt that opened it returned -- so a fold parked in a multi-gigabyte run body would
/// outlive the fence that refused every other request of its operation. The check is the operation's
/// own admission, asked once per SDK buffer, and its refusal is the exception a refused open produces.
///
/// The predicate is held BY VALUE -- the fence's `admit` closure, the admitted generation, the
/// caller's liveness -- never through the operation: `CasOperation::stream` returns a buffer that can
/// outlive the operation object (S3 staging opens its stream under a local mount-plane operation and
/// hands the buffer to the backend). Modelled on `LimitReadBuffer`: no byte is copied, the SDK's
/// window is exposed as this buffer's own.
class AdmittedBodyReadBuffer : public ReadBuffer
{
public:
    AdmittedBodyReadBuffer(std::unique_ptr<ReadBuffer> in_, String key_,
                           std::function<Fence::Admit(uint64_t, uint64_t)> admit_,
                           uint64_t admitted_generation_, Liveness liveness_)
        /// The open already loaded a window (`Backend::stream` forces the first GET): adopt it, so
        /// the first refill this buffer asks for is the SECOND window and the SDK buffer is never
        /// asked to advance over pending data.
        : ReadBuffer(in_->position(), in_->available(), 0)
        , in(std::move(in_))
        , key(std::move(key_))
        , admit(std::move(admit_))
        , admitted_generation(admitted_generation_)
        , liveness(std::move(liveness_))
    {
    }

private:
    bool nextImpl() override
    {
        /// Let the SDK buffer account the bytes the consumer took from the shared window.
        in->position() = position();

        Fence::Admit verdict = admit(admitted_generation, 0);
        if (verdict == Fence::Admit::Ok && liveness && !liveness())
            verdict = Fence::Admit::LostOrRearmed;
        if (verdict != Fence::Admit::Ok)
            throwReadRefused(verdict, "stream body", key, "mid-body");

        if (!in->next())
        {
            BufferBase::set(in->position(), 0, 0);
            return false;
        }
        BufferBase::set(in->position(), in->available(), 0);
        return true;
    }

    std::unique_ptr<ReadBuffer> in;
    String key;
    std::function<Fence::Admit(uint64_t, uint64_t)> admit;
    uint64_t admitted_generation;
    Liveness liveness;
};

}
```

- [ ] **Step 4: Route the request gate's refusals through the same mapping** — replace `giveUpReadFenceLost` and `giveUpReadNoBudget`:

```cpp
void CasOperation::giveUpReadFenceLost(std::string_view verb, const String & subject, std::string_view when)
{
    last_read_stop = ReadStop::FenceLost;
    throwReadRefused(Fence::Admit::LostOrRearmed, verb, subject, when);
}

void CasOperation::giveUpReadNoBudget(std::string_view verb, const String & subject, std::string_view what)
{
    last_read_stop = ReadStop::NoBudgetLease;
    throwReadRefused(Fence::Admit::NoBudget, verb, subject, what);
}
```

The message texts are unchanged from what these two functions produce today.

- [ ] **Step 5: Wrap the body in `stream`** — replace `CasOperation::stream`:

```cpp
std::unique_ptr<ReadBuffer> CasOperation::stream(const String & key, const Retry & policy)
{
    const Retry::Bound bound = policy.bind(owner.now_ms());
    std::unique_ptr<ReadBuffer> body = readLoop("stream", key, policy, bound, [&](auto & access)
    {
        return owner.backend->stream(key, access);
    });
    if (!body)
        return nullptr;   /// absent: the open already answered
    /// By value, deliberately: nothing the buffer holds may reference this operation or its owner.
    return std::make_unique<AdmittedBodyReadBuffer>(std::move(body), key, owner.fence.admit, admitted_generation, liveness);
}
```

- [ ] **Step 6: Build** — BUILD(`build_t3.log`), then ANALYZE(`build_t3.log`). Expected: success.

- [ ] **Step 7: Run the engine suite and every suite that streams** — TEST(`CASRequests.*:CASGCTeardownStop.*:CASGCRoundDefer.*:CASFsck*:CASGCReadAhead.*`, `test_t3.log`), then ANALYZE(`test_t3.log`). Expected: all pass. (If a suite name in this filter matches nothing, that is fine; the gate in Task 9 runs everything.)

- [ ] **Step 8: Commit** — COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp src/Disks/tests/gtest_cas_requests.cpp`, `ca-engine: a streamed body is re-admitted at every refill, refusing the way a refused open does`)

---

### Task 4: `Stopped` — the outcome, the rule, the row, the scheduler's hygiene {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h:24-34` (the `Outcome` enum and its comment)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp` — `runRoundLogged`'s `catch` (`:266-277`), `loop` after `std::lock_guard round_lock(gc_round_mutex);` (`:335`) and its `catch` (`:373-391`), `heartbeatLoop`'s `catch`
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.h:15-21`, `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:21-24`, `:41`, `:54-55`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:525-527` (the outcome mapping)
- Test: `src/Disks/tests/gtest_cas_gc_log.cpp`, `src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`

**Interfaces:**
- Consumes: `Pool::teardownBegun` (Task 1), the open plane's refusal (Task 2).
- Produces: `GcRoundLogRecord::Outcome::Stopped`; `ContentAddressedGarbageCollectionLogElement::STOPPED = 7`; the rule `Failed` if non-transient, else `Stopped` if `teardownBegun()`, else `Aborted`. Task 5's tests assert `Outcome::Stopped`.

- [ ] **Step 1: Write the failing classification tests** — append to `src/Disks/tests/gtest_cas_gc_log.cpp` (after `TransientErrorClassifierFailsClosed`):

```cpp
/// A backend that arms the pool's teardown the moment a chosen key has been read -- after the read
/// returned, before the round can act on it -- so the arm lands mid-round at a known point.
class ArmAfterReadBackend : public InMemoryBackend
{
public:
    using InMemoryBackend::read;

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        auto result = InMemoryBackend::read(key, access);
        if (key == arm_key && on_read)
            on_read();
        return result;
    }

    String arm_key;
    std::function<void()> on_read;
};

/// `Stopped` is a transient failure observed after the pool's teardown began -- a correlation the row
/// records honestly. The arm lands right after the lease read; the round's next request is refused by
/// the open plane's fence, which the engine reports like any lost fence (a transient code).
TEST(CASGCLog, TransientFailureAfterTeardownBeganIsStopped)
{
    auto backend = std::make_shared<ArmAfterReadBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    store->setCasRetrySleepForTest([](uint64_t) {});
    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(
        store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const Rec & r) { rows.push_back(r); });

    backend->arm_key = store->layout().gcStateKey();
    backend->on_read = [&store] { store->beginTeardown(); };
    EXPECT_THROW(sched.runOneRoundNow(Rec::Trigger::Manual), DB::Exception);

    const std::vector<Rec> round_rows = roundRowsOnly(rows);
    ASSERT_EQ(round_rows.size(), 2u);
    EXPECT_EQ(round_rows[1].event_type, Rec::EventType::Finish);
    EXPECT_EQ(round_rows[1].outcome, Rec::Outcome::Stopped)
        << "a transient refusal after the arm is the teardown cutting the round short, not an incident";
    EXPECT_EQ(round_rows[1].error_code, DB::ErrorCodes::NETWORK_ERROR);
    EXPECT_FALSE(round_rows[1].error.empty());
}

/// The rule is fail-closed: a non-transient failure that coincides with the arm stays `Failed`. An
/// undecodable `gc/state` throws `CORRUPTED_DATA` out of the lease phase after the very read that arms.
TEST(CASGCLog, NonTransientFailureCoincidingWithTeardownStaysFailed)
{
    auto backend = std::make_shared<ArmAfterReadBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    store->setCasRetrySleepForTest([](uint64_t) {});
    std::vector<Rec> rows;
    DB::Cas::CasGcScheduler sched(
        store, std::chrono::seconds(1), "test::gc", "ca",
        [&](const Rec & r) { rows.push_back(r); });

    {
        DB::Cas::tests::OperationForTest raw_op(*backend);
        const auto current = (*raw_op).read(store->layout().gcStateKey(), Retry::once());
        ASSERT_TRUE(current.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>(
            (*raw_op).replace(store->layout().gcStateKey(), "not a gc state", current->etag, Retry::once())));
    }
    backend->arm_key = store->layout().gcStateKey();
    backend->on_read = [&store] { store->beginTeardown(); };
    EXPECT_THROW(sched.runOneRoundNow(Rec::Trigger::Manual), DB::Exception);

    const std::vector<Rec> round_rows = roundRowsOnly(rows);
    ASSERT_EQ(round_rows.size(), 2u);
    EXPECT_EQ(round_rows[1].outcome, Rec::Outcome::Failed)
        << "a bug that coincides with a restart is not masked as Stopped";
    EXPECT_EQ(round_rows[1].error_code, DB::ErrorCodes::CORRUPTED_DATA);
}
```

Add `extern const int CORRUPTED_DATA;` to the file's `DB::ErrorCodes` block and `#include <functional>` to its includes.

- [ ] **Step 2: Build and confirm the tests do not compile** — BUILD(`build_t4_red.log`), then ANALYZE(`build_t4_red.log`). Expected: FAIL — `Outcome` has no enumerator `Stopped`.

- [ ] **Step 3: The outcome** — in `Gc/CasGcScheduler.h`, replace the `Outcome` enum and extend its comment:

```cpp
    /// `Aborted`: the round threw an exception whose code names a transient condition (backend
    /// unavailability, a lost lease, a concurrent leader) -- the next scheduled round retries it and
    /// nothing durable is wrong. `Stopped`: the same transient class, observed after the pool's
    /// teardown began -- a correlation, not a cause: the engine reports a refused teardown fence
    /// exactly like a lost mount fence, so the flag is the only witness, and the row says so honestly
    /// rather than reading a clean restart as a backend incident. `Failed` is reserved for everything
    /// else (a logic error, corrupted data, an unclassified code): fail-closed, an unrecognised
    /// failure reads as real -- during a teardown too.
    enum class Outcome { Unknown, Success, NotALeader, Failed, Deferred, Aborted, Stopped };
```

- [ ] **Step 4: The rule** — in `Gc/CasGcScheduler.cpp`, `runRoundLogged`'s `catch (...)`, replace the line assigning `fin.outcome`:

```cpp
        fin.error_code = getCurrentExceptionCode();
        /// Non-transient first, so a bug that coincides with a restart is never masked; then the
        /// teardown flag, the only witness of a refused teardown fence (see `Outcome::Stopped`).
        fin.outcome = !isTransientGcRoundError(fin.error_code) ? Rec::Outcome::Failed
                    : store->teardownBegun()                    ? Rec::Outcome::Stopped
                                                                : Rec::Outcome::Aborted;
```

- [ ] **Step 5: The loop** — in `loop`, directly after `std::lock_guard round_lock(gc_round_mutex);` insert:

```cpp
            /// A round that starts after the pool's teardown began would emit a Start row and be
            /// refused at its first lease request -- a row that says nothing. Checked here, under the
            /// round mutex, so it also covers the tick queued behind a manual round; the extra round
            /// on a plain `stop` (above) is a different race and stays as described.
            if (store->teardownBegun())
                return;
```

In the same function's `catch (...)`, insert at the top of the block, before the existing comment:

```cpp
            if (store->teardownBegun() && isTransientGcRoundError(getCurrentExceptionCode()))
            {
                /// The disk is being torn down and the round was cut at its next request: expected,
                /// recorded as `Stopped` by `runRoundLogged`, not an error to raise.
                LOG_INFO(log, "CA GC round stopped by the disk's teardown: {}", getCurrentExceptionMessage(false));
                continue;
            }
```

- [ ] **Step 6: The heartbeat** — in `heartbeatLoop`, replace the `catch (...)` block:

```cpp
        catch (...)
        {
            /// A pulse refused by the open plane during teardown is the expected end of this loop,
            /// not a failure to report; `stop` joins it moments later.
            if (store->teardownBegun())
                return;
            tryLogCurrentException(log, "CA GC heartbeat pulse failed (advisory; will retry)");
        }
```

- [ ] **Step 7: The log element and the table** — in `src/Interpreters/ContentAddressedGarbageCollectionLog.h`, replace the `Outcome` enum line and extend the comment above it:

```cpp
    /// `ABORTED`: the round threw an exception whose code names a transient condition (backend
    /// unavailability, a lost lease, a concurrent leader); the next scheduled round retries it.
    /// `STOPPED`: a transient failure observed after the disk's teardown began -- the round was cut
    /// short by a shutdown, FORGET or the storage's destructor; `error` carries the engine's refusal.
    /// `FAILED` is everything else -- fail-closed, an unclassified error reads as real.
    enum Outcome   : int8_t { UNKNOWN = 1, SUCCESS = 2, NOT_A_LEADER = 3, FAILED = 4, DEFERRED = 5, ABORTED = 6, STOPPED = 7 };
```

In `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`, replace the `outcome_enum` values so the list ends with `{"Aborted", static_cast<Int8>(ABORTED)}, {"Stopped", static_cast<Int8>(STOPPED)}});`, and replace the three column comments:

```cpp
        {"outcome", outcome_enum, "Unknown (Start) / Success (led, folded, and completed) / NotALeader (another replica holds the GC lease) / Deferred (led but took the skip-unchanged fast path -- no fold ran) / Aborted (the round threw a transient error -- backend unavailability, a lost lease, a concurrent leader -- and the next scheduled round retries) / Stopped (a transient error observed after the disk's teardown began: the round was cut short by a shutdown, FORGET or the storage's destructor; a correlation, not a cause -- a transient incident that started before the teardown is recorded the same way) / Error (the round threw a non-transient error -- during a teardown too)."},
```

```cpp
        {"error", std::make_shared<DataTypeString>(), "Exception text when outcome = Aborted, Stopped or Error. On a Stopped row it names the engine's refusal, not the teardown."},
        {"error_code", std::make_shared<DataTypeInt32>(), "Exception code when outcome = Aborted, Stopped or Error; 0 otherwise. The structured twin of `error`: key monitoring on this column, not on message text."},
```

In `ContentAddressedMetadataStorage.cpp`, in the `switch (r.outcome)` of `makeGcRoundLogger`, add after the `Aborted` case:

```cpp
            case Cas::GcRoundLogRecord::Outcome::Stopped:
                e.outcome = ContentAddressedGarbageCollectionLogElement::STOPPED;
                break;
```

- [ ] **Step 8: Build** — BUILD(`build_t4.log`), then ANALYZE(`build_t4.log`). Expected: success (a `switch` over `Outcome` without the new case would be a `-Wswitch` error; the mapping above is the only such switch).

- [ ] **Step 9: Run** — TEST(`CASGCLog.*:CASGCTeardownStop.*:CASGCStopStart*:CASGCSchedulerSteal.*`, `test_t4.log`), then ANALYZE(`test_t4.log`). Expected: all pass, the two new tests included.

- [ ] **Step 10: Commit** — COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp src/Interpreters/ContentAddressedGarbageCollectionLog.h src/Interpreters/ContentAddressedGarbageCollectionLog.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_cas_gc_log.cpp`, `ca-gc: a round cut by the disk's teardown is Stopped, and no round starts after it`)

---

### Task 5: The four arms — `shutdown`, the destructor path, `FORGET`, `Pool::forgetDisk` {#task-5}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (a test seam next to `setGcVerbAdmitWindowHookForTest` at `:550`, its backing member next to `gc_verb_admit_window_hook_for_test` at `:793`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` — `makeGcRoundLogger` (`:466-`), `shutdown` (`:899-905`), `stopAndDrainForTeardown` (`:908-967`), `forgetDisk` (`:985-1027`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp` — `Pool::forgetDisk` (`:1069-`)
- Test: `src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`

**Interfaces:**
- Consumes: `Pool::beginTeardown`, `Pool::teardownBegun` (Task 1); `Outcome::Stopped` (Task 4).
- Produces: `void ContentAddressedMetadataStorage::setGcRoundRowHookForTest(std::function<void(const Cas::GcRoundLogRecord &)>)` — the storage's round logger calls it for every row before (and regardless of) the system-log path; with a null `Context` it is the whole logger.

- [ ] **Step 1: Add the row-hook seam** — in `ContentAddressedMetadataStorage.h`, after the `setGcVerbAdmitWindowHookForTest` declaration insert:

```cpp
    /// Test-only: sees every round-log row the storage's own scheduler emits (Start, Phase, Finish),
    /// before and independently of the system-log path -- which a unit-test storage (null `Context`)
    /// does not have at all. Lets a test park a synchronous round on a phase row while it holds
    /// `gc_scheduler_mutex`, and read the round's outcome afterwards. Set before the first round.
    void setGcRoundRowHookForTest(std::function<void(const Cas::GcRoundLogRecord &)> fn)
    {
        gc_round_row_hook_for_test = std::move(fn);
    }
```

and next to `gc_verb_admit_window_hook_for_test`'s member declaration insert:

```cpp
    std::function<void(const Cas::GcRoundLogRecord &)> gc_round_row_hook_for_test;
```

In `ContentAddressedMetadataStorage.cpp`, replace the beginning of `makeGcRoundLogger` so the hook is composed with the existing sink:

```cpp
Cas::GcRoundLogger ContentAddressedMetadataStorage::makeGcRoundLogger() const
{
    /// Unit tests pass a null context (no system logs); the scheduler then runs with the test hook
    /// as its only sink, or without a sink.
    const std::function<void(const Cas::GcRoundLogRecord &)> hook = gc_round_row_hook_for_test;
    if (!context)
        return hook ? Cas::GcRoundLogger(hook) : Cas::GcRoundLogger{};
    const ContextWeakPtr weak_context = *context;
    /// The configured disk name (threaded from the metadata-storage factory); falls back to
    /// storage_path_prefix for callers that don't supply one (e.g. unit tests).
    const String disk = disk_name;
    return [weak_context, disk, hook](const Cas::GcRoundLogRecord & r)
    {
        if (hook)
            hook(r);
        auto ctx = weak_context.lock();
```

(the rest of the lambda is unchanged).

- [ ] **Step 2: Write the failing storage-level test** — append to `gtest_cas_gc_teardown_stop.cpp`. First the helpers, inside the file's anonymous namespace (after `openPlainPool`):

```cpp
/// A gate a test opens explicitly, so a thread can be held in flight without a sleep. Bounded, and it
/// names what it waited on: an unbounded wait on a premise that stopped holding hangs the binary.
struct Gate
{
    void wait(std::string_view name)
    {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, std::chrono::seconds(60), [this] { return open_; }))
            ADD_FAILURE() << "timed out waiting for '" << name << "'";
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

/// Opens its gate on every exit from the scope, so a failing assertion cannot strand the thread
/// parked behind it.
struct GateOpenedOnExit
{
    explicit GateOpenedOnExit(Gate & gate_) : gate(gate_) {}
    GateOpenedOnExit(const GateOpenedOnExit &) = delete;
    GateOpenedOnExit & operator=(const GateOpenedOnExit &) = delete;
    ~GateOpenedOnExit() { gate.open(); }
    Gate & gate;
};

/// A real storage over a fresh local object storage, `context == nullptr`: no system log, no
/// scheduler until the first GC entry point creates one.
std::shared_ptr<DB::ContentAddressedMetadataStorage> openTestStorage()
{
    static std::atomic<uint64_t> counter{0};
    const auto scratch = std::filesystem::temp_directory_path()
        / ("cas_gc_teardown_stop_scratch_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)));
    auto settings = DB::Cas::tests::makeSettingsForTest("test", scratch);
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

/// Completes the first read of `key`, then withholds its return until released, so the round that
/// issued it is parked with that request already accounted at the backend.
class ParkFirstReadBackend : public CountingBackend
{
public:
    void armParkFirstRead(String key_, std::shared_ptr<Gate> entered_, std::shared_ptr<Gate> release_)
    {
        key = std::move(key_);
        entered = std::move(entered_);
        release = std::move(release_);
        armed.store(true);
    }

    std::optional<Raw> read(const String & read_key, TransportAccess & access) override
    {
        auto result = CountingBackend::read(read_key, access);
        if (read_key == key && armed.exchange(false))
        {
            entered->open();
            release->wait("release");
        }
        return result;
    }

    uint64_t requestsTotal() const { return getTotal() + headTotal() + listTotal() + writeTotal(); }

private:
    String key;
    std::shared_ptr<Gate> entered;
    std::shared_ptr<Gate> release;
    std::atomic<bool> armed{false};
};

/// A thread-safe sink for the scheduler's rows, with a wait that never sleeps.
class RoundLogSink
{
public:
    GcRoundLogger logger()
    {
        return [this](const GcRoundLogRecord & r)
        {
            std::lock_guard lock(mutex);
            records.push_back(r);
            cv.notify_all();
        };
    }

    std::vector<GcRoundLogRecord> all()
    {
        std::lock_guard lock(mutex);
        return records;
    }

    /// The first Finish row at index >= `from`, waiting up to `timeout`; nullopt on timeout.
    std::optional<GcRoundLogRecord> waitForFinish(size_t from, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex);
        const auto is_finish = [&]
        {
            for (size_t i = from; i < records.size(); ++i)
                if (records[i].event_type == GcRoundLogRecord::EventType::Finish)
                    return true;
            return false;
        };
        if (!cv.wait_for(lock, timeout, is_finish))
            return std::nullopt;
        for (size_t i = from; i < records.size(); ++i)
            if (records[i].event_type == GcRoundLogRecord::EventType::Finish)
                return records[i];
        return std::nullopt;
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<GcRoundLogRecord> records;
};

size_t countStarts(const std::vector<GcRoundLogRecord> & rows)
{
    size_t n = 0;
    for (const auto & r : rows)
        if (r.event_type == GcRoundLogRecord::EventType::Start)
            ++n;
    return n;
}
```

Add `#include <unistd.h>` and `#include <optional>` to the includes. Then the test:

```cpp
/// The defect itself: `shutdown` waits behind `gc_scheduler_mutex`, which a synchronous round holds
/// for its whole duration. After the fix it arms the pool first, the parked round is refused at its
/// next request, and `shutdown` returns. On the old code the arm never lands while the round is
/// parked, which is the assertion that goes red.
TEST(CASGCTeardownStop, ShutdownReturnsWhileASynchronousRoundIsParked)
{
    auto storage = openTestStorage();
    auto pool = storage->poolForTest();
    ASSERT_TRUE(pool);

    auto parked = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    GateOpenedOnExit opener(*release);
    std::mutex rows_mutex;
    std::vector<GcRoundLogRecord> rows;
    storage->setGcRoundRowHookForTest([&](const GcRoundLogRecord & r)
    {
        {
            std::lock_guard lock(rows_mutex);
            rows.push_back(r);
        }
        /// Park on the `lease` phase row: the round holds `gc_scheduler_mutex` and has more
        /// requests ahead of it.
        if (r.event_type == GcRoundLogRecord::EventType::Phase && r.phase == "lease")
        {
            parked->open();
            release->wait("release");
        }
    });

    auto round = std::async(std::launch::async, [&storage] { storage->runOneGcRoundForTest(); });
    parked->wait("parked");

    auto done = std::async(std::launch::async, [&storage] { storage->shutdown(); });

    /// The state handshake: the arm must land while the round is still parked. Bounded, and its
    /// expiry is the failure on the old code.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!pool->teardownBegun() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    EXPECT_TRUE(pool->teardownBegun()) << "shutdown waited for the round instead of arming the pool first";

    release->open();
    EXPECT_THROW(round.get(), DB::Exception) << "the released round must be refused at its next request";
    EXPECT_EQ(done.wait_for(std::chrono::seconds(30)), std::future_status::ready);

    std::optional<GcRoundLogRecord> finish;
    {
        std::lock_guard lock(rows_mutex);
        for (const auto & r : rows)
            if (r.event_type == GcRoundLogRecord::EventType::Finish)
                finish = r;
    }
    ASSERT_TRUE(finish.has_value());
    EXPECT_EQ(finish->outcome, GcRoundLogRecord::Outcome::Stopped);
}
```

- [ ] **Step 3: Build and run to see it fail** — BUILD(`build_t5_red.log`), ANALYZE(`build_t5_red.log`) (expected: success), TEST(`CASGCTeardownStop.ShutdownReturnsWhileASynchronousRoundIsParked`, `test_t5_red.log`), ANALYZE(`test_t5_red.log`). Expected: FAIL at "shutdown waited for the round instead of arming the pool first"; after the gate opens the round finishes with `Success` and the second expectation fails too. The binary does not hang.

- [ ] **Step 4: Arm in `shutdown`** — replace `ContentAddressedMetadataStorage::shutdown`:

```cpp
void ContentAddressedMetadataStorage::shutdown()
{
    /// Arm the pool BEFORE waiting for `gc_scheduler_mutex`: a synchronous round holds that mutex
    /// for its whole duration and releases it only once its next request is refused. The arm frees,
    /// nulls and swaps nothing -- every pointer swap still happens below, under the same locks as
    /// before -- so taking `pointer_mutex` alone here, before the outer lock, inverts no order.
    Cas::PoolPtr pool;
    {
        std::lock_guard ptr_lock(pointer_mutex);
        pool = cas_store;
    }
    if (pool)
        pool->beginTeardown();
    std::lock_guard round_lock(gc_scheduler_mutex);
    shutdown_called = true;
    stopAndDrainForTeardown();
}
```

Also replace the comment on `gc_scheduler_mutex` in the header (`:654-658`): change the sentence `(clean GC completion has priority over fast shutdown)` to `(a round in flight is refused at its next request once the pool is armed, so the wait is one request long)`.

- [ ] **Step 5: Arm before the scheduler join** — in `stopAndDrainForTeardown`, directly before the line `guarded([&] { if (old_scheduler) old_scheduler->stop(); }, "CAS storage teardown: stopping GC");` insert:

```cpp
    /// The destructor reaches here without `shutdown`'s arm: arm now, before the join, so a
    /// background round is refused at its next request rather than joined at its end.
    if (old_pool)
        old_pool->beginTeardown();
```

- [ ] **Step 6: Arm in `FORGET`** — in `ContentAddressedMetadataStorage::forgetDisk`, before `std::lock_guard lifecycle(lifecycle_mutex);` insert:

```cpp
    /// Same order as `shutdown`: arm before the locks a round can hold, so the round is refused at its
    /// next request instead of being waited out. The arm is idempotent; the pool's own teardown
    /// below arms again, harmlessly.
    {
        Cas::PoolPtr armed;
        {
            std::lock_guard lock(pointer_mutex);
            armed = cas_store;
        }
        if (armed)
            armed->beginTeardown();
    }
```

- [ ] **Step 7: Arm in `Pool::forgetDisk`** — in `Pool/CasPool.cpp`, in `Pool::forgetDisk`, directly before the comment `/// (1) Publish the terminal-intent latch FIRST` insert:

```cpp
    /// (0) Arm the open plane first: the injected GC join below waits for a round that is refused at
    /// its next request once this is set, and never for one that runs to completion. A pool torn
    /// down without a storage above it (tests, the offline tools) gets the same bound.
    beginTeardown();
```

- [ ] **Step 8: Build** — BUILD(`build_t5.log`), then ANALYZE(`build_t5.log`). Expected: success.

- [ ] **Step 9: Run the shutdown test green** — TEST(`CASGCTeardownStop.*:CASDetachedWork.*:CASForget*:CASGCStopStart*`, `test_t5.log`), then ANALYZE(`test_t5.log`). Expected: all pass.

- [ ] **Step 10: Write the round-level tests** — append to `gtest_cas_gc_teardown_stop.cpp`:

```cpp
/// A background round parked inside a request is refused at its NEXT request: nothing new reaches
/// the backend after the arm, the Finish row is `Stopped`, and `stop` returns with nothing in flight.
TEST(CASGCTeardownStop, BackgroundRoundIsCutAtItsNextRequest)
{
    auto backend = std::make_shared<ParkFirstReadBackend>();
    auto store = openPlainPool(backend);
    RoundLogSink sink;
    CasGcScheduler sched(store, std::chrono::seconds(1), "test::gc", "ca", sink.logger());

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    GateOpenedOnExit opener(*release);
    backend->armParkFirstRead(store->layout().gcStateKey(), entered, release);
    sched.start();
    entered->wait("entered");

    store->beginTeardown();
    const uint64_t requests_at_arm = backend->requestsTotal();
    release->open();

    const auto finish = sink.waitForFinish(/*from=*/0, std::chrono::seconds(30));
    ASSERT_TRUE(finish.has_value()) << "the parked round never finished";
    EXPECT_EQ(finish->outcome, GcRoundLogRecord::Outcome::Stopped);
    sched.stop();
    EXPECT_TRUE(sched.isQuiescent());
    EXPECT_EQ(backend->requestsTotal(), requests_at_arm)
        << "after the arm no request may reach the backend: the round unwinds at the next gate";
    EXPECT_EQ(countStarts(sink.all()), 1u) << "no further round started after the arm";
}

/// The tick queued behind a manual round must not mint a Start row after the arm: it checks the
/// flag once it holds the round mutex, before it logs anything.
TEST(CASGCTeardownStop, AQueuedScheduledTickEmitsNoStartAfterTeardownBegan)
{
    auto backend = std::make_shared<ParkFirstReadBackend>();
    auto store = openPlainPool(backend);
    RoundLogSink sink;
    /// An hour-long interval: the loop ticks only when asked.
    CasGcScheduler sched(store, std::chrono::seconds(3600), "test::gc", "ca", sink.logger());
    sched.start();

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    GateOpenedOnExit opener(*release);
    backend->armParkFirstRead(store->layout().gcStateKey(), entered, release);
    auto manual = std::async(std::launch::async, [&sched] { return sched.runOneRoundNow(GcRoundLogRecord::Trigger::Manual); });
    entered->wait("entered");

    /// The loop wakes and queues on the round mutex behind the parked manual round.
    sched.requestRoundSoon();
    store->beginTeardown();
    release->open();

    EXPECT_THROW(manual.get(), DB::Exception);
    sched.stop();
    const auto rows = sink.all();
    EXPECT_EQ(countStarts(rows), 1u) << "the queued tick minted a Start row after teardown began";
}
```

- [ ] **Step 11: Write the janitor test** — append:

```cpp
/// A stop that lands inside advisory work the round swallows is not `Stopped`: the deferred path
/// runs the namespace janitor's page and returns normally, and the janitor turns a refused request
/// into an anomaly. The row is `Deferred`; the round did finish. Pinned so a later change to this
/// behaviour is made on purpose.
class ArmOnJanitorListBackend : public CountingBackend
{
public:
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        auto page = CountingBackend::list(prefix, cursor, limit, access);
        if (!arm_prefix.empty() && prefix == arm_prefix && on_list)
        {
            on_list();
            on_list = {};
        }
        return page;
    }
    String arm_prefix;
    std::function<void()> on_list;
};

TEST(CASGCTeardownStop, AStopInsideTheJanitorPageIsSwallowedAsDeferred)
{
    auto backend = std::make_shared<ArmOnJanitorListBackend>();
    auto store = DB::Cas::tests::openPoolForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    DB::Cas::tests::writeBlobBody(*backend, store->layout(), UInt128(1));
    DB::Cas::tests::writeManifestRaw(*backend, store->layout(), ns, r, {DB::Cas::tests::blobEntryFor("a", UInt128(1))});
    DB::Cas::tests::publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);

    Gc gc(store, UInt128(0xAB));
    const RoundReport fold_rep = gc.runRegularRound();
    ASSERT_FALSE(fold_rep.deferred) << "the first round folds";

    backend->arm_prefix = store->layout().namespaceRootPrefix();
    backend->on_list = [&store] { store->beginTeardown(); };
    RoundReport rep;
    EXPECT_NO_THROW(rep = gc.runRegularRound()) << "the janitor page swallows the refusal";
    EXPECT_TRUE(store->teardownBegun()) << "sanity: the arm landed inside the round";
    EXPECT_TRUE(rep.deferred) << "an idle second round defers; the stop inside its janitor page is advisory";
}
```

The helpers `writeBlobBody`, `writeManifestRaw`, `blobEntryFor`, `publishCommittedTransition` and `openPoolForTest` are the ones `gtest_cas_gc_round_defer.cpp`'s `IdleRoundDefersAndReadsNoGeneration` uses; copy their `using` lines from that file if they are not reachable through `DB::Cas::tests::`.

- [ ] **Step 12: Build and run** — BUILD(`build_t5b.log`), ANALYZE(`build_t5b.log`); TEST(`CASGCTeardownStop.*`, `test_t5b.log`), ANALYZE(`test_t5b.log`). Expected: all pass. If `AQueuedScheduledTickEmitsNoStartAfterTeardownBegan` reports two Start rows, the post-lock check of Task 4 is missing or placed after the Start row — fix that, not the test.

- [ ] **Step 13: Commit** — COMMIT(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/tests/gtest_cas_gc_teardown_stop.cpp`, `ca-mount: every teardown arms the pool before the lock or join it would otherwise wait behind`)

---

### Task 6: The full unit gate {#task-6}

**Files:** none edited.

- [ ] **Step 1: Build** — BUILD(`build_t6_gate.log`), then ANALYZE(`build_t6_gate.log`). Expected: success.

- [ ] **Step 2: The whole CAS gate** — TEST(`CAS*`, `test_t6_gate.log`), then ANALYZE(`test_t6_gate.log`). Expected: every test passes; the count of tests run is at least the Task 0 baseline plus the fourteen added by this plan. Any red is a root cause to fix in the task that owns the code, followed by a re-run of this task; never a filter narrowed to exclude it.

---

### Task 7: Integration — a restart that provably cut a long round short {#task-7}

**Files:**
- Modify: `tests/integration/test_cas_gc_s3/test.py` (append one test; the module fixture is reused)

**Interfaces:**
- Consumes: the disk `disk_cas_gc_s3` (`configs/storage_conf.xml`), `cas_gc_interval_sec=1`, `system.cas_gc_log` with `outcome = 'Stopped'` (Task 4), `SYSTEM CAS GC STOP <disk>`.

- [ ] **Step 1: Write the test** — append to `tests/integration/test_cas_gc_s3/test.py`:

```python
GC_DISK = "disk_cas_gc_s3"

# One insert produces this many parts: the fold's work grows with the manifest count, and that is what
# makes one round long enough to be cut. The calibration below asserts the round is long, not this
# number; raise it if the assertion trips on a fast host.
RESTART_PARTS_PER_INSERT = 200
RESTART_INSERTS = 32
# A round must last at least this long for a restart in its first half to land inside it.
MIN_CALIBRATED_ROUND_MS = 4000
# What the teardown adds on top of the paused baseline: one control-plane attempt plus slack.
ATTEMPT_TIMEOUT_S = 5.0
STOP_SLACK_S = 5.0


def _stop_seconds(node):
    """The stop phase alone, timed; startup variation is kept out of the measurement."""
    t0 = time.monotonic()
    node.stop_clickhouse(stop_wait_sec=600)
    return time.monotonic() - t0


def _gc_rows(node, where):
    node.query("SYSTEM FLUSH LOGS")
    return node.query(
        "SELECT round_id, event_type, outcome, duration_ms, event_time_microseconds "
        "FROM system.cas_gc_log WHERE disk_name = '{}' AND {} "
        "ORDER BY event_time_microseconds FORMAT TSV".format(GC_DISK, where)
    ).strip().splitlines()


def _wait_round_in_flight(node, timeout_s=120):
    """The round_id of a Start row that has no Finish yet, or None if none appeared in time."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        starts = {r.split("\t")[0] for r in _gc_rows(node, "event_type = 'Start'")}
        finishes = {r.split("\t")[0] for r in _gc_rows(node, "event_type = 'Finish'")}
        open_rounds = starts - finishes
        if open_rounds:
            return next(iter(open_rounds))
        time.sleep(0.2)
    return None


def test_restart_under_gc_cuts_the_round_short():
    """
    A restart must not wait out a GC round. The paused baseline is measured first, one round is
    calibrated, and the under-GC restart is taken inside the first half of a round that is provably
    in flight; afterwards that round's own Finish row must be Stopped -- the witness that the round
    was cut, without which the timing bound would be an upper bound a restart between rounds
    satisfies trivially.
    """
    node = cluster.instances["node"]
    node.query("DROP TABLE IF EXISTS cas_gc_restart SYNC")

    # (1) Pause GC while the pool is small, so this stop is short by construction.
    node.query("SYSTEM CAS GC STOP {}".format(GC_DISK))

    # (2) Populate, GC paused: many small parts, so one round has real work.
    node.query(
        "CREATE TABLE cas_gc_restart (id Int64, data String) ENGINE = MergeTree() ORDER BY id "
        "SETTINGS storage_policy = '{}'".format(STORAGE_POLICY)
    )
    for i in range(RESTART_INSERTS):
        node.query(
            "INSERT INTO cas_gc_restart SELECT number + {offset}, toString(number) "
            "FROM numbers({rows}) SETTINGS max_insert_block_size = 1000, min_insert_block_size_rows = 1000, "
            "min_insert_block_size_bytes = 0".format(offset=i * 1000 * RESTART_PARTS_PER_INSERT,
                                                     rows=1000 * RESTART_PARTS_PER_INSERT)
        )
    parts = int(node.query("SELECT count() FROM system.parts WHERE table = 'cas_gc_restart' AND active"))
    assert parts >= RESTART_INSERTS * RESTART_PARTS_PER_INSERT // 2, "expected many small parts, got {}".format(parts)

    # (3) The paused baseline: the stop phase with no round in flight.
    paused_stop_s = _stop_seconds(node)
    node.start_clickhouse()

    # (4) GC restarts with the server. Calibrate one full round.
    first = None
    deadline = time.monotonic() + 300
    while time.monotonic() < deadline and first is None:
        finished = _gc_rows(node, "event_type = 'Finish' AND outcome IN ('Success', 'Deferred')")
        if finished:
            first = finished[-1]
        else:
            time.sleep(0.5)
    assert first is not None, "no GC round finished within 300 s after the restart"
    round_ms = int(first.split("\t")[3])
    assert round_ms >= MIN_CALIBRATED_ROUND_MS, (
        "a round took only {} ms; raise RESTART_PARTS_PER_INSERT so a restart can land inside one".format(round_ms)
    )

    # (5) Wait for the next round to be in flight, restart within its first half, measure.
    cut_round_id = _wait_round_in_flight(node)
    assert cut_round_id is not None, "no round started within 120 s"
    started_at = time.monotonic()
    under_gc_stop_s = _stop_seconds(node)
    assert time.monotonic() - started_at < round_ms / 1000.0 / 2.0 + under_gc_stop_s, (
        "the stop began too late to be inside the round's first half"
    )
    node.start_clickhouse()

    # (6) The bound: independence from round length, and the witness.
    assert under_gc_stop_s <= paused_stop_s + ATTEMPT_TIMEOUT_S + STOP_SLACK_S, (
        "stop under GC took {:.1f} s against a paused baseline of {:.1f} s".format(under_gc_stop_s, paused_stop_s)
    )
    cut = _gc_rows(node, "event_type = 'Finish' AND round_id = '{}'".format(cut_round_id))
    assert cut, "the cut round {} has no Finish row".format(cut_round_id)
    assert cut[-1].split("\t")[2] == "Stopped", (
        "no interruption witnessed: the round in flight at the restart finished as {}".format(cut[-1].split("\t")[2])
    )
    bad = int(gc_log_scalar(node,
        "SELECT count() FROM system.cas_gc_log WHERE disk_name = '{}' AND event_type = 'Finish' "
        "AND outcome IN ('Aborted', 'Error')".format(GC_DISK)))
    assert bad == 0, "{} Aborted/Error rounds during a clean restart".format(bad)

    node.query("DROP TABLE IF EXISTS cas_gc_restart SYNC")
```

- [ ] **Step 2: Run it** — from the `lane-g` root, with the binary at `ci/tmp/clickhouse` as the praktika harness expects (copy `build/programs/clickhouse` there if it is absent), run:

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
python3 -m ci.praktika run "integration" --test test_cas_gc_s3 > build/test_t7_integration.log 2>&1
```

then ANALYZE(`test_t7_integration.log`). Expected: both tests of the module pass. If `MIN_CALIBRATED_ROUND_MS` trips, raise `RESTART_PARTS_PER_INSERT` (to 400) and re-run; do not lower the threshold.

- [ ] **Step 3: Commit** — COMMIT(`tests/integration/test_cas_gc_s3/test.py`, `ca-tests: a restart under GC is measured against a paused baseline and must witness the cut round as Stopped`)

---

### Task 8: Soak — S46, N graceful restarts under GC, final fsck {#task-8}

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s46_restart_under_gc.py`

**Interfaces:**
- Consumes: the framework (`Scenario`, `register`, `Verdict`, `cluster_boot.wait_healthy`, `observe.gc_log_all`, `_common.standard_end`), the compose container `ca-soak-ch1-1`, the disk `ca`.

- [ ] **Step 1: Write the card** — create `utils/ca-soak/scenarios/cards/s46_restart_under_gc.py`:

```python
"""S46: graceful restarts under GC -- the premise that the protocol survives death at any instant.

Probabilistic by design: each restart is taken after a Start row without its Finish is observed and
within the first half of the last calibrated round duration, so most restarts land inside a round.
Asserted: every stop time within one attempt budget plus slack of the paused baseline; no Aborted
or Error round in the window; a clean final fsck. The number of restarts witnessed as Stopped is
reported, not asserted -- a stop that lands in the round's advisory tail finishes as Success.
"""

import subprocess
import time

from ..framework import cluster_boot, observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s46_restart"
_DISK = "ca"
_CH1 = "ca-soak-ch1-1"
_ATTEMPT_TIMEOUT_S = 5.0
_STOP_SLACK_S = 5.0


def _graceful_restart_seconds(container, log_fn):
    """`docker stop` with a long grace, timed -- the stop phase alone; then `docker start`."""
    t0 = time.monotonic()
    subprocess.run(["docker", "stop", "-t", "900", container], check=True, timeout=1000)
    stop_s = time.monotonic() - t0
    subprocess.run(["docker", "start", container], check=True, timeout=120)
    log_fn(f"S46 restart of {container}: stop phase {stop_s:.1f}s")
    return stop_s


def _finished_rounds(cluster, since):
    rows = observe.gc_log_all(cluster, since).get("per_node", {}).get(_CH1, [])
    return [r for r in rows if r.get("event_type") == "Finish"]


def _open_round_ids(cluster, since):
    rows = observe.gc_log_all(cluster, since).get("per_node", {}).get(_CH1, [])
    starts = {r.get("round_id") for r in rows if r.get("event_type") == "Start"}
    finishes = {r.get("round_id") for r in rows if r.get("event_type") == "Finish"}
    return starts - finishes


@register
class S46RestartUnderGc(Scenario):
    name = "S46"
    title = "graceful restarts under GC"
    priority = "P1"

    param_table = {
        "dev": {"restarts": 3, "rows_per_insert": 1000, "inserts": 40, "payload_bytes": 4096,
                "calibrate_timeout_s": 300, "in_flight_timeout_s": 180},
        "ci": {"restarts": 8, "rows_per_insert": 2000, "inserts": 120, "payload_bytes": 8192,
               "calibrate_timeout_s": 600, "in_flight_timeout_s": 300},
        "full": {"restarts": 20, "rows_per_insert": 5000, "inserts": 300, "payload_bytes": 16384,
                 "calibrate_timeout_s": 900, "in_flight_timeout_s": 600},
    }

    def run(self, ctx, result):
        p = self.resolve_params(ctx.scale, ctx.param_overrides)
        cl = ctx.cluster
        node = cl.nodes()[0]
        since = ctx.extra.get("since_event_time")

        # Pause GC while the pool is small, populate with GC paused, take the paused baseline.
        node.command(f"SYSTEM CAS GC STOP {_DISK}", timeout=120)
        sql.create_ca_table(node, _TABLE, columns="id UInt64, payload String", order_by="id")
        for i in range(int(p["inserts"])):
            sql.insert_random(node, _TABLE, rows=int(p["rows_per_insert"]),
                              payload_bytes=int(p["payload_bytes"]), op_id=i)
        paused_stop_s = _graceful_restart_seconds(_CH1, ctx.log)
        if not cluster_boot.wait_healthy(cl, timeout_s=240, log_fn=ctx.log):
            result.add(Verdict.check("cluster healthy after baseline restart", "healthy", "not healthy", False,
                                     "the paused-GC restart must come back"))
            return
        result.observe("paused_stop_s", round(paused_stop_s, 1))

        # Calibrate one full round after GC restarted with the server.
        deadline = time.monotonic() + int(p["calibrate_timeout_s"])
        round_ms = None
        while time.monotonic() < deadline and round_ms is None:
            done = [r for r in _finished_rounds(cl, since) if r.get("outcome") in ("Success", "Deferred")]
            if done:
                round_ms = int(done[-1].get("duration_ms") or 0)
            else:
                time.sleep(1.0)
        result.add(Verdict.check("one round calibrated", "a Success/Deferred Finish row",
                                 f"round_ms={round_ms}", round_ms is not None,
                                 "GC must complete a round before restarts are attempted"))
        if round_ms is None:
            return
        result.observe("calibrated_round_ms", round_ms)

        # N restarts, each inside an observed in-flight round's first half.
        stops = []
        witnessed = 0
        cut_ids = []
        for r in range(int(p["restarts"])):
            deadline = time.monotonic() + int(p["in_flight_timeout_s"])
            open_ids = set()
            while time.monotonic() < deadline and not open_ids:
                open_ids = _open_round_ids(cl, since)
                if not open_ids:
                    time.sleep(0.5)
            if not open_ids:
                result.note_anomaly(f"S46 restart {r}: no round in flight within the timeout; skipped this restart")
                continue
            cut_ids.append(next(iter(open_ids)))
            stop_s = _graceful_restart_seconds(_CH1, ctx.log)
            stops.append(stop_s)
            if not cluster_boot.wait_healthy(cl, timeout_s=240, log_fn=ctx.log):
                result.add(Verdict.check("cluster healthy after restart", "healthy", f"restart {r}: not healthy",
                                         False, "a restarted node did not return"))
                return

        finishes = {r.get("round_id"): r.get("outcome") for r in _finished_rounds(cl, since)}
        witnessed = sum(1 for rid in cut_ids if finishes.get(rid) == "Stopped")
        result.observe("restarts_taken", len(stops))
        result.observe("restarts_witnessed_stopped", witnessed)
        result.observe("stop_seconds", [round(s, 1) for s in stops])

        bound = paused_stop_s + _ATTEMPT_TIMEOUT_S + _STOP_SLACK_S
        worst = max(stops) if stops else 0.0
        result.add(Verdict.check("every stop within one attempt of the paused baseline",
                                 f"<= {bound:.1f}s", f"max {worst:.1f}s", worst <= bound,
                                 "GC's contribution to a teardown is one request, not one round"))
        bad = [rid for rid, outcome in finishes.items() if outcome in ("Aborted", "Error")]
        result.add(Verdict.check("no Aborted/Error round across the restarts", "0", str(len(bad)),
                                 not bad, "a clean restart must never read as a backend incident"))

        # The premise: every interrupted round left consistent state.
        _common.standard_end(ctx, result, [_TABLE])
```

If `observe.gc_log_all` keys its per-node map by a name other than the container (`ctx.log` the keys once on the first call to see), adapt `_CH1` for that lookup only; the `docker` commands keep the container name.

- [ ] **Step 2: Run it at `dev` scale** — following `utils/ca-soak/README.md` for the compose bring-up, from the `lane-g` root:

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g/utils/ca-soak
python3 -m scenarios.run --scenarios S46 --scale dev > ../../build/test_t8_soak_s46.log 2>&1
```

then ANALYZE(`test_t8_soak_s46.log`) with the added question "which verdicts are green, which are red, and what `restarts_witnessed_stopped` reports". Expected: every verdict green, fsck clean, `restarts_witnessed_stopped >= 1`. A red verdict is a root cause, not a threshold to move.

- [ ] **Step 3: Commit** — COMMIT(`utils/ca-soak/scenarios/cards/s46_restart_under_gc.py`, `ca-soak: S46 restarts the server under GC and asks fsck whether every cut round left consistent state`)

---

### Task 9: Spec alignment, the gate again, merge back {#task-9}

**Files:**
- Modify (in the `master` worktree, on `cas-gc-rebuild`): `docs/superpowers/specs/2026-09-04-cas-gc-teardown-stop-design.md` — the T2 row of the testing table

- [ ] **Step 1: Align the spec's test table with what was built** — in the `master` worktree, edit the T2 row of the spec's testing table: replace `(b) **inside the body**: a backend whose \`stream\` returns a buffer that parks in \`nextImpl\` after the open returned;` with `(b) **inside the body** — exercised at the engine level in T10 (\`StreamBodyKeepsThePreloadedWindowAndRefusesOnTheNextRefill\`), because the fold streams a run only after a first generation exists; the round-level body case is what T8's witness covers;`. Commit in `master`:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git diff --cached --stat
git add docs/superpowers/specs/2026-09-04-cas-gc-teardown-stop-design.md
git commit -m "ca-docs: the teardown-stop spec's T2 names where the body case is actually exercised

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VhphEVZSskrNzFKUGkHyPi" -- docs/superpowers/specs/2026-09-04-cas-gc-teardown-stop-design.md
git log -1 --stat
```

- [ ] **Step 2: The gate once more on the branch head** — in `lane-g`: BUILD(`build_t9_gate.log`), ANALYZE(`build_t9_gate.log`), TEST(`CAS*`, `test_t9_gate.log`), ANALYZE(`test_t9_gate.log`). Expected: all pass.

- [ ] **Step 3: The teardown integration tests** — from the `lane-g` root:

```bash
cd /home/mfilimonov/workspace/ClickHouse/lane-g
python3 -m ci.praktika run "integration" --test "test_cas_gc_s3 test_cas_gc_sharded test_cas_drop_pool_member test_cas_shared_pool" > build/test_t9_integration.log 2>&1
```

then ANALYZE(`test_t9_integration.log`). Expected: all pass.

- [ ] **Step 4: Merge into `cas-gc-rebuild`** — in the `master` worktree, only if Steps 2 and 3 were green:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git status --short | grep -v '^??' | grep -v '^ ? contrib'
```

Expected: no tracked changes staged by another session that would ride along (untracked files are fine). Then:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git merge --no-ff cas-gc-teardown-stop -m "Merge cas-gc-teardown-stop: a disk's teardown no longer waits out a GC round

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01VhphEVZSskrNzFKUGkHyPi"
git log --oneline -3
```

Expected: a merge commit on `cas-gc-rebuild`. **Do not push.**

- [ ] **Step 5: Report** — list the commits merged, the gate counts from Step 2 (run/passed/failed), the integration result, the S46 verdicts and `restarts_witnessed_stopped`, and the two backlog entries to update afterwards (`operability-and-introspection.md` `{#lifecycle-verbs-wait-out-uncancellable-scans}`: the teardown half is done, `SYSTEM CAS GC STOP` and the FSCK deadline remain; `gc.md` `{#gc-lease-not-released-on-clean-stop}`: unchanged).

---

## Self-review against the spec {#self-review}

- **Decision / bound:** Task 2 (fence, sleep), Task 3 (body), Task 5 (arms) — the three gated units; the CPU reduce and the in-refill SDK retries are ungated by decision and covered only by T8/T9's slack.
- **The flag already exists, atomic, no reorder:** Task 1.
- **Delivery — a teardown fence on the open plane, `check_or_throw` a no-op:** Task 2 Step 4.
- **The retry sleep, seam restoration:** Task 2 Steps 3 and 5; T4 as a wiring test.
- **The stream body — preloaded window, `nextImpl`, one mapping, by-value captures:** Task 3.
- **Which plane:** Task 2's mount-plane assertion.
- **Sequencing — four arms, no `~Pool` arm, before any lock that can block:** Task 5 Steps 4–7.
- **Classification — honest `Stopped`, fail-closed, loop hygiene, heartbeat, REBUILD untouched:** Task 4; T5 twins.
- **Tests T1–T11:** T1 = `OpenPlaneRefusesAfterTeardownBeganAndTheMountPlaneDoesNot`; T2a = `BackgroundRoundIsCutAtItsNextRequest`; T2b = `StreamBodyKeepsThePreloadedWindowAndRefusesOnTheNextRefill` (engine level, spec aligned in Task 9); T2c = `ReadAheadWorkerIsRefusedAndTheTakeSiteSeesIt`; T3 = `ShutdownReturnsWhileASynchronousRoundIsParked`; T4 = `OpenPlaneSleepReturnsAtOnceOnceTeardownBegan`; T5 = the two `CASGCLog` twins; T6 = `AQueuedScheduledTickEmitsNoStartAfterTeardownBegan`; T7 = `AStopInsideTheJanitorPageIsSwallowedAsDeferred`; T8 = Task 7; T9 = Task 8; T10 = `StreamBodyServesEveryWindowThenEof` + the refusal test; T11 = `StreamBodyRefusalKeepsTheNoBudgetMapping`.
- **Verification gate:** Task 6 and Task 9.
- **Consequences / out of scope:** nothing in this plan touches the durable lease, `SYSTEM CAS GC STOP`, a global deadline or `src/IO`.

Names used across tasks and their definitions: `Pool::beginTeardown`/`teardownBegun` (Task 1, used in 2, 4, 5); `openPlaneSleepFn` (Task 2); `throwReadRefused`, `AdmittedBodyReadBuffer` (Task 3); `Outcome::Stopped`, `STOPPED` (Task 4, used in 5); `setGcRoundRowHookForTest` (Task 5); test helpers `Gate`, `GateOpenedOnExit`, `openTestStorage`, `ParkFirstReadBackend`, `RoundLogSink`, `countStarts` (Task 5, defined before first use in the same file).
