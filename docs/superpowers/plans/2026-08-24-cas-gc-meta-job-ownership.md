# CAS GC meta-job ownership and mount-claim error classification — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close findings B1 and B2a of the 2026-08-05 umbrella review: make the CAS GC meta-pool's jobs own everything they touch, drain that pool on every round exit, and stop `MountLeaseKeeper::claim` from raising `LOGICAL_ERROR` on externally reachable conflicts.

**Architecture:** The bounded meta pool, its two job counters, its condemn-marker confirmation registry and the `PoolPtr` its jobs write through move out of `Gc` into a new `GcMetaWriter` that exposes two typed operations and accepts no caller-supplied callable, so a job capturing `this` becomes inexpressible rather than merely absent. `Gc::runRegularRound` gains a scope guard that drains the pool on the throwing exit as well as the normal one. Independently, six `LOGICAL_ERROR` throws in `MountLeaseKeeper::claim` become `ABORTED`, and each of those branches — plus the two `MountFencedException` branches that must keep precedence over them — gets a direct test driven by a one-shot backend race seam.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`), gtest via `unit_tests_dbms`, CMake + ninja.

**Spec:** `docs/superpowers/specs/2026-08-24-cas-gc-meta-job-ownership-design.md` (rev.4, approved 2026-08-24)

## Global Constraints

- **Branch:** work on the existing `cas-gc-rebuild`. Do **not** create a new branch, do **not** rebase, do **not** amend — add new commits only.
- **No pushing.** Commit locally; pushing requires a separate, explicit request.
- **Scope:** item 9's B1 and B2a only. B3 (the `custom_disk` privilege gate) and item 10 (detached work outliving `Context`) are out of scope and must not be touched.
- **No durable-format, key-shape or protocol-step changes.** Nothing in this plan may alter what is written to object storage, only which in-process object owns the writing.
- **C++ style:** Allman braces (opening brace on its own line) — enforced by the CI style check.
- **Comments:** no references to plans, backlogs, reviews or task numbers in source comments. Keep the reason, drop the provenance.
- **Prose:** write function names as `f`, not `f()`. Wrap SQL identifiers, class and function names, and log-message excerpts in inline code.
- **Never add a `sleep` to fix a race.** A bounded `wait_for` used to assert that a thread is blocked is a different thing and is used deliberately in Task 2.
- **Build output goes to a log file** in the build directory, and the log is summarized by a subagent — never pasted wholesale.
- **No `-j` with ninja**, and do not invoke `nproc`.
- **Never `git add -A`.** The working tree contains large untracked artifacts; stage named paths only.
- New `.cpp`/`.h` files under `.../ContentAddressed/Gc/` and new `gtest*.cpp` files under `src/` need **no** CMake edit: both are globbed with `CONFIGURE_DEPENDS` (`cmake/dbms_glob_sources.cmake:1-3`, `src/CMakeLists.txt:903`), so ninja reconfigures on its own.

## File Structure

| File | Responsibility |
|---|---|
| `src/Disks/.../ContentAddressed/Gc/CasGcMetaWriter.h` (create) | Declares `GcMetaWriter`: owns the bounded meta pool and everything its jobs touch; two typed scheduling operations, a drain, counter accessors, and the condemn-marker registry accessors. |
| `src/Disks/.../ContentAddressed/Gc/CasGcMetaWriter.cpp` (create) | Implements it, including the two meta-write helpers moved out of `CasGc.cpp`. |
| `src/Disks/.../ContentAddressed/Gc/CasGc.h` (modify) | Drops `meta_pool`, both counters, the mutex, the set, `scheduleMetaJob` and the three registry accessors; gains one `std::unique_ptr<GcMetaWriter> meta_writer`. |
| `src/Disks/.../ContentAddressed/Gc/CasGc.cpp` (modify) | Constructs `meta_writer` in the constructor body; routes every former call site through it; adds the round-exit drain guard. |
| `src/Disks/.../ContentAddressed/Pool/CasServerRoot.cpp` (modify) | Six `LOGICAL_ERROR` throws in `claim` become `ABORTED`. |
| `src/Disks/tests/cas_test_helpers.h` (modify) | Two new test backends: `MetaWriteLatchBackend` (Task 1) and `MountSlotRaceBackend` (Task 3). |
| `src/Disks/tests/gtest_cas_gc_meta_writer.cpp` (create) | Task 1 and Task 2 tests: jobs across `Gc` destruction, and the round-exit drain. |
| `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp` (create) | Task 3 tests: all six reclassified branches plus both fenced-precedence branches. |
| `src/Disks/tests/gtest_cas_mount.cpp` (modify) | The existing death test folds into the new file's different-epoch case. |

---

### Task 1: Extract `GcMetaWriter`

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h:882-894`, `:947-968`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:127-155`, `:374-378`, `:381-440`, `:867-870`, `:916`, `:965`, `:511`, `:1017-1034`, `:1890-1896`
- Modify: `src/Disks/tests/cas_test_helpers.h`
- Test: `src/Disks/tests/gtest_cas_gc_meta_writer.cpp` (create)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, for Task 2 and Task 3:
  - `DB::Cas::GcMetaWriter`, with `void scheduleCondemnMarkerWrite(const BlobRef &, const Token &, uint64_t condemn_round, uint64_t size)`, `void scheduleConfirmedMetaDelete(const BlobRef &)`, `void drain()`, `uint64_t scheduled() const`, `uint64_t completed() const`, `void noteCondemnMarkerDurable(const BlobRef &, const Token &)`, `bool condemnMarkerConfirmedInProcess(const BlobRef &, const Token &)`, `void forgetCondemnMarker(const BlobRef &, const Token &)`.
  - On `Gc`, a test seam `GcMetaWriter & metaWriterForTest()`.
  - In `cas_test_helpers.h`, `DB::Cas::tests::MetaWriteLatchBackend` with members `std::atomic<bool> entered{false}` and `void release()`.

**Context an implementer needs.** `Gc` today owns `std::unique_ptr<ThreadPool> meta_pool` (`CasGc.h:950`), the counters `meta_jobs_scheduled_` / `meta_jobs_completed_` (`:956-957`), and the registry `condemn_marker_mutex` + `condemn_markers_confirmed` (`:967-968`). `Gc::scheduleMetaJob` (`CasGc.cpp:381-440`) wraps a caller's closure so it never throws, counts it, and falls back to running it inline when scheduling fails. Two callers hand it a closure that captures `this`: `scheduleCondemnMarkerWrite` (`:433`) and the delete confirmation (`:867`). Both reach `store` through `this`. The two helpers those jobs call, `writeCondemnedMeta` (`:127`) and `deleteConfirmedMeta` (`:142`), live in the anonymous namespace of `CasGc.cpp` and have no other callers, so they move with the jobs.

`GcMetaWriter` must be held by `std::unique_ptr` and built in the **body** of `Gc::Gc`, not as a direct member: its pool size comes from `store->poolConfig().gc_meta_pool_size`, and `Gc::Gc` validates a null `store` in its body (`CasGc.cpp:350-357`). A direct member would dereference null before that check and break `CASGCLease.CtorFailsClosedOnBadArguments` (`src/Disks/tests/gtest_cas_gc_round.cpp:459`).

- [ ] **Step 1: Create the header**

Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h`:

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Common/ThreadPool.h>
#include <Common/logger_useful.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>

namespace DB::Cas
{

/// Owns the bounded pool for a GC round's per-hash freshness-meta writes (condemn / spare / delete)
/// AND everything those writes touch.
///
/// There is deliberately NO way to hand this class a closure. The only paths onto the pool are the
/// two typed operations below, and each captures nothing but a `shared_ptr` to `State`. A job
/// therefore cannot reach anything owned by the enclosing `Gc`, which is what keeps a job that
/// outlives its owner well-defined instead of dependent on member-declaration order.
class GcMetaWriter
{
public:
    GcMetaWriter(PoolPtr store_, LoggerPtr logger_, size_t pool_size);

    GcMetaWriter(const GcMetaWriter &) = delete;
    GcMetaWriter & operator=(const GcMetaWriter &) = delete;

    /// Publish durable Condemned evidence for one (blob, exact incarnation-token) pair. On success the
    /// pair is recorded in the in-process confirmation registry, which the graduation gate reads. A
    /// lost CAS or a thrown error leaves the pair UNCONFIRMED: the gate then carries the entry and a
    /// later round retries the write.
    void scheduleCondemnMarkerWrite(const BlobRef & ref, const Token & token,
                                    uint64_t condemn_round, uint64_t size);

    /// Drop the freshness meta of a blob whose body is confirmed deleted or absent.
    void scheduleConfirmedMetaDelete(const BlobRef & ref);

    /// Wait for every job scheduled so far. Never throws: each job already caught its own exception.
    void drain();

    uint64_t scheduled() const;
    uint64_t completed() const;

    /// The in-process condemn-marker confirmation registry, keyed (blob, exact token value). Pool
    /// completions insert concurrently with the round thread's reads, and the round thread also
    /// inserts directly when it re-checks a marker synchronously.
    void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
    bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
    void forgetCondemnMarker(const BlobRef & ref, const Token & token);

private:
    /// Everything a job reaches. Held by `shared_ptr` and captured by value into every job.
    struct State
    {
        PoolPtr store;
        LoggerPtr logger;
        std::atomic<uint64_t> scheduled{0};
        std::atomic<uint64_t> completed{0};
        std::mutex condemn_marker_mutex;
        std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;

        void noteCondemnMarkerDurable(const BlobRef & ref, const Token & token);
        bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token);
        void forgetCondemnMarker(const BlobRef & ref, const Token & token);
    };

    /// Wrap one meta op so it can never throw, count it, and put it on the pool -- running it inline
    /// if scheduling itself fails, rather than silently losing the write. Private, and takes only
    /// what this class produces: the typed operations above are the sole callers.
    void submit(std::function<void()> op);

    std::shared_ptr<State> state;
    ThreadPool pool;
};

}
```

- [ ] **Step 2: Create the implementation**

Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp`. Move `writeCondemnedMeta` (`CasGc.cpp:127-140`) and `deleteConfirmedMeta` (`CasGc.cpp:142-155`) here **verbatim**, including their doc comments at `CasGc.cpp:100-126`, into this file's anonymous namespace. Then:

```cpp
namespace ProfileEvents
{
extern const Event CASGCMetaOps;
extern const Event CASGCMetaWriteAnomaly;
}

namespace DB::Cas
{

GcMetaWriter::GcMetaWriter(PoolPtr store_, LoggerPtr logger_, size_t pool_size)
    : state(std::make_shared<State>())
    , pool(CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
           CurrentMetrics::LocalThreadScheduled, std::max<size_t>(1, pool_size))
{
    state->store = std::move(store_);
    state->logger = std::move(logger_);
}

void GcMetaWriter::submit(std::function<void()> op)
{
    /// `run` is safe to invoke either on the pool or inline (the scheduling-failure fallback below)
    /// and NEVER lets an exception escape: a per-hash meta op is advisory -- the ledger and the
    /// exact-token body delete are the actual safety core. It captures only `state`, so it stays
    /// well-defined however long it outlives the writer that scheduled it.
    auto run = [op, st = state]()
    {
        ProfileEvents::increment(ProfileEvents::CASGCMetaOps);
        try
        {
            op();
        }
        catch (...)
        {
            ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);
            tryLogCurrentException(st->logger,
                "CAS gc: a per-hash freshness-meta op failed on the bounded pool (advisory-only; "
                "never wedges the round)");
        }
        /// A job that threw still FINISHED: this counter reports drain progress, not success.
        st->completed.fetch_add(1, std::memory_order_relaxed);
    };
    state->scheduled.fetch_add(1, std::memory_order_relaxed);
    try
    {
        pool.scheduleOrThrowOnError(run);
    }
    catch (...)
    {
        /// Scheduling itself failed (e.g. resource exhaustion under a mass-DROP burst) -- run inline
        /// rather than silently lose the meta write. `run` still never throws.
        ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);
        tryLogCurrentException(state->logger,
            "CAS gc: meta pool scheduling failed; running the op inline on the round's own thread");
        run();
    }
}

void GcMetaWriter::scheduleCondemnMarkerWrite(const BlobRef & ref, const Token & token,
                                              uint64_t condemn_round, uint64_t size)
{
    submit([st = state, ref, token, condemn_round, size]()
    {
        if (writeCondemnedMeta(*st->store, ref, condemn_round, size))
            st->noteCondemnMarkerDurable(ref, token);
    });
}

void GcMetaWriter::scheduleConfirmedMetaDelete(const BlobRef & ref)
{
    submit([st = state, ref]()
    {
        deleteConfirmedMeta(st->store->backend(), st->store->layout(), ref);
    });
}

void GcMetaWriter::drain()
{
    pool.wait();
}

uint64_t GcMetaWriter::scheduled() const
{
    return state->scheduled.load(std::memory_order_relaxed);
}

uint64_t GcMetaWriter::completed() const
{
    return state->completed.load(std::memory_order_relaxed);
}

void GcMetaWriter::State::noteCondemnMarkerDurable(const BlobRef & ref, const Token & token)
{
    std::lock_guard lock(condemn_marker_mutex);
    condemn_markers_confirmed.emplace(ref, token.value);
}

bool GcMetaWriter::State::condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token)
{
    std::lock_guard lock(condemn_marker_mutex);
    return condemn_markers_confirmed.contains({ref, token.value});
}

void GcMetaWriter::State::forgetCondemnMarker(const BlobRef & ref, const Token & token)
{
    std::lock_guard lock(condemn_marker_mutex);
    condemn_markers_confirmed.erase({ref, token.value});
}

void GcMetaWriter::noteCondemnMarkerDurable(const BlobRef & ref, const Token & token)
{
    state->noteCondemnMarkerDurable(ref, token);
}

bool GcMetaWriter::condemnMarkerConfirmedInProcess(const BlobRef & ref, const Token & token)
{
    return state->condemnMarkerConfirmedInProcess(ref, token);
}

void GcMetaWriter::forgetCondemnMarker(const BlobRef & ref, const Token & token)
{
    state->forgetCondemnMarker(ref, token);
}

}
```

Add the `CurrentMetrics` extern block for `LocalThread`, `LocalThreadActive` and `LocalThreadScheduled`, copying the form used at the top of `CasGc.cpp`.

- [ ] **Step 3: Strip the moved state out of `Gc`**

In `CasGc.h`: delete the declarations of `scheduleMetaJob` (`:882`), `noteCondemnMarkerDurable`, `condemnMarkerConfirmedInProcess` and `forgetCondemnMarker` (`:891-894`), `meta_pool` (`:947-950`), `meta_jobs_scheduled_` / `meta_jobs_completed_` (`:952-957`), `condemn_marker_mutex` and `condemn_markers_confirmed` (`:960-968`). Add `#include <.../Gc/CasGcMetaWriter.h>` and, in their place:

```cpp
    /// Owns the bounded pool for this round's per-hash freshness-meta writes and everything those
    /// writes touch. A `unique_ptr` because its pool size comes from `store->poolConfig()`, which may
    /// only be read after the constructor body has validated `store` -- a direct member would be
    /// initialized before that check.
    std::unique_ptr<GcMetaWriter> meta_writer;
```

Add, to the existing `TEST SEAM` block at the end of the class:

```cpp
    /// TEST SEAM: reach the meta writer so a unit test can schedule a real meta op and read the
    /// round's job accounting without driving a full round.
    GcMetaWriter & metaWriterForTest() { return *meta_writer; }
```

- [ ] **Step 4: Rewire `Gc` in the implementation**

In `CasGc.cpp`:

1. Delete `writeCondemnedMeta`, `deleteConfirmedMeta` and their doc comments (`:100-155`) — they now live in `CasGcMetaWriter.cpp`.
2. Delete `Gc::scheduleMetaJob` (`:381-440`), `Gc::scheduleCondemnMarkerWrite` (`:430-440`), `Gc::noteCondemnMarkerDurable`, `Gc::condemnMarkerConfirmedInProcess` and `Gc::forgetCondemnMarker` (`:442-458`).
3. Replace the pool construction in the constructor body (`:374-378`) with:

```cpp
    /// Build the meta writer here (ctor body), not in a member-initializer, so it can safely read
    /// `store->poolConfig()` AFTER the null check above.
    meta_writer = std::make_unique<GcMetaWriter>(
        store, logger, static_cast<size_t>(store->poolConfig().gc_meta_pool_size));
```

4. Rewrite each remaining call site:

| Site | Was | Becomes |
|---|---|---|
| `:867` | `scheduleMetaJob([this, ref]() { deleteConfirmedMeta(store->backend(), store->layout(), ref); });` | `meta_writer->scheduleConfirmedMetaDelete(ref);` |
| `:870`, `:916`, `:965` | `forgetCondemnMarker(…)` | `meta_writer->forgetCondemnMarker(…)` |
| `:964` and the fold's condemn sites | `scheduleCondemnMarkerWrite(…)` | `meta_writer->scheduleCondemnMarkerWrite(…)` |
| `:511` | `meta_jobs_scheduled_.load(…)` / `meta_jobs_completed_.load(…)` baselines | `meta_writer->scheduled()` / `meta_writer->completed()` |
| `:1026-1034` | the same loads plus `meta_pool->wait()` | the same accessors plus `meta_writer->drain()` |
| `:1890` | `condemnMarkerConfirmedInProcess(…)` | `meta_writer->condemnMarkerConfirmedInProcess(…)` |
| `:1896` | `noteCondemnMarkerDurable(…)` | `meta_writer->noteCondemnMarkerDurable(…)` |

5. Delete the comment at `:386-392` that justifies the raw-pointer counter capture by an `~Gc` that does not exist. Do not rewrite it — the argument it makes is the one this change removes.

- [ ] **Step 5: Verify no callback surface remains**

Run:

```bash
grep -rn "scheduleMetaJob" src/Disks/
```

Expected: no output. This grep is the regression guard for the whole task — if it ever prints again, the lifetime boundary has been reopened.

- [ ] **Step 6: Add the latch backend to the test helpers**

Append to `src/Disks/tests/cas_test_helpers.h`, next to `MetaWriteFaultBackend`:

```cpp
/// Blocks INSIDE a blob-meta write until `release` is called, so a test can hold a real meta job in
/// flight and observe that it got there. `entered` is set before blocking.
class MetaWriteLatchBackend : public DB::Cas::InMemoryBackend
{
public:
    using DB::Cas::Backend::get;
    using DB::Cas::Backend::getStream;
    using DB::Cas::Backend::putIfAbsent;
    using DB::Cas::Backend::putOverwrite;
    using DB::Cas::Backend::casPut;

    std::atomic<bool> entered{false};

    void release()
    {
        std::lock_guard lock(latch_mutex);
        released = true;
        latch_cv.notify_all();
    }

    DB::Cas::PutResult putIfAbsent(
        const String & key, const String & bytes, const DB::Cas::ObjectMeta & meta) override
    {
        waitIfMeta(key);
        return InMemoryBackend::putIfAbsent(key, bytes, meta);
    }

    DB::Cas::PutResult putOverwrite(
        const String & key, const String & bytes, const DB::Cas::Token & expected,
        const DB::Cas::ObjectMeta & meta) override
    {
        waitIfMeta(key);
        return InMemoryBackend::putOverwrite(key, bytes, expected, meta);
    }

    DB::Cas::CasResult casPut(const String & key, const String & bytes,
                              const std::optional<DB::Cas::Token> & expected,
                              const DB::Cas::ObjectMeta & meta) override
    {
        waitIfMeta(key);
        return InMemoryBackend::casPut(key, bytes, expected, meta);
    }

    DB::Cas::DeleteOutcome deleteExact(const String & key, const DB::Cas::Token & token) override
    {
        waitIfMeta(key);
        return InMemoryBackend::deleteExact(key, token);
    }

private:
    void waitIfMeta(const String & key)
    {
        if (!key.ends_with(".meta"))
            return;
        entered.store(true);
        std::unique_lock lock(latch_mutex);
        latch_cv.wait(lock, [this] { return released; });
    }

    std::mutex latch_mutex;
    std::condition_variable latch_cv;
    bool released = false;
};
```

- [ ] **Step 7: Write the functional-coverage test**

Create `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`. This test is **not** failing-first and the plan does not pretend otherwise: the pre-change defect is an access to destructed-but-still-allocated storage, which no sanitizer reports, so no runtime assertion can distinguish the two trees. Its job is to pin the behaviour going forward; the boundary itself is enforced by Step 5's grep.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <thread>

using namespace DB::Cas;
using DB::Cas::tests::MetaWriteLatchBackend;

namespace
{
constexpr auto kGcId = "0000000000000000000000000000002a";
}

/// A real condemn-marker job may be in flight when its `Gc` is destroyed. The job holds everything it
/// touches, so the pool's join completes it correctly rather than racing member teardown.
///
/// This asserts function, not ordering: the release may land before, during or after destruction
/// begins, and all three are sound. Nothing here detects a job that wrongly captured its owner --
/// that is prevented by there being no API to write one.
TEST(CasGcMetaWriter, RealCondemnMarkerJobSurvivesOwnerDestruction)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    const BlobRef ref = DB::Cas::tests::idOf(1);
    const Token token{"tok-1"};

    auto gc = std::make_unique<Gc>(store, DB::Cas::tests::u128Of(kGcId));
    gc->metaWriterForTest().scheduleCondemnMarkerWrite(ref, token, /*condemn_round=*/1, /*size=*/128);

    while (!backend->entered.load())
        std::this_thread::yield();

    std::thread releaser([&] { backend->release(); });
    gc.reset();
    releaser.join();
}

/// Same for the other production job.
TEST(CasGcMetaWriter, RealConfirmedMetaDeleteSurvivesOwnerDestruction)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    auto gc = std::make_unique<Gc>(store, DB::Cas::tests::u128Of(kGcId));
    gc->metaWriterForTest().scheduleConfirmedMetaDelete(DB::Cas::tests::idOf(2));

    while (!backend->entered.load())
        std::this_thread::yield();

    std::thread releaser([&] { backend->release(); });
    gc.reset();
    releaser.join();
}
```

Check `idOf` and `u128Of` against `src/Disks/tests/cas_test_helpers.h` before using them and adjust the calls to the helpers' real signatures; they are used the same way in `gtest_cas_gc_leak.cpp:34-36`.

- [ ] **Step 8: Build**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_task1.log 2>&1; echo "NINJA_EXIT=$?"
```

Expected: `NINJA_EXIT=0`. Dispatch a subagent to read `build_debug/build_task1.log` and report a short summary; do not read the log directly.

- [ ] **Step 9: Run the CAS suite**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_debug/unit_tests_dbms --gtest_filter='Cas*:CA*' > build_debug/test_task1.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: `TEST_EXIT=0`. Dispatch a subagent to summarize the log. `CASGCLease.CtorFailsClosedOnBadArguments` and `CASGCAckFloor` (the suite containing `gtest_cas_gc_ack_floor.cpp:1242-1277`, which reasons that the confirmation registry dies with its `Gc`) must both be present and passing — name them explicitly in the summary request rather than accepting an aggregate "all passed".

- [ ] **Step 10: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
        src/Disks/tests/cas_test_helpers.h \
        src/Disks/tests/gtest_cas_gc_meta_writer.cpp
git commit -m "fix: give CAS GC meta-pool jobs ownership of what they touch"
```

Verify with `git log -1 --stat` that only those six paths are in the commit — this checkout is shared with other sessions.

---

### Task 2: Drain the meta pool on every round exit

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` (`runRegularRound`, whose body starts at `:461`)
- Modify: `src/Disks/tests/cas_test_helpers.h`
- Test: `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`

**Interfaces:**
- Consumes: `GcMetaWriter::drain`, `GcMetaWriter::scheduled`, `GcMetaWriter::completed`, `Gc::metaWriterForTest`, `MetaWriteLatchBackend` — all from Task 1.
- Produces: `DB::Cas::tests::OutcomeLogFaultBackend`, a `MetaWriteLatchBackend` subclass that throws out of the round's outcome-log write.

**Context an implementer needs.** `Cas::Gc gc` is a member of the scheduler (`Gc/CasGcScheduler.h:190`), so one pool and one registry are shared across rounds. The only drain today is the `meta_pool_wait` phase (`CasGc.cpp:1017-1034`), which sits between the pending-deletes phase and the round commit. Condemn markers are scheduled earlier — during the fold, and again in the supersede branch at `:964`. A round that throws in between leaves its jobs running while the next round starts; those jobs insert into the registry the next round's graduation gate reads (`:1890`) and land inside its counter deltas, whose baseline is sampled at its own start (`:511`).

The uncaught throw seam used by the test is the outcome-log write at `CasGc.cpp:970-984`: a `putIfAbsent` reporting `PreconditionFailed` followed by a `get` returning nothing raises `ABORTED` — "outcome log at {} vanished between putIfAbsent and read". It runs after every condemn-marker scheduling site and before the wait.

- [ ] **Step 1: Add the fault backend**

Append to `src/Disks/tests/cas_test_helpers.h`:

```cpp
/// Makes a GC round throw at its outcome-log write -- after every condemn marker has been scheduled
/// and before the round's meta-pool wait. Inherits the `.meta` latch so a condemn-marker job can be
/// held in flight across that throw.
class OutcomeLogFaultBackend : public MetaWriteLatchBackend
{
public:
    std::atomic<bool> fail_outcome_logs{true};

    DB::Cas::PutResult putIfAbsent(
        const String & key, const String & bytes, const DB::Cas::ObjectMeta & meta) override
    {
        if (fail_outcome_logs.load() && key.contains("outcomes/"))
            return DB::Cas::PutResult{.outcome = DB::Cas::PutOutcome::PreconditionFailed};
        return MetaWriteLatchBackend::putIfAbsent(key, bytes, meta);
    }

    std::optional<DB::Cas::GetResult> get(const String & key, DB::Cas::Range range) override
    {
        if (fail_outcome_logs.load() && key.contains("outcomes/"))
            return std::nullopt;
        return MetaWriteLatchBackend::get(key, range);
    }
};
```

Confirm `PutResult`'s field names against `Backend/CasBackend.h` before writing this, and confirm `outcomesKey` still contains the literal `outcomes/` (`Formats/CasLayout.h:359-363`).

- [ ] **Step 2: Write the failing test**

Append to `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`. The assertion is that the round does not **return** while a meta job is still in flight — which is what a drain means, and the only formulation that does not race the job's completion:

```cpp
/// A round that throws must not leave its meta jobs running into the next round: their confirmations
/// would land in the registry the next round's graduation gate reads, and inside its counter deltas.
///
/// The round is made to throw at its outcome-log write, with one condemn-marker job held inside the
/// backend. The round must then BLOCK, draining, until that job is released -- so the test asserts
/// the round has not returned while the job is still held, releases, and only then joins.
TEST(CasGcMetaWriter, ThrowingRoundDrainsBeforeReturning)
{
    auto backend = std::make_shared<DB::Cas::tests::OutcomeLogFaultBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    /// A part written and dropped gives the round blobs to condemn; drive one clean round first so
    /// the fold has a baseline. Mirror the setup in `gtest_cas_gc_leak.cpp`.
    DB::Cas::tests::writeAndDropOnePart(store);

    Gc gc(store, DB::Cas::tests::u128Of(kGcId));

    auto round = std::async(std::launch::async, [&]
    {
        EXPECT_ANY_THROW(gc.runRegularRound());
    });

    while (!backend->entered.load())
        std::this_thread::yield();

    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while a meta job was still in flight -- it did not drain on its "
           "throwing exit";

    backend->release();
    round.get();

    EXPECT_EQ(gc.metaWriterForTest().scheduled(), gc.metaWriterForTest().completed());
}
```

`writeAndDropOnePart` is a placeholder for whatever the existing helpers in `gtest_cas_gc_leak.cpp` use to produce a condemnable blob — read that file's setup (`gtest_cas_gc_leak.cpp:40-100`) and inline the equivalent here rather than inventing a new helper. Add `#include <future>` and `#include <chrono>`.

- [ ] **Step 3: Run it and watch it fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_task2_red.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CasGcMetaWriter.ThrowingRoundDrainsBeforeReturning' \
    > build_debug/test_task2_red.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: build succeeds, test **fails** on the `wait_for` assertion — the round returns immediately instead of timing out. If it fails for any other reason, the setup does not reach the intended throw; fix the setup before continuing, and do not proceed on a red obtained for the wrong reason.

- [ ] **Step 4: Add the drain guard**

In `runRegularRound`, immediately after the round's local state is set up and before any work that can schedule a meta job, add:

```cpp
    /// Every exit path leaves this round's meta jobs finished. The `meta_pool_wait` phase below is a
    /// protocol barrier -- this round's condemns must be durable no later than the ledger they are
    /// paired with -- and covers only the successful path. A round that throws between the fold and
    /// that barrier would otherwise leave its jobs running into the NEXT round, where their
    /// confirmations reach a graduation gate that never scheduled them.
    SCOPE_EXIT({ meta_writer->drain(); });
```

Add `#include <base/scope_guard.h>` to `CasGc.cpp` if `SCOPE_EXIT` is not already available there — check first, since `CasGcScheduler.cpp:175` already uses it and may pull it in transitively; rely on a direct include rather than a transitive one.

- [ ] **Step 5: Run it and watch it pass**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_task2_green.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CasGcMetaWriter.*' \
    > build_debug/test_task2_green.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: `TEST_EXIT=0`, all three `CasGcMetaWriter` tests passing.

- [ ] **Step 6: Run the whole CAS suite**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_debug/unit_tests_dbms --gtest_filter='Cas*:CA*' > build_debug/test_task2_full.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: `TEST_EXIT=0`. Summarize via a subagent. Pay particular attention to any test that drives several rounds and asserts timing-sensitive counters — the new guard makes a throwing round slower to return.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
        src/Disks/tests/cas_test_helpers.h \
        src/Disks/tests/gtest_cas_gc_meta_writer.cpp
git commit -m "fix: drain the CAS GC meta pool on a round's throwing exit"
```

---

### Task 3: `MountLeaseKeeper::claim` raises `ABORTED`, with a test per branch

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:1460-1537`
- Modify: `src/Disks/tests/cas_test_helpers.h`
- Modify: `src/Disks/tests/gtest_cas_mount.cpp:1057-1078`
- Test: `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp` (create)

**Interfaces:**
- Consumes: nothing from Tasks 1 and 2 — this task is independent and may be done first if convenient.
- Produces: `DB::Cas::tests::MountSlotRaceBackend` with one-shot hooks `before_put_if_absent`, `before_get`, `before_put_overwrite`.

**Context an implementer needs.** `claim` (`CasServerRoot.cpp:1460-1537`) raises `LOGICAL_ERROR` on six branches and `MountFencedException` on two. Constructing an exception with `LOGICAL_ERROR` calls `handle_error_code` (`src/Common/Exception.cpp`), which aborts the process on any `DEBUG_OR_SANITIZER_BUILD` — and `claim` is reached from the background self-remount thread (`Pool/CasPool.cpp:1233`), so today these are aborts on the lanes that certify the feature. Every one of the six describes a state produced by another process, not a violated invariant of ours; the renewal path already raises `ABORTED` for the same conditions (`:1594`, `:1604`, `:1618`).

The two `MountFencedException` branches (`:1508`, `:1521`) are **not** touched: they are caught by type at `Pool/CasPool.cpp:709`, which turns a GC fence during mount into a bounded epoch-bump retry. Their precedence over the reclassified branches is part of the contract.

- [ ] **Step 1: Add the race seam**

Append to `src/Disks/tests/cas_test_helpers.h`:

```cpp
/// Runs a caller-supplied action ONCE, immediately before the named backend call, so a test can make
/// the mount slot change inside a window `MountLeaseKeeper::claim` holds open. Each hook clears
/// itself after firing.
class MountSlotRaceBackend : public DB::Cas::InMemoryBackend
{
public:
    using DB::Cas::Backend::get;
    using DB::Cas::Backend::getStream;
    using DB::Cas::Backend::putIfAbsent;
    using DB::Cas::Backend::putOverwrite;
    using DB::Cas::Backend::casPut;

    std::function<void()> before_put_if_absent;
    std::function<void()> before_get;
    std::function<void()> before_put_overwrite;

    DB::Cas::PutResult putIfAbsent(
        const String & key, const String & bytes, const DB::Cas::ObjectMeta & meta) override
    {
        fire(before_put_if_absent);
        return InMemoryBackend::putIfAbsent(key, bytes, meta);
    }

    std::optional<DB::Cas::GetResult> get(const String & key, DB::Cas::Range range) override
    {
        fire(before_get);
        return InMemoryBackend::get(key, range);
    }

    DB::Cas::PutResult putOverwrite(
        const String & key, const String & bytes, const DB::Cas::Token & expected,
        const DB::Cas::ObjectMeta & meta) override
    {
        fire(before_put_overwrite);
        return InMemoryBackend::putOverwrite(key, bytes, expected, meta);
    }

private:
    static void fire(std::function<void()> & hook)
    {
        if (!hook)
            return;
        auto once = std::move(hook);
        hook = nullptr;
        once();
    }
};
```

A hook that must remove the slot reads its token with `get` and calls `deleteExact` — `Backend` has no unconditional delete (`Backend/CasBackend.h:298`).

- [ ] **Step 2: Write the failing tests**

Create `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp` with eight tests. Each builds the slot state, constructs a `MountLeaseKeeper` the way `gtest_cas_mount.cpp:1062-1064` does, calls `start`, and asserts the code and a message substring. Use `expectThrowsCode` (`cas_test_helpers.h:145`) for the `ABORTED` cases and `EXPECT_THROW(…, MountFencedException)` for the two fenced ones.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h>
#include <Disks/tests/cas_test_helpers.h>

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

using namespace DB::Cas;
using DB::Cas::tests::MountSlotRaceBackend;
using DB::Cas::tests::expectThrowsCode;

namespace
{

/// One keeper for the mount slot of server-root "r", under (uuid=1, epoch=7) unless overridden.
MountLeaseKeeper makeKeeper(const std::shared_ptr<MountSlotRaceBackend> & b, uint64_t & now,
                            DB::UInt128 uuid = DB::UInt128(1), uint64_t epoch = 7)
{
    return MountLeaseKeeper(b, Layout("p"), "r", uuid, epoch, std::chrono::milliseconds(100),
                            [&now] { return now; }, [] { return uint64_t{0}; });
}

}

TEST(CasMountClaimConflicts, SlotAppearedBetweenHeadAndPutIfAbsent)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    /// Empty at `head`; another process mints it before our `putIfAbsent` lands.
    b->before_put_if_absent = [&] { claimMount(*b, l, "r", DB::UInt128(2), 1, now, /*ttl*/ 100); };
    auto k = makeKeeper(b, now);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { k.start(); });
}

TEST(CasMountClaimConflicts, SlotVanishedBetweenHeadAndGet)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(1), 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    b->before_get = [&]
    {
        const auto got = b->get(l.mountKey("r"));
        ASSERT_TRUE(got);
        b->deleteExact(l.mountKey("r"), got->token);
    };
    auto k = makeKeeper(b, now);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { k.start(); });
}

TEST(CasMountClaimConflicts, SlotHeldByForeignServer)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(2), 1, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    auto k = makeKeeper(b, now);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { k.start(); });
}

TEST(CasMountClaimConflicts, SlotHeldByDifferentWriterEpoch)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(1), 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    auto k = makeKeeper(b, now, DB::UInt128(1), /*epoch=*/8);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { k.start(); });
}

TEST(CasMountClaimConflicts, SlotChangedInsideAdoptionWindow)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(1), 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    /// Rewrite the slot under a NEW token after our `get`, so our adoption `putOverwrite` conflicts.
    b->before_put_overwrite = [&] { claimMount(*b, l, "r", DB::UInt128(1), 7, now + 1, /*ttl*/ 100); };
    auto k = makeKeeper(b, now);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { k.start(); });
}

TEST(CasMountClaimConflicts, SlotVanishedInsideAdoptionWindow)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(1), 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    b->before_put_overwrite = [&]
    {
        const auto got = b->get(l.mountKey("r"));
        ASSERT_TRUE(got);
        b->deleteExact(l.mountKey("r"), got->token);
    };
    auto k = makeKeeper(b, now);
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { k.start(); });
}

/// The two fenced branches keep their own type, and keep PRECEDENCE over the conflicts above: the
/// mount-open loop catches `MountFencedException` by type and recovers with a fresh writer epoch, so
/// a fence reported as a plain conflict would turn a recoverable state into a failed mount.
TEST(CasMountClaimConflicts, FencedBeforeAdoptionRaisesMountFenced)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(1), 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    DB::Cas::tests::markMountGcFenced(*b, l, "r");
    auto k = makeKeeper(b, now);
    EXPECT_THROW(k.start(), MountFencedException);
}

TEST(CasMountClaimConflicts, FencedInsideAdoptionWindowRaisesMountFencedNotAborted)
{
    auto b = std::make_shared<MountSlotRaceBackend>();
    Layout l("p");
    uint64_t now = 1000;
    ASSERT_EQ(claimMount(*b, l, "r", DB::UInt128(1), 7, now, /*ttl*/ 100).kind, MountClaimResult::Claimed);
    /// The slot changes inside the adoption window AND the new body is fenced: the fenced branch must
    /// win over the "changed while adopting" one.
    b->before_put_overwrite = [&] { DB::Cas::tests::markMountGcFenced(*b, l, "r"); };
    auto k = makeKeeper(b, now);
    EXPECT_THROW(k.start(), MountFencedException);
}
```

`markMountGcFenced` does not exist yet: write it in this file's anonymous namespace as a `get` of the mount key, `decodeMountLease`, set `gc_fenced = true`, `encodeMountLease`, `putOverwrite` with the observed token. Read `Pool/CasServerRoot.h` for the exact encode/decode names and the `MountLeaseKeeper` constructor's real parameter list before writing any of this, and correct the sketch above to match — it is written from the call at `gtest_cas_mount.cpp:1062-1064`, which may not carry every parameter.

- [ ] **Step 3: Run them and watch them fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_task3_red.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CasMountClaimConflicts.*' \
    > build_debug/test_task3_red.log 2>&1; echo "TEST_EXIT=$?"
```

Expected on a debug build: the six `ABORTED` tests **abort the process**, because `LOGICAL_ERROR` aborts there — the binary dies partway through and the two fenced tests may not run at all. That is the finding, reproduced. Note in the log summary which test aborted first; run the two fenced tests separately with `--gtest_filter='CasMountClaimConflicts.Fenced*'` to confirm they pass **before** the change, since they assert behaviour this task must preserve.

- [ ] **Step 4: Reclassify the six throws**

In `CasServerRoot.cpp`, change `ErrorCodes::LOGICAL_ERROR` to `ErrorCodes::ABORTED` at `:1468`, `:1479`, `:1489`, `:1499`, `:1525` and `:1530`. Leave every message string byte-for-byte unchanged. Do **not** touch `:1508` or `:1521`, and do not touch any other `LOGICAL_ERROR` in the file — `start` at `:1543`, `terminalResult` at `:1629`/`:1643` and `renew` at `:1661` are genuine state-machine invariants and stay as they are.

Confirm `ABORTED` is already in the file's `ErrorCodes` extern block (`:37`) — it is; no new declaration is needed.

- [ ] **Step 5: Run them and watch them pass**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_task3_green.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='CasMountClaimConflicts.*' \
    > build_debug/test_task3_green.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: `TEST_EXIT=0`, all eight passing, no abort.

- [ ] **Step 6: Retire the death test**

In `gtest_cas_mount.cpp:1069-1078`, delete the `EXPECT_DEATH` block and the `abort_on_logical_error` store that sets up the child process — `CasMountClaimConflicts.SlotHeldByDifferentWriterEpoch` now covers it. Keep the first half of `KeeperStartAdoptsOurOwnClaimNotDoubleStart` (`:1057-1068`), which asserts the successful adoption and is unrelated.

Then sweep the whole file:

```bash
grep -n "EXPECT_DEATH\|abort_on_logical_error\|EXPECT_THROW\|EXPECT_ANY_THROW" src/Disks/tests/gtest_cas_mount.cpp
```

For each hit, check the code raised at the site it exercises. A process abort hides every test behind it in the same binary, so a second stale death test would have been invisible while the first one existed.

- [ ] **Step 7: Confirm the untouched renewal tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./build_debug/unit_tests_dbms --gtest_filter='CASMount*:CasMount*:*Heartbeat*' \
    > build_debug/test_task3_mount.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: `TEST_EXIT=0`. `gtest_cas_pool.cpp:1824` and `gtest_cas_heartbeat.cpp:373` match on message substrings from the **renewal** path, which this task does not modify; they must pass unchanged. If either fails, a message string was edited — revert that edit.

- [ ] **Step 8: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
        src/Disks/tests/cas_test_helpers.h \
        src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp \
        src/Disks/tests/gtest_cas_mount.cpp
git commit -m "fix: CAS mount claim conflicts raise ABORTED, not LOGICAL_ERROR"
```

---

### Task 4: Gate both builds

**Files:** none — verification only.

**Interfaces:**
- Consumes: everything from Tasks 1 to 3.
- Produces: the evidence that the change is done.

**Context an implementer needs.** A debug-only run is not sufficient evidence for Task 3: its whole subject is behaviour that differs between release and sanitizer builds. A release-only run is not sufficient either, for the same reason in reverse.

- [ ] **Step 1: Debug gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_debug unit_tests_dbms > build_debug/build_gate.log 2>&1; echo "NINJA_EXIT=$?"
./build_debug/unit_tests_dbms --gtest_filter='Cas*:CA*' > build_debug/test_gate.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: both `0`. Summarize each log with a subagent. The summary must state the build marker explicitly — a green suite after a failed build is evidence about a different binary.

- [ ] **Step 2: ASan gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build_asan unit_tests_dbms > build_asan/build_gate.log 2>&1; echo "NINJA_EXIT=$?"
./build_asan/unit_tests_dbms --gtest_filter='Cas*:CA*' > build_asan/test_gate.log 2>&1; echo "TEST_EXIT=$?"
```

Expected: both `0`, and no sanitizer report anywhere in the test log. Search the log for `ERROR: AddressSanitizer` explicitly rather than trusting the exit code alone.

- [ ] **Step 3: Confirm the boundary holds**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
grep -rn "scheduleMetaJob" src/Disks/
grep -rn "LOGICAL_ERROR" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
```

Expected: the first prints nothing. The second prints only the state-machine sites at `:1543`, `:1629`, `:1643`, `:1661` and the two catalog-observer sites at `:649` and `:716` — no hit inside `claim` (`:1460-1537`).

- [ ] **Step 4: Confirm the commits**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git log --oneline -4
git log -3 --stat
```

Expected: three new commits on `cas-gc-rebuild`, each touching only the paths its task named. This checkout is shared with other sessions, so verify the contents rather than assuming. Do not push.

---

## Self-Review

**Spec coverage.** Every section of the spec maps to a task: `{#b1-fix}` to Task 1, `{#b1-round-drain}` to Task 2, `{#b2a-fix}` and `{#b2a-testing}` to Task 3, `{#gate}` to Task 4. `{#b1-dropped-jobs}` specifies behaviour that is deliberately unchanged and needs no task; it is recorded in the spec so a later reader does not assume otherwise. `{#b2a-audit}` is analysis that justifies Task 3's safety and produces no code.

**Placeholders.** Three steps deliberately tell the implementer to read something before writing: Task 1 Step 7 (the `idOf` / `u128Of` signatures), Task 2 Step 2 (the condemnable-blob setup) and Task 3 Step 2 (the `MountLeaseKeeper` constructor and the encode/decode names). These are not "figure it out later" — each names the exact file and line to read and what to reconcile the sketch against. Inventing those signatures here would be worse than pointing at them, because a plausible-but-wrong signature reads as authoritative.

**Type consistency.** `GcMetaWriter`'s methods are named identically in the header (Task 1 Step 1), the implementation (Step 2), the call-site table (Step 4) and both later tasks: `scheduleCondemnMarkerWrite`, `scheduleConfirmedMetaDelete`, `drain`, `scheduled`, `completed`, `noteCondemnMarkerDurable`, `condemnMarkerConfirmedInProcess`, `forgetCondemnMarker`. The test seam is `metaWriterForTest` in Step 3 and in both tests. `MetaWriteLatchBackend` exposes `entered` and `release` where Tasks 1 and 2 use them, and `OutcomeLogFaultBackend` derives from it so Task 2's test can use both.
