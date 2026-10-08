---
description: 'Implementation plan for reworking PR #2351: two round-scoped CAS GC thread pools sized by one setting, and a parallel pending_deletes fan-out with every failure logged and counted.'
sidebar_label: 'CAS GC round pools plan'
sidebar_position: 100
slug: /superpowers/plans/cas-gc-round-pools-and-parallel-redelete
title: 'CAS GC Round Pools and Parallel Redelete Implementation Plan'
doc_type: 'guide'
---

# CAS GC Round Pools and Parallel Redelete Implementation Plan {#cas-gc-round-pools-and-parallel-redelete-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework PR #2351 so the CAS GC owns no thread pools between rounds, one disk setting `cas_gc_concurrency` sizes two round-scoped pools, and the `pending_deletes` phase runs its `HEAD` plus conditional `DELETE` requests in parallel with every failure logged and counted.

**Architecture:** `Gc` stops owning `read_pool`, `redelete_pool` and `meta_writer`; a `GcRound` object created at the top of `runRegularRound` and `rebuildBaseline` owns an I/O `ThreadPool` and a per-round `GcMetaWriter` over a `GcMetaWriter::State` that `Gc` keeps across rounds. Read-ahead, the orphan-manifest sweep and `redeleteBlobs` submit to the I/O pool; `redeleteBlobs` uses `ThreadPoolCallbackRunnerLocal` with a bounded submission window and per-slot futures. The pre-commit barrier drains the meta pool and waits on the I/O pool in both round verbs.

**Tech Stack:** C++23, ClickHouse `ThreadPool` (`src/Common/ThreadPool.h`), `ThreadPoolCallbackRunnerLocal` (`src/Common/threadPoolCallbackRunner.h`), gtest (`src/Disks/tests/gtest_cas_*.cpp`, globbed into `unit_tests_dbms`), CAS settings framework (`ContentAddressedSettings.cpp`).

**Spec:** `docs/superpowers/specs/2026-09-14-cas-gc-round-pool-and-parallel-redelete-design.md` (revision 11). Read it first; every task cites the section it implements. Revisions 2 to 5 of this plan fold the Codex plan reviews (`tmp/review2351/codex/plan1_findings.md` to `plan4_findings.md`); the review loop stopped after revision 4, whose remaining findings were a compile slip and a guard nuance, both folded here.

## Global Constraints {#global-constraints}

- Branch: the PR's branch `cas/gc-parallel-delete-blobs` (Altinity/ClickHouse, base `antalya-26.6`). New commits only; never rebase or amend. Commit by explicit path: `git commit -- <paths>`.
- Anchors: `CasGc.cpp:NNN` and other GC file lines refer to the PR head tree (`8e0ec8de875`). Lines drift as you edit; every anchor below also quotes text to grep for. Files outside the GC directory are as in `antalya-26.6`.
- Allman braces; `f` not `f()` in prose and comments; literal names in backticks in docs and comments; say "exception", never "crash", for logical errors.
- Setting surface after this plan: exactly one new disk key `cas_gc_concurrency` (UInt64, default 16, must be at least 1). Removed keys: `cas_gc_read_concurrency`, `cas_gc_meta_pool_size`, `cas_gc_redelete_concurrency`, `cas_gc_redelete_min_batch_size`. No alias, no server-level key.
- ProfileEvents after this plan: new `CASGCRetiredRedeleteFailed`; removed `CASGCMetaWriteAnomaly`; no other anomaly counter. CurrentMetrics: new `CASGCIOPoolThreads`, `CASGCIOPoolThreadsActive`, `CASGCIOPoolThreadsScheduled`, `CASGCMetaPoolThreads`, `CASGCMetaPoolThreadsActive`, `CASGCMetaPoolThreadsScheduled`.
- Event outcome labels are `"deleted"`, `"absent"`, `"replaced"` (`removalName` in `CasGc.cpp:105`); tests assert those strings.
- Tests are gtests whose suite name starts with `CAS` (the CI gate filter is exactly `CAS*`). Failing-first: write the test, build, watch it fail for the stated reason, then implement. No `LOGICAL_ERROR`-expecting tests are introduced. There is no log-capture facility in the CAS unit tests; "logged" requirements are verified by counters in tests and by reading the test log once in Task 7.
- Build and run, always as one chained command so a failed build never runs a stale binary:
  `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='<filter>' > build/test_<name>.log 2>&1; echo EXIT=$?; grep -E '^\[  (PASSED|FAILED)  \]' build/test_<name>.log`
  A PASSED line must name a non-zero test count; `[  PASSED  ] 0 tests.` means the filter matched nothing. Before pushing: the `CAS*` filter green under an ASan build and a TSan build of the exact tree being pushed.
- Test hooks live in `PoolConfig` (the existing `*_hook_for_test` fields at `Pool/CasPool.h:189-209`) and are set before `Pool::open`. Hooks capture only heap state (`std::shared_ptr`) or atomics that outlive every round of the test; never a test-frame local by reference.
- Do not touch `blobUploadPool`, GC protocol, budgets, lease handling, or outcome ordering.

---

## File Structure {#file-structure}

| File | Responsibility after this plan |
|---|---|
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp` | declares `gc_concurrency`; validates it; no longer declares the four removed keys |
| `.../ContentAddressedMetadataStorage.{h,cpp}` | copies `gc_concurrency` into `PoolConfig` |
| `.../Pool/CasPool.h` | `PoolConfig::gc_concurrency`; test hooks `gc_redelete_apply_hook_for_test`, `gc_io_pool_framework_fault_for_test`, `gc_io_pool_refuse_at_for_test` |
| `.../Gc/CasGcMetaWriter.{h,cpp}` | `GcMetaWriter` over an externally owned `State`; fallback only on `CANNOT_SCHEDULE_TASK`; completion guard; `CASGCMetaPool*` metrics; raw-job test seam |
| `.../Gc/CasGcRound.h` (new) | `GcRound`: I/O pool + per-round meta writer; destructor body order; `concurrency()`; `waitIoPoolAndRethrow` |
| `.../Gc/CasGc.{h,cpp}` | `Gc` owns `meta_state` only; round verbs construct `GcRound`; barrier waits both pools; `redeleteBlobs` rework; `RedeleteRoundContext`; new `RoundReport` fields |
| `.../Gc/CasGcScheduler.{h,cpp}` | round log record carries `entries_redelete_failed`, `gc_concurrency` |
| `src/Interpreters/ContentAddressedGarbageCollectionLog.{h,cpp}`, `src/Interpreters/InterpreterSystemQuery.cpp` | new columns `entries_redelete_failed`, `gc_concurrency` |
| `src/Common/ProfileEvents.cpp`, `src/Common/CurrentMetrics.cpp`, `src/Common/setThreadName.h` | counters, metrics, `CAS_GC_REDELETE` thread name |
| `src/Disks/tests/gtest_cas_settings.cpp` | settings tests |
| `src/Disks/tests/gtest_cas_gc_meta_writer.cpp` | writer tests over an owned `State`, including test 3a |
| `src/Disks/tests/gtest_cas_gc_round_pools.cpp` (new) | tests 1, 2, 3, 4, 4a |
| `src/Disks/tests/gtest_cas_gc_redelete_concurrency.cpp` | tests 5 to 10, 9a, 9b |
| `src/Disks/tests/gtest_cas_gc_read_ahead.cpp`, `gtest_cas_ref_gc.cpp`, `gtest_cas_gc_log.cpp` | switch to `gc_concurrency`; log expectations |
| `docs/en/operations/storing-data.md`, `docs/en/antalya/cas/configuration.md`, `docs/en/antalya/cas/architecture/garbage-collection.md`, `docs/en/antalya/cas/operations/monitoring.md` | settings row, migration note and failure sentence on each of the three pages; anomaly row removal |

---

### Task 1: One setting, `cas_gc_concurrency` (spec 3.2) {#task-1-one-setting-cas-gc-concurrency-spec-3-2}

**Files:**
- Modify: `ContentAddressedSettings.cpp` DECLARE block (grep `DECLARE(UInt64, gc_read_concurrency`) and `validate` (grep `cas_gc_shards and cas_gc_read_concurrency must be >= 1`)
- Modify: `ContentAddressedMetadataStorage.h` (grep `const uint64_t gc_read_concurrency;`), `ContentAddressedMetadataStorage.cpp` (three sites: the `extern const ContentAddressedSettingsUInt64 gc_read_concurrency;` block, the constructor initializer `gc_read_concurrency(settings_[`, and `pool_config.gc_read_concurrency =` in `openPool`)
- Modify: `Pool/CasPool.h` (grep `uint64_t gc_read_concurrency = 16;` and `uint64_t gc_meta_pool_size = 16;` and the two `gc_redelete_*` fields the PR added)
- Test: `src/Disks/tests/gtest_cas_settings.cpp`
- Modify tests: `gtest_cas_gc_read_ahead.cpp` (three designated initializers `.gc_read_concurrency =`), `gtest_cas_ref_gc.cpp:200` (a comment), `gtest_cas_gc_redelete_concurrency.cpp` (`config.gc_redelete_concurrency = concurrency;` in the PR's `openPoolWithRedeleteConcurrency`; the file is rewritten in Task 4, this keeps it compiling now)

**Interfaces:**
- Produces: `PoolConfig::gc_concurrency` (`uint64_t`, default 16); `ContentAddressedSetting::gc_concurrency`; `ContentAddressedMetadataStorage::gc_concurrency` (`const uint64_t`). Later tasks read `store->poolConfig().gc_concurrency`.
- Removes the four old `PoolConfig` fields. Compile bridges until Tasks 3 and 4: in `CasGc.cpp` read `gc_concurrency` at the constructor's three pool constructions (grep `read_pool = std::make_unique<ThreadPool>` and `redelete_pool = std::make_unique<ThreadPool>` and `make_unique<GcMetaWriter>`), at the two `GcReadAhead reads(op, ...)` sites and the `planManifestCursorPage(` call (`read_pool.get(), store->poolConfig().gc_read_concurrency`), and replace `store->poolConfig().gc_redelete_min_batch_size` in `redeleteBlobs` with the literal `2`.

- [ ] **Step 1: Write the failing settings tests**

In `gtest_cas_settings.cpp` the PR branch (not `antalya-26.6`) contains `TEST(CASSettings, RedeleteConcurrencyBoundsAreEnforced)` and `TEST(CASSettings, RedeleteMinBatchSizeBoundsAreEnforced)`; delete both and add:

```cpp
TEST(CASSettings, GcConcurrencyIsTheOnlyPoolSetting)
{
    {
        auto cfg = makeConfig("<cas_server_root_id>srv1</cas_server_root_id>");
        ContentAddressedSettings s;
        s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros);
        EXPECT_EQ(s[ContentAddressedSetting::gc_concurrency].value, 16u);
    }
    expectLoadFailureWithExactMessage(
        "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_concurrency>0</cas_gc_concurrency>",
        ErrorCodes::BAD_ARGUMENTS,
        "content_addressed disk: cas_gc_interval_sec, cas_gc_shards and cas_gc_concurrency must be >= 1 "
        "(got 60, 1, 0)");
    {
        auto cfg = makeConfig(
            "<cas_server_root_id>srv1</cas_server_root_id><cas_gc_concurrency>4</cas_gc_concurrency>");
        ContentAddressedSettings s;
        EXPECT_NO_THROW(s.loadFromConfig(*cfg, "disk", "/scratch", "/scratch", identity_macros));
        EXPECT_EQ(s[ContentAddressedSetting::gc_concurrency].value, 4u);
    }
}

TEST(CASSettings, RemovedPoolSettingsAreUnknown)
{
    for (const char * key : {"cas_gc_read_concurrency", "cas_gc_meta_pool_size",
                             "cas_gc_redelete_concurrency", "cas_gc_redelete_min_batch_size"})
    {
        expectLoadFailureWithExactMessage(
            fmt::format("<cas_server_root_id>srv1</cas_server_root_id><{0}>8</{0}>", key),
            ErrorCodes::UNKNOWN_SETTING,
            fmt::format("Unknown setting '{}'", key));
    }
}
```

(`expectLoadFailureWithExactMessage` and the `UNKNOWN_SETTING` message shape are the ones `TEST(CASContentAddressedSettings, UnknownKeyRejected)` already uses.) Fix the two existing expectations that quote `cas_gc_shards and cas_gc_read_concurrency must be >= 1` to say `cas_gc_concurrency`, and the snippet `<cas_gc_read_concurrency>0</cas_gc_read_concurrency>` to `<cas_gc_concurrency>0</cas_gc_concurrency>`. In the `namespace DB::ContentAddressedSetting` extern block at the top, add `extern const ContentAddressedSettingsUInt64 gc_concurrency;` and delete the externs of the four removed keys. The literal `60, 1` is `gc_interval_sec`'s and `gc_shards`'s defaults (`DECLARE(UInt64, gc_interval_sec, 60,` and `DECLARE(UInt64, gc_shards, 1,`); confirm before pasting.

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CASSettings.*' > build/test_settings.log 2>&1; echo EXIT=$?; tail -5 build/build.log`
Expected: build fails, `gc_concurrency` is not a member of `ContentAddressedSetting`.

- [ ] **Step 3: Declare, validate, plumb**

`ContentAddressedSettings.cpp` DECLARE block: delete `gc_meta_pool_size`, `gc_read_concurrency`, `gc_redelete_concurrency`, `gc_redelete_min_batch_size`; add

```cpp
    DECLARE(UInt64, gc_concurrency, 16, "Worker count of each of the two per-round GC thread pools: the I/O pool (fold read-ahead, orphan-manifest sweep, pending_deletes HEAD+conditional-DELETE fan-out) and the meta-write pool. A pool-worker limit, not a limit on the round's simultaneous requests; 1 runs every fan-out one job at a time. At least 1", 0) \
```

`validate`: first check becomes

```cpp
    if (settings[ContentAddressedSetting::gc_interval_sec] == 0 || settings[ContentAddressedSetting::gc_shards] == 0
        || settings[ContentAddressedSetting::gc_concurrency] == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "content_addressed disk: cas_gc_interval_sec, cas_gc_shards and cas_gc_concurrency must be >= 1 "
            "(got {}, {}, {})",
            settings[ContentAddressedSetting::gc_interval_sec].value, settings[ContentAddressedSetting::gc_shards].value,
            settings[ContentAddressedSetting::gc_concurrency].value);
```

Delete the two `gc_redelete_*` checks the PR added.

`ContentAddressedMetadataStorage.h`: replace the four members with

```cpp
    /// Worker count of each of the two per-round GC pools (I/O and meta writes); see `PoolConfig::gc_concurrency`.
    const uint64_t gc_concurrency;
```

`ContentAddressedMetadataStorage.cpp`: keep only `extern const ContentAddressedSettingsUInt64 gc_concurrency;` of the four; initializer `, gc_concurrency(settings_[ContentAddressedSetting::gc_concurrency].value)`; in `openPool`, `pool_config.gc_concurrency = gc_concurrency;`.

`Pool/CasPool.h`: replace the four fields with

```cpp
    /// Worker count of EACH of the two pools a GC round creates and destroys (`GcRound`): the I/O pool
    /// (fold read-ahead, orphan-manifest sweep, `pending_deletes` fan-out) and the meta-write pool.
    /// This bounds pool workers, not the round's simultaneous requests: the round thread's own inline
    /// reads run outside both pools. `1` gives each pool one worker, so every fan-out runs one job at a
    /// time. No pool thread exists between rounds.
    uint64_t gc_concurrency = 16;
```

Apply the compile bridges listed under Interfaces and the three test-file renames.

- [ ] **Step 4: Run**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CASSettings.*:CASContentAddressedSettings.*:CASGCReadAhead.*:CASGCRedeleteConcurrency.*' > build/test_task1.log 2>&1; echo EXIT=$?; grep -E '^\[  (PASSED|FAILED)  \]' build/test_task1.log`
Expected: EXIT=0 and a PASSED line with a non-zero count.

- [ ] **Step 5: Commit**

```bash
git commit -m "cas: one GC pool setting, cas_gc_concurrency, replaces four per-pool keys" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/tests/gtest_cas_settings.cpp src/Disks/tests/gtest_cas_gc_read_ahead.cpp \
  src/Disks/tests/gtest_cas_ref_gc.cpp src/Disks/tests/gtest_cas_gc_redelete_concurrency.cpp
```

---

### Task 2: `GcMetaWriter` over an external `State`, fallback only on `CANNOT_SCHEDULE_TASK` (spec 3.1, 3.5, test 3a) {#task-2-gcmetawriter-over-an-external-state-fallback-only-on-cannot-schedule-task-spec-3-1-3-5-test-3a}

**Files:**
- Modify: `Gc/CasGcMetaWriter.h` (whole class declaration) and `Gc/CasGcMetaWriter.cpp` (`extern const Event CASGCMetaWriteAnomaly;` at line 13; constructor at `GcMetaWriter::GcMetaWriter(PoolPtr store_`; `GcMetaWriter::submit`)
- Modify: `src/Common/CurrentMetrics.cpp` (append after `M(CASBlobUploadPoolThreadsScheduled,`)
- Modify: `src/Common/ProfileEvents.cpp` (delete the `M(CASGCMetaWriteAnomaly,` line)
- Modify: `Gc/CasGc.cpp`: delete `extern const Event CASGCMetaWriteAnomaly;` from the `ProfileEvents` extern block near the top, and delete the single statement `ProfileEvents::increment(ProfileEvents::CASGCMetaWriteAnomaly);` inside the `catch (...)` that logs `condemn-marker re-check failed to read the meta` (around `CasGc.cpp:2017`); keep the `tryLogCurrentException` and the `CASGCCondemnMarkerUnconfirmedCarry` increment that follow it
- Modify: `docs/en/antalya/cas/operations/monitoring.md:47` (delete the `CASGCMetaWriteAnomaly` table row)
- Test: `src/Disks/tests/gtest_cas_gc_meta_writer.cpp`

**Interfaces:**
- Produces:
  ```cpp
  class GcMetaWriter
  {
  public:
      struct State;                      // public; fields unchanged
      using StatePtr = std::shared_ptr<State>;
      static StatePtr makeState(PoolPtr store, LoggerPtr logger);
      GcMetaWriter(StatePtr state_, size_t pool_size);   // the only constructor; destructor drains
      LoggerPtr logger() const;
      // unchanged: scheduleCondemnMarkerWrite, scheduleConfirmedMetaDelete, drain, drainOnExitNoThrow,
      //            scheduled, completed, noteCondemnMarkerDurable, condemnMarkerConfirmedInProcess, forgetCondemnMarker
      // test seams:
      void injectNextScheduleFailureForTest(std::exception_ptr e);   // the next submit sees scheduleOrThrowOnError throw e
      void submitForTest(std::function<void()> op);                  // calls submit
      void submitRawJobForTest(std::function<void()> job);           // pool.scheduleOrThrowOnError(job) with no wrapper and no counting
  };
  ```
- Consumed by Task 3.

- [ ] **Step 1: Write the failing tests**

Rewrite `gtest_cas_gc_meta_writer.cpp`. Keep the header block and `kGcId`; add after the existing `using` lines:

```cpp
using DB::Exception;
namespace ErrorCodes = DB::ErrorCodes;

namespace
{
GcMetaWriter::StatePtr stateFor(const PoolPtr & store)
{
    return GcMetaWriter::makeState(store, getLogger("CasGcMetaWriterTest"));
}
}
```

and add the extern block `namespace DB::ErrorCodes { extern const int CANNOT_SCHEDULE_TASK; extern const int NETWORK_ERROR; }` above it. Rewrite the four existing tests to build the writer directly (`GcMetaWriter writer(stateFor(store), 4);`) instead of `gc->metaWriterForTest()`; the `ThrowingRoundDrainsBeforeReturning` test moves to Task 3 (it needs a round). Add:

```cpp
TEST(CASGcMetaWriter, StateOutlivesTheWriter)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto state = stateFor(store);
    const BlobRef ref = DB::Cas::tests::idOf("1");
    const PersistedEtag token{"emulated", "tok-1"};
    {
        GcMetaWriter writer(state, 4);
        writer.scheduleCondemnMarkerWrite(ref, token, /*condemn_round=*/1, /*size=*/128);
        writer.drain();
        EXPECT_TRUE(writer.condemnMarkerConfirmedInProcess(ref, token));
    }
    GcMetaWriter second(state, 4);
    EXPECT_TRUE(second.condemnMarkerConfirmedInProcess(ref, token));
    EXPECT_EQ(second.scheduled(), second.completed());
}

TEST(CASGcMetaWriter, SubmitDistinguishesRefusalFromWorkerFailure)
{
    auto backend = std::make_shared<MetaWriteLatchBackend>();
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    auto state = stateFor(store);

    /// (a) A refusal: the operation runs inline on the calling thread and is counted.
    {
        GcMetaWriter writer(state, 1);
        writer.injectNextScheduleFailureForTest(std::make_exception_ptr(
            Exception(ErrorCodes::CANNOT_SCHEDULE_TASK, "injected refusal")));
        const std::thread::id here = std::this_thread::get_id();
        auto ran_here = std::make_shared<std::atomic<bool>>(false);
        writer.submitForTest([ran_here, here] { ran_here->store(std::this_thread::get_id() == here); });
        EXPECT_TRUE(ran_here->load());
        EXPECT_EQ(writer.scheduled(), writer.completed());
    }

    /// (b) A real pending worker exception: a raw job lets a NETWORK_ERROR escape its body, the pool
    /// (default shutdown_on_exception) stores it as first_exception; the next submit's
    /// scheduleOrThrowOnError rethrows it, and submit must propagate it unchanged, not run inline.
    /// Accounting convention: the raw job bypasses the writer's counters entirely, so the only
    /// operation submit saw is the rejected one, which is counted; scheduled == completed holds.
    {
        GcMetaWriter writer(state, 1);
        writer.submitRawJobForTest([] { throw Exception(ErrorCodes::NETWORK_ERROR, "injected escaped worker exception"); });
        /// Let the raw job finish before the next submit so the pool has recorded it.
        while (CurrentMetrics::get(CurrentMetrics::CASGCMetaPoolThreadsScheduled) != 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        auto ran = std::make_shared<std::atomic<bool>>(false);
        try
        {
            writer.submitForTest([ran] { ran->store(true); });
            FAIL() << "submit must propagate the pending worker exception";
        }
        catch (const Exception & e)
        {
            EXPECT_EQ(e.code(), ErrorCodes::NETWORK_ERROR);
        }
        EXPECT_FALSE(ran->load());
        EXPECT_EQ(writer.scheduled(), writer.completed());
    }
}
```

Add `#include <Common/CurrentMetrics.h>` and `namespace CurrentMetrics { extern const Metric CASGCMetaPoolThreadsScheduled; }`. The sleep loop is a wait on a pool metric, not a race fix: the raw job must have finished for the pool to hold `first_exception`, and there is no other completion signal for an uncounted job.

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1; echo EXIT=$?; tail -5 build/build.log`
Expected: build fails on `makeState`, `injectNextScheduleFailureForTest`, `submitRawJobForTest`.

- [ ] **Step 3: Implement**

`CasGcMetaWriter.h`, full class:

```cpp
class GcMetaWriter
{
public:
    struct State
    {
        PoolPtr store;
        LoggerPtr logger;
        std::atomic<uint64_t> scheduled{0};
        std::atomic<uint64_t> completed{0};
        std::mutex condemn_marker_mutex;
        std::set<std::pair<BlobRef, String>> condemn_markers_confirmed;
        void noteCondemnMarkerDurable(const BlobRef & ref, const PersistedEtag & token);
        bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const PersistedEtag & token);
        void forgetCondemnMarker(const BlobRef & ref, const PersistedEtag & token);
    };
    using StatePtr = std::shared_ptr<State>;

    /// The cross-round state a `Gc` owns; a writer is built over it for the duration of one round.
    static StatePtr makeState(PoolPtr store, LoggerPtr logger);

    /// `pool_size` workers and a queue depth equal to `pool_size` (the bounded queue is the writer's
    /// backpressure: `submit` blocks while `pool_size` jobs are queued or running).
    GcMetaWriter(StatePtr state_, size_t pool_size);
    GcMetaWriter(const GcMetaWriter &) = delete;
    GcMetaWriter & operator=(const GcMetaWriter &) = delete;
    ~GcMetaWriter() { drainOnExitNoThrow(); }

    LoggerPtr logger() const { return state->logger; }

    void scheduleCondemnMarkerWrite(const BlobRef & ref, const PersistedEtag & token, uint64_t condemn_round, uint64_t size);
    void scheduleConfirmedMetaDelete(const BlobRef & ref);
    void drain();
    void drainOnExitNoThrow() noexcept;
    uint64_t scheduled() const;
    uint64_t completed() const;
    void noteCondemnMarkerDurable(const BlobRef & ref, const PersistedEtag & token);
    bool condemnMarkerConfirmedInProcess(const BlobRef & ref, const PersistedEtag & token);
    void forgetCondemnMarker(const BlobRef & ref, const PersistedEtag & token);

    /// Test seams (inert in production).
    void injectNextScheduleFailureForTest(std::exception_ptr e) { injected_schedule_failure = std::move(e); }
    void submitForTest(std::function<void()> op) { submit(std::move(op)); }
    /// Schedules `job` on the pool with no wrapper and no counting: the way to make a job let an
    /// exception escape its body, which no production job does.
    void submitRawJobForTest(std::function<void()> job) { pool.scheduleOrThrowOnError(std::move(job)); }

private:
    void submit(std::function<void()> op);
    StatePtr state;
    ThreadPool pool;
    std::exception_ptr injected_schedule_failure;
};
```

`CasGcMetaWriter.cpp`: delete the `CASGCMetaWriteAnomaly` extern and both increments; add

```cpp
namespace CurrentMetrics
{
    extern const Metric CASGCMetaPoolThreads;
    extern const Metric CASGCMetaPoolThreadsActive;
    extern const Metric CASGCMetaPoolThreadsScheduled;
}
namespace DB::ErrorCodes
{
    extern const int CANNOT_SCHEDULE_TASK;
}
```

and replace the constructor and `submit`:

```cpp
GcMetaWriter::StatePtr GcMetaWriter::makeState(PoolPtr store, LoggerPtr logger)
{
    auto state = std::make_shared<State>();
    state->store = std::move(store);
    state->logger = std::move(logger);
    return state;
}

GcMetaWriter::GcMetaWriter(StatePtr state_, size_t pool_size)
    : state(std::move(state_))
    , pool(CurrentMetrics::CASGCMetaPoolThreads, CurrentMetrics::CASGCMetaPoolThreadsActive,
           CurrentMetrics::CASGCMetaPoolThreadsScheduled, std::max<size_t>(1, pool_size))
{
}

void GcMetaWriter::submit(std::function<void()> op)
{
    /// The per-hash meta operation is advisory: its own failure is logged and swallowed (the
    /// `BackgroundSchedulePool` idiom). `completed` is incremented from a scope guard entered BEFORE
    /// the operation, so a throwing operation or a throwing diagnostic still counts; `drain` waits on
    /// the pool and the round's `meta_pool_wait` row reads these counters.
    auto run = [op, st = state]()
    {
        SCOPE_EXIT({ st->completed.fetch_add(1, std::memory_order_relaxed); });
        ProfileEvents::increment(ProfileEvents::CASGCMetaOps);
        try
        {
            op();
        }
        catch (...)
        {
            tryLogCurrentException(st->logger,
                "CAS gc: a per-hash freshness-meta op failed on the meta pool (advisory-only; never wedges the round)");
        }
    };
    state->scheduled.fetch_add(1, std::memory_order_relaxed);
    try
    {
        if (injected_schedule_failure)
        {
            std::exception_ptr e;
            std::swap(e, injected_schedule_failure);
            std::rethrow_exception(e);
        }
        pool.scheduleOrThrowOnError(run);
        return;
    }
    catch (const Exception & e)
    {
        /// Only a refusal takes the inline fallback. `scheduleOrThrowOnError` rethrows a pending worker
        /// exception (clearing the pool's record of it) BEFORE it would report a refusal; that is a
        /// framework failure and must reach the round's barrier, so it propagates instead of being
        /// mistaken for "the queue is full". The rejected operation never runs: count it completed so
        /// the writer's drain accounting stays balanced.
        if (e.code() != ErrorCodes::CANNOT_SCHEDULE_TASK)
        {
            state->completed.fetch_add(1, std::memory_order_relaxed);
            throw;
        }
    }
    catch (...)
    {
        state->completed.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
    /// Refused: run inline rather than lose the meta write. Nothing that can throw sits between the
    /// refusal and this call; `run` carries its own completion guard.
    run();
}
```

`drain` and `drainOnExitNoThrow` stay as they are.

`CurrentMetrics.cpp` after the `CASBlobUploadPoolThreadsScheduled` line:

```cpp
    M(CASGCIOPoolThreads, "Number of threads in a CAS GC round's I/O pool (fold read-ahead, orphan-manifest sweep, pending_deletes fan-out). Zero between rounds.") \
    M(CASGCIOPoolThreadsActive, "Number of threads in a CAS GC round's I/O pool running a job.") \
    M(CASGCIOPoolThreadsScheduled, "Number of queued or running jobs in a CAS GC round's I/O pool.") \
    M(CASGCMetaPoolThreads, "Number of threads in a CAS GC round's meta-write pool. Zero between rounds.") \
    M(CASGCMetaPoolThreadsActive, "Number of threads in a CAS GC round's meta-write pool running a job.") \
    M(CASGCMetaPoolThreadsScheduled, "Number of queued or running jobs in a CAS GC round's meta-write pool.") \
```

`ProfileEvents.cpp`: delete the `CASGCMetaWriteAnomaly` line. `CasGc.cpp`: the two deletions listed under Files, plus the compile bridge in the constructor (deleted in Task 3): `meta_writer = std::make_unique<GcMetaWriter>(GcMetaWriter::makeState(store, logger), static_cast<size_t>(store->poolConfig().gc_concurrency));`. `monitoring.md`: delete the row.

- [ ] **Step 4: Run**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CASGcMetaWriter.*:CASGCReadAhead.*' > build/test_task2.log 2>&1; echo EXIT=$?; grep -E '^\[  (PASSED|FAILED)  \]' build/test_task2.log; git grep -n CASGCMetaWriteAnomaly -- src docs/en tests | wc -l`
Expected: EXIT=0, PASSED with a non-zero count, `0` references left in code, tests and user docs (the spec and this plan keep historical mentions).

- [ ] **Step 5: Commit**

```bash
git commit -m "cas: GcMetaWriter over an external State; inline fallback only on CANNOT_SCHEDULE_TASK; drop CASGCMetaWriteAnomaly" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Common/CurrentMetrics.cpp src/Common/ProfileEvents.cpp \
  docs/en/antalya/cas/operations/monitoring.md src/Disks/tests/gtest_cas_gc_meta_writer.cpp
```

---

### Task 3: `GcRound`, two pools per round, none per disk (spec 3.1, tests 1 to 4a) {#task-3-gcround-two-pools-per-round-none-per-disk-spec-3-1-tests-1-to-4a}

**Files:**
- Create: `Gc/CasGcRound.h`
- Modify: `Gc/CasGc.h`: the members `std::unique_ptr<GcMetaWriter> meta_writer;`, `std::unique_ptr<ThreadPool> read_pool;`, `std::unique_ptr<ThreadPool> redelete_pool;`; the accessor `GcMetaWriter & metaWriterForTest()`; the `fold` declaration; the `redeleteBlobs` declaration
- Modify: `Gc/CasGc.cpp`: constructor (grep `meta_writer = std::make_unique`), `runRegularRound` (grep `std::optional<Etag> state_etag;` near its top, `SCOPE_EXIT({ meta_writer->drainOnExitNoThrow(); });`, `const uint64_t meta_jobs_scheduled_at_round_start`, `FoldResult folded = fold(`, the `meta_pool_wait` block), `fold` (grep `GcReadAhead reads(op, store->openRequests(), *read_pool`), the sweep call (grep `&work_budget, read_pool.get()`), `redeleteBlobs` (grep `redelete_pool->`), `rebuildBaseline` (grep `RebuildReport Gc::rebuildBaseline`, its `GcReadAhead reads(`, and `WriteResult commit = op.replace(layout.gcStateKey()`)
- Modify: `Pool/CasPool.h` `PoolConfig` (after `teardown_phase1_throw_for_test`)
- Test: `src/Disks/tests/gtest_cas_gc_round_pools.cpp` (new)

**Interfaces:**
- Consumes: `GcMetaWriter::StatePtr`, `GcMetaWriter(StatePtr, size_t)`, `submitRawJobForTest` (Task 2); `PoolConfig::gc_concurrency` (Task 1).
- Produces:
  ```cpp
  namespace DB::Cas
  {
  class GcRound
  {
  public:
      GcRound(GcMetaWriter::StatePtr meta_state, size_t concurrency_);
      ~GcRound();                                   // body: meta_writer.drainOnExitNoThrow(); guarded io_pool.wait()
      ThreadPool & ioPool();
      GcMetaWriter & metaWriter();
      size_t concurrency() const;
      void waitIoPoolAndRethrow();                  // io_pool.wait(): the barrier's I/O half
  };
  }
  ```
  `PoolConfig` gains `std::function<void()> gc_redelete_apply_hook_for_test = {};`, `std::shared_ptr<std::atomic<bool>> gc_io_pool_framework_fault_for_test = {};` (a shared flag so a test can arm it AFTER its fixture rounds; null = off), `size_t gc_io_pool_refuse_at_for_test = 0;` (0 = off; k = the k-th enqueue in a `redeleteBlobs` call is refused with `CANNOT_SCHEDULE_TASK`). `Gc` gains `GcMetaWriter::StatePtr meta_state;` and `GcMetaWriter::State & metaStateForTest() { return *meta_state; }`; loses the three pool members and `metaWriterForTest`. `Gc::fold(..., GcRound & round)`. Transitional `redeleteBlobs(entries, layout, ThreadPool & pool, op, new_round, generation, budget, report, outcome_log, GcMetaWriter & meta_writer)` and `applyRedeleteOutcome(..., GcMetaWriter & meta_writer)` until Task 4 replaces them; the PR's one-call-site wrapper `redeleteBlob` is deleted in this task (its loop body is inlined into `redeleteBlobs`), so nothing keeps calling the old `applyRedeleteOutcome` signature.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_gc_round_pools.cpp`:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

namespace CurrentMetrics
{
    extern const Metric CASGCIOPoolThreads;
    extern const Metric CASGCMetaPoolThreads;
}
namespace DB::ErrorCodes
{
    extern const int ABORTED;
    extern const int NETWORK_ERROR;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;
using DB::Exception;
namespace ErrorCodes = DB::ErrorCodes;

namespace
{

const DB::UInt128 kGc = hexToU128("00000000000000000000000000000001");
constexpr uint64_t kBlobs = 6;

String blobKeyOf(const Pool & store, uint64_t blob)
{
    return store.layout().blobKey(BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(blob))});
}

/// Watches `remove` on the fixture's blob keys only (meta objects and other keys are ignored), so
/// overlap and thread identity speak about the `pending_deletes` fan-out and nothing else. Each
/// watched remove waits up to 250 ms for a second watched remove to be in flight, so overlapping
/// callers are observed overlapping while a sequential caller costs at most 250 ms per blob.
class BlobRemoveWitness : public InMemoryBackend
{
public:
    void watch(std::set<String> keys, size_t k_overlap_) { watched = std::move(keys); k_overlap = k_overlap_; owner = std::this_thread::get_id(); }
    RawRemoval remove(const String & key, const String & expected, TransportAccess & access) override
    {
        if (!watched.contains(key))
            return InMemoryBackend::remove(key, expected, access);
        {
            std::unique_lock lock(mutex);
            ++in_flight;
            peak = std::max(peak, in_flight);
            if (std::this_thread::get_id() != owner)
                ++on_pool_threads;
            gate.wait_for(lock, std::chrono::milliseconds(250), [&] { return in_flight >= k_overlap; });
            gate.notify_all();
        }
        RawRemoval r = InMemoryBackend::remove(key, expected, access);
        {
            std::lock_guard lock(mutex);
            --in_flight;
        }
        return r;
    }
    size_t peakInFlight() const { std::lock_guard l(mutex); return peak; }
    size_t removesOnPoolThreads() const { std::lock_guard l(mutex); return on_pool_threads; }
private:
    std::set<String> watched;
    size_t k_overlap = 2;
    std::thread::id owner;
    mutable std::mutex mutex;
    std::condition_variable gate;
    size_t in_flight = 0, peak = 0, on_pool_threads = 0;
};

PoolPtr openPool(std::shared_ptr<Backend> backend, uint64_t concurrency, PoolConfig extra = {})
{
    PoolConfig config = std::move(extra);
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.gc_concurrency = concurrency;
    return Pool::open(std::move(backend), std::move(config));
}

std::set<String> blobKeys(const Pool & store)
{
    std::set<String> keys;
    for (uint64_t b = 1; b <= kBlobs; ++b)
        keys.insert(blobKeyOf(store, b));
    return keys;
}

/// Publishes kBlobs blobs under one manifest, runs one round so they are live, then drops the ref.
void publishThenDrop(Backend & backend, const PoolPtr & store, Gc & gc)
{
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    std::vector<ManifestEntry> entries;
    for (uint64_t b = 1; b <= kBlobs; ++b)
    {
        writeBlobBody(backend, store->layout(), DB::UInt128(b));
        entries.push_back(blobEntryFor("f" + std::to_string(b), DB::UInt128(b)));
    }
    writeManifestRaw(backend, store->layout(), ns, r, entries);
    publishCommittedTransition(backend, store->layout(), ns, "tbl", std::nullopt, r);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(backend, store->layout(), ns, "tbl", r);
}

/// Runs rounds until `previewDeletes` shows at least one `delete_pending` entry: the next round will
/// redelete. Fixtures that must arm a fault only in the redelete phase call this first.
void runUntilDeletePending(Gc & gc, const PoolPtr & store)
{
    for (int i = 0; i < 16; ++i)
    {
        for (const auto & e : gc.previewDeletes())
            if (e.reason == "delete_pending")
                return;
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    FAIL() << "no round reached a pending delete; the fixture is wrong";
}

bool allBlobsAbsent(Backend & backend, const Pool & store)
{
    for (uint64_t b = 1; b <= kBlobs; ++b)
        if (!blobAbsent(backend, store.layout(), DB::UInt128(b)))
            return false;
    return true;
}

/// Per-entry records in apply order. `applyRedeleteOutcome` builds the audit row and the event from the
/// same (ref, token, outcome) triple in the same statement, so the ordered event records are the audit
/// rows' content in the audit rows' order (the outcome objects themselves are zstd-compressed control
/// objects; reading them back here would test the codec, not the fan-out).
struct RunSummary
{
    std::vector<std::tuple<String, String, String>> records;   /// (object_hash, token, outcome) per BlobDelete event, in order
    uint64_t deleted = 0, absent = 0, replaced = 0, redeleted = 0;
};

RunSummary reclaimAndSummarise(Gc & gc, const PoolPtr & store, Backend & backend, const std::shared_ptr<SharedEventLog> & events)
{
    RunSummary s;
    for (int i = 0; i < 8 && !allBlobsAbsent(backend, *store); ++i)
    {
        const RoundReport rep = runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        s.deleted += rep.deleted; s.absent += rep.absent; s.replaced += rep.replaced; s.redeleted += rep.redeleted;
    }
    for (const CasEvent & e : events->snapshot())
        if (e.type == CasEventType::BlobDelete)
            s.records.emplace_back(e.object_hash, e.token, e.outcome);
    return s;
}

}

TEST(CASGCRoundPools, NoThreadsBetweenRoundsAndRedeleteRunsOnPoolWorkers)
{
    auto backend = std::make_shared<BlobRemoveWitness>();
    auto store = openPool(backend, 4);
    backend->watch(blobKeys(*store), /*k_overlap*/ 2);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCIOPoolThreads), 0);
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCMetaPoolThreads), 0);

    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();

    EXPECT_GT(backend->peakInFlight(), 1u) << "blob removes never overlapped";
    EXPECT_EQ(backend->removesOnPoolThreads(), kBlobs) << "every blob remove must run on a pool worker";
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCIOPoolThreads), 0);
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCMetaPoolThreads), 0);
}

TEST(CASGCRoundPools, ConcurrencyOneIsSequentialAndEquivalent)
{
    RunSummary at[2];
    for (int variant = 0; variant < 2; ++variant)
    {
        const uint64_t concurrency = variant == 0 ? 1 : 4;
        auto backend = std::make_shared<BlobRemoveWitness>();
        auto store = openPool(backend, concurrency);
        backend->watch(blobKeys(*store), 2);
        auto events = std::make_shared<SharedEventLog>();
        store->setEventSink([events](CasEvent e) { events->push(std::move(e)); });
        Gc gc(store, kGc);
        publishThenDrop(*backend, store, gc);
        at[variant] = reclaimAndSummarise(gc, store, *backend, events);
        if (concurrency == 1)
            EXPECT_EQ(backend->peakInFlight(), 1u);
        EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    }
    /// Same entries, same tokens, same outcomes, same order at concurrency 1 and 4 (see `RunSummary`);
    /// the tallies below are taken from the persisted audit rows when the outcome objects are written.
    EXPECT_EQ(at[0].records, at[1].records);
    EXPECT_EQ(at[0].deleted, at[1].deleted);
    EXPECT_EQ(at[0].absent, at[1].absent);
    EXPECT_EQ(at[0].replaced, at[1].replaced);
    EXPECT_EQ(at[0].redeleted, at[1].redeleted);
}

TEST(CASGCRoundPools, MetaStateSurvivesRounds)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    /// Round N condemns the six blobs and confirms their condemn markers in process.
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    std::vector<std::pair<BlobRef, PersistedEtag>> condemned;
    for (const auto & e : gc.previewDeletes())
        if (e.reason == "awaiting_graduation" || e.reason == "delete_pending")
            condemned.emplace_back(e.ref, e.token);
    ASSERT_FALSE(condemned.empty());
    GcMetaWriter::State & st = gc.metaStateForTest();
    for (const auto & [ref, token] : condemned)
        EXPECT_TRUE(st.condemnMarkerConfirmedInProcess(ref, token));
    EXPECT_EQ(st.scheduled.load(), st.completed.load());
    /// Round N+1 graduates; the writer of round N is gone, the confirmations must still be there.
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    for (const auto & [ref, token] : condemned)
        EXPECT_TRUE(st.condemnMarkerConfirmedInProcess(ref, token));
    EXPECT_EQ(st.scheduled.load(), st.completed.load());
}

/// Moved from gtest_cas_gc_meta_writer.cpp (`ThrowingRoundDrainsBeforeReturning`) with its ordering
/// intact: the round runs on another thread, a meta job is held on its latch, and the round must
/// still be running while the job is held (a 2 s `wait_for` that must time out), then throw once the
/// job is released, then show both pools gone.
TEST(CASGCRoundPools, RoundThrowingMidPhaseJoinsTheMetaPool)
{
    auto backend = std::make_shared<OutcomeLogFaultBackend>();
    auto store = openPool(backend, 4);
    const RootNamespace ns{"test/tbl"};
    const String ref_name = "all_0_0_0";
    const String payload = "round-drain-payload";
    PartWriteInfo info;
    info.intended_ref = ns.string() + "/" + ref_name;
    auto build = store->beginPartWrite(info);
    ManifestEntry entry;
    entry.path = "data.bin";
    entry.placement = EntryPlacement::Blob;
    entry.ref = idOf(payload);
    entry.blob_size = payload.size();
    const ManifestId manifest_id = build->stageManifest({entry});
    build->precommitAdd(ns, ref_name, manifest_id);
    build->putBlob(entry.ref, BlobSource::fromString(payload));
    build->promote(ns, ref_name, build->buildId(), manifest_id);
    store->dropRef(ns, ref_name);
    store->renewWatermarkOnce();
    Gc gc(store, kGc);
    runUntilDeletePending(gc, store);

    const uint64_t scheduled_before = gc.metaStateForTest().scheduled.load();
    backend->arm();
    backend->fail_outcome_logs.store(true);
    auto round = std::async(std::launch::async, [&]() -> std::pair<int, String>
    {
        try
        {
            gc.runRegularRound();
            return {0, ""};
        }
        catch (...)
        {
            return {DB::getCurrentExceptionCode(), DB::getCurrentExceptionMessage(/*with_stacktrace*/ false)};
        }
    });
    awaitLatchEntered(*backend);
    EXPECT_GT(gc.metaStateForTest().scheduled.load(), scheduled_before)
        << "the faulted round scheduled no meta job -- it cannot be the deleting round";
    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while a meta job was still in flight -- it did not drain on its throwing exit";
    backend->release();
    const auto [code, what] = round.get();
    EXPECT_EQ(code, ErrorCodes::ABORTED) << what;   /// the outcome-log conflict is classified ABORTED by the round
    EXPECT_NE(what.find("outcome log"), String::npos)
        << "expected the injected outcome-log failure, got: " << (what.empty() ? "a normal completion" : what);
    GcMetaWriter::State & st = gc.metaStateForTest();
    EXPECT_EQ(st.scheduled.load(), st.completed.load());
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCIOPoolThreads), 0);
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCMetaPoolThreads), 0);
}

/// The I/O twin: an entry's `remove` is held on a pool worker while the apply hook throws for another
/// entry, so the round's exceptional exit must join the held I/O job before it can return.
class HoldRemoveBackend : public InMemoryBackend
{
public:
    void hold(String key) { std::lock_guard l(mutex); held_key = std::move(key); holding = true; }
    void release() { { std::lock_guard l(mutex); holding = false; } gate.notify_all(); }
    bool entered() const { std::lock_guard l(mutex); return entered_flag; }
    RawRemoval remove(const String & key, const String & expected, TransportAccess & access) override
    {
        std::unique_lock lock(mutex);
        if (key == held_key)
        {
            entered_flag = true;
            gate.wait(lock, [&] { return !holding; });
        }
        lock.unlock();
        return InMemoryBackend::remove(key, expected, access);
    }
private:
    mutable std::mutex mutex;
    std::condition_variable gate;
    String held_key;
    bool holding = false;
    bool entered_flag = false;
};

TEST(CASGCRoundPools, RoundThrowingMidPhaseJoinsTheIOPool)
{
    auto backend = std::make_shared<HoldRemoveBackend>();
    const BlobRef held_ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(kBlobs))};
    auto thrown = std::make_shared<std::atomic<bool>>(false);
    PoolConfig cfg;
    cfg.gc_redelete_apply_hook_for_test = [held_ref, thrown](const BlobRef & ref)
    {
        if (!(ref == held_ref) && !thrown->exchange(true))
            throw std::runtime_error("injected apply fault");
    };
    auto store = openPool(backend, 4, cfg);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    backend->hold(blobKeyOf(*store, kBlobs));

    auto round = std::async(std::launch::async, [&]
    {
        try
        {
            gc.runRegularRound();
            return String{};
        }
        catch (const std::runtime_error & e)
        {
            return String(e.what());
        }
    });
    /// Declared AFTER `round`, so it is destroyed BEFORE the future: on any early exit the guard releases
    /// the held job first, and only then does the future's destructor wait for the round. A failed
    /// assertion therefore cannot hang the test.
    SCOPE_EXIT({ backend->release(); });
    /// Wait until the held remove is on a worker, then confirm the round cannot return while it is held.
    for (int i = 0; i < 500 && !backend->entered(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(backend->entered()) << "the held blob's remove never reached the backend";
    EXPECT_EQ(round.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the round returned while an I/O job was still running -- it did not join on its throwing exit";
    backend->release();
    EXPECT_EQ(round.get(), "injected apply fault");
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCIOPoolThreads), 0);
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCMetaPoolThreads), 0);
}

TEST(CASGCRoundPools, IOPoolFrameworkFailureFailsTheVerbBeforeCommit)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto fault = std::make_shared<std::atomic<bool>>(false);
    PoolConfig cfg;
    cfg.gc_io_pool_framework_fault_for_test = fault;
    auto store = openPool(backend, 4, cfg);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);

    /// The lease phase rewrites gc/state (owner, seq) on every round, so the etag is not the barrier's
    /// witness; the published pipeline position is. `decodeGcState` is the decoder `Gc` itself uses to
    /// read gc/state (`Formats/CasGcStateFormat.h`).
    const auto published = [&]
    {
        OperationForTest op(*backend);
        const auto raw = (*op).read(store->layout().gcStateKey(), Retry::standard());
        EXPECT_TRUE(raw.has_value());
        const GcState st = decodeGcState(raw->bytes);
        return std::make_tuple(st.round, st.snap_generation, st.snap_attempt);
    };
    const auto before = published();

    fault->store(true);
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative);
        FAIL() << "the escaped I/O job exception must fail the round";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::NETWORK_ERROR);
    }
    store->renewWatermarkOnce();
    EXPECT_EQ(published(), before) << "the round committed its pipeline position despite the framework failure";
    try
    {
        gc.rebuildBaseline(/*force*/ true);
        FAIL() << "the escaped I/O job exception must fail the rebuild";
    }
    catch (const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::NETWORK_ERROR);
    }
    EXPECT_EQ(published(), before) << "the rebuild published a baseline despite the framework failure";
    EXPECT_EQ(CurrentMetrics::get(CurrentMetrics::CASGCIOPoolThreads), 0);

    /// Recovery: the failed round already performed its physical deletes before the barrier (the blobs
    /// are gone), so one healthy round now records them absent and commits; the published round advances.
    fault->store(false);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_NE(std::get<0>(published()), std::get<0>(before)) << "a healthy round must advance the round";
    const RebuildReport rebuilt = gc.rebuildBaseline(/*force*/ true);
    EXPECT_TRUE(rebuilt.performed) << rebuilt.refusal;
    EXPECT_NE(std::get<1>(published()), std::get<1>(before)) << "a performed rebuild publishes a new generation";
}
```

`OperationForTest`, `idOf`, `awaitLatchEntered`, `OutcomeLogFaultBackend`, `SharedEventLog` come from `cas_test_helpers.h`; `GcState` and `decodeGcState` from `Formats/CasGcStateFormat.h` (include it).

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1; echo EXIT=$?; tail -5 build/build.log`
Expected: build fails on `metaStateForTest`, `gc_io_pool_framework_fault_for_test`.

- [ ] **Step 3: Implement**

Create `Gc/CasGcRound.h`:

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>
#include <Common/logger_useful.h>

namespace CurrentMetrics
{
    extern const Metric CASGCIOPoolThreads;
    extern const Metric CASGCIOPoolThreadsActive;
    extern const Metric CASGCIOPoolThreadsScheduled;
}

namespace DB::Cas
{

/// Everything a GC round needs that must not outlive it: the I/O pool the fold's read-ahead, the
/// orphan-manifest sweep and the `pending_deletes` fan-out submit to, and the round's meta writer.
/// A round verb constructs one at its top and destroys it on return or throw, so between rounds a
/// disk holds no GC pool worker (the backlog item this replaces measured 523 idle GC threads at 30
/// inline disks). Workers are borrowed from the global pool on first use and returned at round end.
///
/// The I/O pool is built with `shutdown_on_exception = false`: no I/O consumer lets an exception
/// escape a job (each settles a promise), but if one ever did, the default would shut the pool down
/// and discard queued jobs, and a discarded read-ahead job leaves its waiter blocked in `take`
/// forever. Under the flag queued jobs still run; the pool keeps the first escaped exception and
/// `waitIoPoolAndRethrow` surfaces it at the round's pre-commit barrier.
class GcRound
{
public:
    GcRound(GcMetaWriter::StatePtr meta_state, size_t concurrency_)
        : concurrency_value(std::max<size_t>(1, concurrency_))
        , io_pool(CurrentMetrics::CASGCIOPoolThreads, CurrentMetrics::CASGCIOPoolThreadsActive,
                  CurrentMetrics::CASGCIOPoolThreadsScheduled,
                  /*max_threads*/ concurrency_value, /*max_free_threads*/ concurrency_value,
                  /*queue_size*/ 0, /*shutdown_on_exception*/ false)
        , meta_writer(std::move(meta_state), concurrency_value)
    {
    }

    GcRound(const GcRound &) = delete;
    GcRound & operator=(const GcRound &) = delete;

    /// The body runs before the members are destroyed: drain the meta pool (formerly the round-exit
    /// `SCOPE_EXIT`), then wait for the I/O pool inside a guard, because a `ThreadPool` destructor only
    /// finalizes and neither rethrows nor logs a pending failure. Then members die in reverse order:
    /// the meta writer (its destructor drains again and finds nothing), then the I/O pool. Neither
    /// pool's jobs wait on the other pool, so this order cannot deadlock.
    ~GcRound()
    {
        meta_writer.drainOnExitNoThrow();
        try
        {
            io_pool.wait();
        }
        catch (...)
        {
            tryLogCurrentException(meta_writer.logger(), "CAS gc: I/O pool wait failed during round-exit cleanup");
        }
    }

    ThreadPool & ioPool() { return io_pool; }
    GcMetaWriter & metaWriter() { return meta_writer; }
    size_t concurrency() const { return concurrency_value; }

    /// The I/O half of the pre-commit barrier: blocks until no I/O job is queued or running and
    /// rethrows a framework failure the pool still holds, failing the verb before its commit.
    void waitIoPoolAndRethrow() { io_pool.wait(); }

private:
    const size_t concurrency_value;
    ThreadPool io_pool;
    GcMetaWriter meta_writer;
};

}
```

`Pool/CasPool.h`, `PoolConfig`, after `teardown_phase1_throw_for_test`:

```cpp
    /// Test seams for the GC (inert in production). The apply hook fires at the START of
    /// `Gc::applyRedeleteOutcome`, before any effect of the entry, with the entry's ref. The framework
    /// fault flag, while set, makes each round verb submit one I/O job that lets a `NETWORK_ERROR`
    /// exception escape its body, to prove the pre-commit barrier surfaces it; it is a shared flag so a
    /// test can arm it after its fixture rounds. `gc_io_pool_refuse_at_for_test = k` makes the k-th enqueue of a
    /// `redeleteBlobs` call (1-based) fail with `CANNOT_SCHEDULE_TASK`; 0 disables it.
    std::function<void(const BlobRef &)> gc_redelete_apply_hook_for_test = {};
    std::shared_ptr<std::atomic<bool>> gc_io_pool_framework_fault_for_test = {};
    size_t gc_io_pool_refuse_at_for_test = 0;
```

`CasGc.h`: replace the three pool members with `GcMetaWriter::StatePtr meta_state;`; replace `metaWriterForTest` with `GcMetaWriter::State & metaStateForTest() { return *meta_state; }`; add `GcRound & round` as the last parameter of `fold`; add the transitional `GcMetaWriter & meta_writer` parameter and `ThreadPool & pool` parameter to `redeleteBlobs` (Task 4 replaces the whole signature); `#include "CasGcRound.h"`. Add `extern const int NETWORK_ERROR;` to the `ErrorCodes` block in `CasGc.cpp` if it is not already there.

`CasGc.cpp` constructor: replace the meta writer and both pool constructions with `meta_state = GcMetaWriter::makeState(store, logger);`.

`runRegularRound`: immediately after `std::optional<Etag> state_etag;` (before the lease phase and before the `meta_jobs_*_at_round_start` reads) insert

```cpp
    /// The round's pools live exactly as long as this frame (see `GcRound`). Built before the lease
    /// phase so every later line of the verb can use them; a round that returns from the lease phase
    /// destroys them having started no worker.
    GcRound round(meta_state, static_cast<size_t>(store->poolConfig().gc_concurrency));
    GcMetaWriter & meta_writer = round.metaWriter();
    if (const auto & fault = store->poolConfig().gc_io_pool_framework_fault_for_test; fault && fault->load())
        round.ioPool().scheduleOrThrowOnError([] { throw Exception(ErrorCodes::NETWORK_ERROR, "injected escaped I/O job exception"); });
```

Delete `SCOPE_EXIT({ meta_writer->drainOnExitNoThrow(); });`. Change every `meta_writer->` in this function to `meta_writer.`. Pass `round` to `fold`. Replace `redeleteBlobs(redelete_now, layout, op, new_round, generation, round_work_budget, report, outcomes[shard]);` with `redeleteBlobs(redelete_now, layout, round.ioPool(), op, new_round, generation, round_work_budget, report, outcomes[shard], meta_writer);`. Inside the PR's `redeleteBlobs` replace `redelete_pool->` with `pool.`, delete the `if (!redelete_pool || ...)` sequential branch (with one worker the pool path is sequential), and delete the PR's `redeleteBlob` wrapper (declaration and definition; it was that branch's only caller). `applyRedeleteOutcome` takes `GcMetaWriter & meta_writer` as a trailing parameter instead of the member; its two `meta_writer->` uses become `meta_writer.`; and its first statement becomes `if (const auto & hook = store->poolConfig().gc_redelete_apply_hook_for_test) hook(entry.ref);` so the I/O latch test in this task can throw from the apply step (Task 4 keeps that line). After the `meta_pool_wait` block insert

```cpp
    /// The barrier's I/O half: a framework failure a pool worker left behind (no consumer lets one
    /// escape a job; this is the guard) fails the round HERE, before the gc/state CAS, rather than being
    /// logged by the round's destructor after a commit.
    round.waitIoPoolAndRethrow();
```

`fold`: add `GcRound & round`; `GcReadAhead reads(op, store->openRequests(), round.ioPool(), round.concurrency());`; its four `meta_writer->` uses become `round.metaWriter().` (lambdas capturing `this` for them must also capture `&round`). Sweep call: `&work_budget, &round.ioPool(), round.concurrency());`.

`rebuildBaseline`: it already has a local `const uint64_t round` further down, so name this one `gc_round`. Right after its `CasOperation op = store->openRequests().admit();` insert

```cpp
    GcRound gc_round(meta_state, static_cast<size_t>(store->poolConfig().gc_concurrency));
    if (const auto & fault = store->poolConfig().gc_io_pool_framework_fault_for_test; fault && fault->load())
        gc_round.ioPool().scheduleOrThrowOnError([] { throw Exception(ErrorCodes::NETWORK_ERROR, "injected escaped I/O job exception"); });
```

and `GcReadAhead reads(op, store->openRequests(), gc_round.ioPool(), gc_round.concurrency());`. Immediately before the line `WriteResult commit = op.replace(layout.gcStateKey(), encodeGcState(next), *state_etag,` (this is the durable publish; the `orThrow(std::move(commit), ...)` below it only classifies the result) insert

```cpp
    /// Pre-commit barrier, I/O half (there is no meta drain on this verb: a rebuild condemns nothing).
    /// Placed before the gc/state write, not before `orThrow`, so a framework failure can never follow
    /// a published baseline.
    gc_round.waitIoPoolAndRethrow();
```

- [ ] **Step 4: Run**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CASGCRoundPools.*:CASGcMetaWriter.*:CASGCReadAhead.*:CASGCRedeleteConcurrency.*:CASRefGc.*' > build/test_task3.log 2>&1; echo EXIT=$?; grep -E '^\[  (PASSED|FAILED)  \]' build/test_task3.log`
Expected: EXIT=0, PASSED with a non-zero count.

- [ ] **Step 5: Commit**

```bash
git commit -m "cas: GcRound owns the GC pools for one round; Gc keeps only the meta State; barrier waits both pools in both verbs" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcRound.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Disks/tests/gtest_cas_gc_round_pools.cpp src/Disks/tests/gtest_cas_gc_meta_writer.cpp
```

---

### Task 4: The `pending_deletes` fan-out on the round's I/O pool (spec 3.3, 3.4, 3.5, tests 5 to 10, 9a, 9b) {#task-4-the-pending-deletes-fan-out-on-the-round-s-i-o-pool-spec-3-3-3-4-3-5-tests-5-to-10-9a-9b}

**Files:**
- Modify: `Gc/CasGc.h`: `struct RedeleteIo`, the declarations of `performRedeleteIo`, `applyRedeleteOutcome`, `redeleteBlob`, `redeleteBlobs`; `struct RoundReport` (grep `size_t redeleted = 0;`); add the test seams listed below
- Modify: `Gc/CasGc.cpp`: the four redelete functions; the call site (grep `redeleteBlobs(redelete_now`); the `pending_deletes` timer (grep `pending_deletes_timer.emplace(phase_sink, "pending_deletes")` and `pending_deletes_timer->metric("redeleted"`); the `ProfileEvents` and `ErrorCodes` extern blocks near the top
- Modify: `Gc/CasGcMetaWriter.h/.cpp` (one seam, `injectNextConfirmedMetaDeleteFailureForTest`)
- Modify: `src/Common/ProfileEvents.cpp` (after `M(CASGCRetiredRedeleted,`), `src/Common/setThreadName.h` (after `M(CAS_GC_SCHEDULER, "CasGcSched")`)
- Test: `src/Disks/tests/gtest_cas_gc_redelete_concurrency.cpp` (rewrite)

**Interfaces:**
- Consumes: `GcRound` and the three `PoolConfig` seams (Task 3).
- Produces:
  ```cpp
  struct RedeleteRoundContext
  {
      uint64_t new_round;
      uint64_t round_generation;     /// feeds the event row; NOT what workers resume under
      GcRoundWorkBudget & budget;
      RoundReport & report;
      OutcomeLog & outcome_log;
      GcMetaWriter & meta_writer;
  };
  struct RedeleteIo { String blob_key; Removal del = Removal::Gone; };   // moved to namespace DB::Cas scope in CasGc.h, above class Gc (it was a private nested type)
  // free function in CasGc.cpp's anonymous namespace (no `this`):
  RedeleteIo performRedeleteIo(const RetiredEntry & entry, const Layout & layout, CasOperation & op);
  void Gc::applyRedeleteOutcome(const RetiredEntry & entry, const RedeleteIo & io, RedeleteRoundContext & ctx);
  void Gc::redeleteBlobs(const std::vector<RetiredEntry> & entries, ThreadPool & pool, size_t concurrency,
                         CasOperation & op, RedeleteRoundContext & ctx);
  ```
  `RoundReport` gains `size_t redelete_failed = 0; uint64_t gc_concurrency = 0; uint64_t redelete_jobs_scheduled = 0; uint64_t redelete_pool_wait_us = 0;`. `PoolConfig::gc_redelete_apply_hook_for_test` becomes `std::function<void(const BlobRef &)>` (the entry's ref lets tests target one entry). Seams: `size_t Gc::redeleteWindowPeakForTest() const` (largest number of outstanding futures observed by any `redeleteBlobs` call on this `Gc`); `LoggerPtr Gc::loggerForTest() const` (the `Gc` logger, for log capture); `GcMetaWriter::armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::exception_ptr)` (the next `scheduleConfirmedMetaDelete` reaches `submit` with `scheduleOrThrowOnError` throwing that exception, so the failure travels the real `submit` propagation path) and `Gc::armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::exception_ptr)` which forwards it to the next round's writer.

- [ ] **Step 1: Write the failing tests**

Rewrite `src/Disks/tests/gtest_cas_gc_redelete_concurrency.cpp` in full:

```cpp
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <Poco/StreamChannel.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

namespace ProfileEvents
{
    extern const Event CASGCRetiredRedeleteFailed;
}
namespace CurrentMetrics
{
    extern const Metric CASGCIOPoolThreadsScheduled;
}
namespace DB::ErrorCodes
{
    extern const int CANNOT_SCHEDULE_TASK;
    extern const int NETWORK_ERROR;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;
using DB::Exception;
namespace ErrorCodes = DB::ErrorCodes;

namespace
{

const DB::UInt128 kGc = hexToU128("00000000000000000000000000000001");
constexpr uint64_t kBlobs = 6;
constexpr uint64_t kFaultedBlob = 3;

BlobRef refOf(uint64_t blob) { return BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(DB::UInt128(blob))}; }
String blobKeyOf(const Pool & store, uint64_t blob) { return store.layout().blobKey(refOf(blob)); }

/// Faults fire once per armed key, only from a thread other than the arming one (a pool worker), and
/// name the key so a test can tell which failure was rethrown.
class FaultBackend : public InMemoryBackend
{
public:
    void armRemoveAgainstOtherThreads(std::set<String> keys) { arm(remove_keys, std::move(keys)); }
    void armHeadAgainstOtherThreads(std::set<String> keys) { arm(head_keys, std::move(keys)); }
    size_t removeFaultsFired() const { std::lock_guard l(mutex); return fired_remove; }
    size_t headFaultsFired() const { std::lock_guard l(mutex); return fired_head; }
    /// Samples the I/O pool's queued+running jobs on every watched remove; test 10 reads the maximum.
    long peakScheduledSeen() const { std::lock_guard l(mutex); return peak_scheduled; }

    RawRemoval remove(const String & key, const String & expected, TransportAccess & access) override
    {
        {
            std::lock_guard l(mutex);
            peak_scheduled = std::max(peak_scheduled, CurrentMetrics::get(CurrentMetrics::CASGCIOPoolThreadsScheduled));
            if (remove_keys.erase(key) && std::this_thread::get_id() != owner)
            {
                ++fired_remove;
                throw std::runtime_error("injected remove fault on " + key);
            }
        }
        return InMemoryBackend::remove(key, expected, access);
    }
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        {
            std::lock_guard l(mutex);
            if (head_keys.erase(key) && std::this_thread::get_id() != owner)
            {
                ++fired_head;
                throw std::runtime_error("injected head fault on " + key);
            }
        }
        return InMemoryBackend::head(key, access);
    }

private:
    void arm(std::set<String> & target, std::set<String> keys)
    {
        std::lock_guard l(mutex);
        target = std::move(keys);
        owner = std::this_thread::get_id();
    }
    mutable std::mutex mutex;
    std::set<String> remove_keys, head_keys;
    size_t fired_remove = 0, fired_head = 0;
    long peak_scheduled = 0;
    std::thread::id owner;
};

PoolPtr openPool(std::shared_ptr<Backend> backend, uint64_t concurrency, PoolConfig extra = {})
{
    PoolConfig config = std::move(extra);
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.gc_concurrency = concurrency;
    return Pool::open(std::move(backend), std::move(config));
}

void publishThenDrop(Backend & backend, const PoolPtr & store, Gc & gc, uint64_t blobs = kBlobs)
{
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 0xAA};
    std::vector<ManifestEntry> entries;
    for (uint64_t b = 1; b <= blobs; ++b)
    {
        writeBlobBody(backend, store->layout(), DB::UInt128(b));
        entries.push_back(blobEntryFor("f" + std::to_string(b), DB::UInt128(b)));
    }
    writeManifestRaw(backend, store->layout(), ns, r, entries);
    publishCommittedTransition(backend, store->layout(), ns, "tbl", std::nullopt, r);
    runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    dropRefTransition(backend, store->layout(), ns, "tbl", r);
}

void runUntilDeletePending(Gc & gc, const PoolPtr & store)
{
    for (int i = 0; i < 16; ++i)
    {
        for (const auto & e : gc.previewDeletes())
            if (e.reason == "delete_pending")
                return;
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
    }
    FAIL() << "no round reached a pending delete; the fixture is wrong";
}

bool allBlobsAbsent(Backend & backend, const Pool & store, uint64_t blobs = kBlobs)
{
    for (uint64_t b = 1; b <= blobs; ++b)
        if (!blobAbsent(backend, store.layout(), DB::UInt128(b)))
            return false;
    return true;
}

size_t blobDeleteEvents(const std::vector<CasEvent> & events, const char * outcome)
{
    size_t n = 0;
    for (const CasEvent & e : events)
        if (e.type == CasEventType::BlobDelete && e.outcome == outcome)
            ++n;
    return n;
}

/// Captures the `Gc` logger's output (the `ScopedCasSettingsLogCapture` idiom from gtest_cas_settings.cpp).
class ScopedGcLogCapture
{
public:
    explicit ScopedGcLogCapture(LoggerPtr logger_)
        : logger(std::move(logger_))
        , channel(new Poco::StreamChannel(stream))
        , old_channel(logger->getChannel(), /*shared=*/true)
        , old_level(logger->getLevel())
    {
        logger->setChannel(channel.get());
        logger->setLevel("warning");
    }
    ~ScopedGcLogCapture()
    {
        logger->setChannel(old_channel);
        logger->setLevel(old_level);
    }
    size_t count(const String & needle) const
    {
        const String text = stream.str();
        size_t n = 0;
        for (size_t pos = text.find(needle); pos != String::npos; pos = text.find(needle, pos + needle.size()))
            ++n;
        return n;
    }
private:
    LoggerPtr logger;
    std::ostringstream stream;
    Poco::AutoPtr<Poco::StreamChannel> channel;
    Poco::AutoPtr<Poco::Channel> old_channel;
    int old_level;
};

/// Number of keys under the pool prefix whose path contains `/outcomes/` (the `GcOutcomes` family).
size_t outcomeObjects(Backend & backend, const Pool & store)
{
    OperationForTest op(backend);
    size_t n = 0;
    String cursor;
    while (true)
    {
        const auto page = (*op).list(store.layout().poolPrefix(), cursor, /*limit*/ 1000, Retry::standard());
        for (const auto & listed : page.keys)   /// `ListPage::keys` holds `ListedKey` records
            if (listed.key.contains("/outcomes/"))
                ++n;
        if (page.next_cursor.empty())
            return n;
        cursor = page.next_cursor;
    }
}

/// The order `pending_deletes` walks a shard's batch: the merge consumes rows by ascending key, so
/// entries come in ascending blob-key order. Tests that depend on "the lower-index failure" check this
/// precondition through `previewDeletes`, which lists entries in the same order.
std::vector<uint64_t> deletePendingOrder(Gc & gc, const Pool & store)
{
    std::vector<uint64_t> order;
    for (const auto & e : gc.previewDeletes())
        if (e.reason == "delete_pending")
            for (uint64_t b = 1; b <= 64; ++b)
                if (e.ref == refOf(b))
                    order.push_back(b);
    return order;
}

/// Runs one redelete round expecting it to throw; returns the report the round filled before throwing.
template <typename ExceptionT>
RoundReport runOneThrowingRound(Gc & gc, const PoolPtr & store, std::function<void(const ExceptionT &)> check)
{
    RoundReport progress;
    bool threw = false;
    try
    {
        gc.runRegularRound({}, /*allow_steal*/ true, UniversePolicy::Authoritative, &progress);
    }
    catch (const ExceptionT & e)
    {
        threw = true;
        check(e);
    }
    store->renewWatermarkOnce();
    EXPECT_TRUE(threw) << "the round was expected to throw";
    return progress;
}

}

TEST(CASGCRedeleteConcurrency, WorkerRemoveFaultIsLoggedCountedAndRetried)
{
    auto backend = std::make_shared<FaultBackend>();
    auto store = openPool(backend, 4);
    auto events = std::make_shared<SharedEventLog>();
    store->setEventSink([events](CasEvent e) { events->push(std::move(e)); });
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    backend->armRemoveAgainstOtherThreads({blobKeyOf(*store, kFaultedBlob)});
    const auto failed_before = ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load();

    const size_t outcome_objects_before = outcomeObjects(*backend, *store);
    RoundReport progress;
    {
        ScopedGcLogCapture log(gc.loggerForTest());
        progress = runOneThrowingRound<std::runtime_error>(gc, store, [&](const std::runtime_error & e)
        {
            EXPECT_NE(String(e.what()).find(blobKeyOf(*store, kFaultedBlob)), String::npos);
        });
        EXPECT_EQ(log.count("pending delete of blob"), 1u) << "exactly one failure line for one failed slot";
        /// The key also appears inside the injected exception text, so count the diagnostic's own field.
        EXPECT_EQ(log.count("(key " + blobKeyOf(*store, kFaultedBlob) + ", condemned"), 1u);
    }
    EXPECT_EQ(backend->removeFaultsFired(), 1u) << "the fault must have fired on a pool worker";
    EXPECT_EQ(progress.redeleted, kBlobs - 1);
    EXPECT_EQ(progress.redelete_failed, 1u);
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load() - failed_before, 1u);
    EXPECT_EQ(blobDeleteEvents(events->snapshot(), "deleted"), kBlobs - 1);
    /// A throwing round never reaches its `GcOutcomes` write: no new outcome object appears, and the
    /// tallies that are taken at that write (`deleted`/`absent`/`replaced`) stay 0 while `redeleted` counted 5.
    EXPECT_EQ(outcomeObjects(*backend, *store), outcome_objects_before) << "a throwing round persists no GcOutcomes";
    EXPECT_EQ(progress.deleted, 0u);
    EXPECT_EQ(progress.absent, 0u);
    for (uint64_t b = 1; b <= kBlobs; ++b)
        EXPECT_EQ(blobAbsent(*backend, store->layout(), DB::UInt128(b)), b != kFaultedBlob) << "blob " << b;

    /// Next round: the faulted blob is deleted for real; its five siblings replay as absent.
    const RoundReport next = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(next.deleted, 1u);
    EXPECT_EQ(next.absent, kBlobs - 1);
    EXPECT_EQ(blobDeleteEvents(events->snapshot(), "deleted"), kBlobs);
    EXPECT_EQ(blobDeleteEvents(events->snapshot(), "absent"), kBlobs - 1);
}

TEST(CASGCRedeleteConcurrency, WorkerHeadFaultIsLoggedCountedAndRetried)
{
    auto backend = std::make_shared<FaultBackend>();
    auto store = openPool(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    /// Arm only once the pipeline reached delete_pending: the fold's condemn-time HEADs also run on
    /// pool workers, and a fault there would fire in the wrong phase.
    runUntilDeletePending(gc, store);
    backend->armHeadAgainstOtherThreads({blobKeyOf(*store, kFaultedBlob)});

    RoundReport progress;
    {
        ScopedGcLogCapture log(gc.loggerForTest());
        progress = runOneThrowingRound<std::runtime_error>(gc, store, [](const std::runtime_error &) {});
        EXPECT_EQ(log.count("pending delete of blob"), 1u);
    }
    EXPECT_EQ(backend->headFaultsFired(), 1u);
    EXPECT_EQ(progress.redeleted, kBlobs - 1);
    EXPECT_EQ(progress.redelete_failed, 1u);
    EXPECT_FALSE(blobAbsent(*backend, store->layout(), DB::UInt128(kFaultedBlob))) << "a HEAD fault mutates nothing";

    const RoundReport next = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_TRUE(allBlobsAbsent(*backend, *store));
    EXPECT_EQ(next.deleted, 1u);
    EXPECT_EQ(next.absent, kBlobs - 1);
}

TEST(CASGCRedeleteConcurrency, TwoFailuresProduceTwoLogLinesAndTheLowerIndexException)
{
    auto backend = std::make_shared<FaultBackend>();
    auto store = openPool(backend, 4);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    const std::vector<uint64_t> order = deletePendingOrder(gc, *store);
    ASSERT_EQ(order.size(), kBlobs);
    const uint64_t first = order[1], second = order[4];   /// two entries with a known relative order
    backend->armRemoveAgainstOtherThreads({blobKeyOf(*store, first), blobKeyOf(*store, second)});
    RoundReport progress;
    {
        ScopedGcLogCapture log(gc.loggerForTest());
        progress = runOneThrowingRound<std::runtime_error>(gc, store, [&](const std::runtime_error & e)
        {
            EXPECT_NE(String(e.what()).find(blobKeyOf(*store, first)), String::npos) << e.what();
        });
        EXPECT_EQ(log.count("pending delete of blob"), 2u);
    }
    EXPECT_EQ(backend->removeFaultsFired(), 2u);
    EXPECT_EQ(progress.redelete_failed, 2u);
    EXPECT_EQ(progress.redeleted, kBlobs - 2);
}

TEST(CASGCRedeleteConcurrency, WorkerFailureBeforeARefusalIsTheOneRethrown)
{
    /// Entry 0's request fails on a worker and the 4th enqueue is refused; the rethrown exception must
    /// be the worker failure (lower index), not the refusal that was recorded first in time.
    auto backend = std::make_shared<FaultBackend>();
    PoolConfig cfg;
    cfg.gc_io_pool_refuse_at_for_test = 4;
    auto store = openPool(backend, 4, cfg);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    const std::vector<uint64_t> order = deletePendingOrder(gc, *store);
    ASSERT_EQ(order.size(), kBlobs);
    backend->armRemoveAgainstOtherThreads({blobKeyOf(*store, order[0])});
    RoundReport progress;
    {
        ScopedGcLogCapture log(gc.loggerForTest());
        progress = runOneThrowingRound<std::runtime_error>(gc, store, [&](const std::runtime_error & e)
        {
            EXPECT_NE(String(e.what()).find(blobKeyOf(*store, order[0])), String::npos) << e.what();
        });
        EXPECT_EQ(log.count("pending delete of blob"), 2u) << "the worker failure and the refusal are both logged";
    }
    EXPECT_EQ(progress.redelete_failed, 2u) << "the worker failure and the refusal are both counted";
    EXPECT_EQ(progress.redeleted, 2u) << "entries 1 and 2 applied; 0 failed; 3 refused; 4 and 5 never submitted";
}

TEST(CASGCRedeleteConcurrency, ReplacedEntryInsideAParallelBatch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend, 4);
    auto events = std::make_shared<SharedEventLog>();
    store->setEventSink([events](CasEvent e) { events->push(std::move(e)); });
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    /// A writer republishes blob 2 under a new incarnation: a conditional replace against the current
    /// etag, so the condemned incarnation's exact-etag delete no longer matches.
    {
        OperationForTest op(*backend);
        const String key = blobKeyOf(*store, 2);
        const auto meta = (*op).head(key, Retry::standard());
        ASSERT_TRUE(meta.has_value());
        EnvelopeHeader header;
        header.kind = ObjectKind::Blob;
        header.incarnation_tag = DB::UInt128(0x4321);
        header.build_id = DB::UInt128(0x8765);
        const String body = encodeEnvelopeHeader(header, 256) + String("y");
        orThrow((*op).replace(key, body, meta->etag, Retry::standard()), "test: replace blob 2");
    }
    const RoundReport rep = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_EQ(rep.replaced, 1u);
    EXPECT_EQ(rep.deleted, kBlobs - 1);
    EXPECT_FALSE(blobAbsent(*backend, store->layout(), DB::UInt128(2))) << "the fresh incarnation is a live object";
    EXPECT_EQ(blobDeleteEvents(events->snapshot(), "replaced"), 1u);
    EXPECT_EQ(blobDeleteEvents(events->snapshot(), "deleted"), kBlobs - 1);
}

TEST(CASGCRedeleteConcurrency, ApplyPhaseFaultLeavesSiblingsForNextRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    /// kBlobs = 6 < 2 × concurrency = 8: every entry is submitted before the hook fires, and no
    /// request fault is armed, so every unapplied sibling was physically removed.
    auto applies = std::make_shared<std::atomic<int>>(0);
    PoolConfig cfg;
    cfg.gc_redelete_apply_hook_for_test = [applies](const BlobRef &)
    {
        if (applies->fetch_add(1) == 2)
            throw std::runtime_error("injected apply fault");
    };
    auto store = openPool(backend, 4, cfg);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);

    const RoundReport progress = runOneThrowingRound<std::runtime_error>(gc, store, [](const std::runtime_error & e)
    {
        EXPECT_STREQ(e.what(), "injected apply fault");
    });
    EXPECT_EQ(progress.redeleted, 2u) << "entries 0 and 1 applied; entry 2 hit the hook before any effect";
    EXPECT_EQ(progress.redelete_failed, 0u);
    EXPECT_TRUE(allBlobsAbsent(*backend, *store)) << "every job's DELETE had run";

    /// The throwing round committed nothing, so ALL six entries are still delete_pending and replay
    /// against an absent blob, including the two that were applied in memory.
    applies->store(-1000);
    const RoundReport next = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_EQ(next.absent, kBlobs);
    EXPECT_EQ(next.deleted, 0u);
}

TEST(CASGCRedeleteConcurrency, ApplyPhaseFaultWithLaterWorkerFailureLogsBoth)
{
    auto backend = std::make_shared<FaultBackend>();
    const BlobRef faulted = refOf(kFaultedBlob);
    /// The hook throws at the first entry that is NOT the faulted one; the faulted one's own request
    /// fails on a worker. Whichever comes first in batch order, cleanup must still inspect and count
    /// the worker failure while the apply exception wins.
    auto thrown = std::make_shared<std::atomic<bool>>(false);
    PoolConfig cfg;
    cfg.gc_redelete_apply_hook_for_test = [faulted, thrown](const BlobRef & ref)
    {
        if (!(ref == faulted) && !thrown->exchange(true))
            throw std::runtime_error("injected apply fault");
    };
    auto store = openPool(backend, 4, cfg);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    backend->armRemoveAgainstOtherThreads({blobKeyOf(*store, kFaultedBlob)});
    const auto failed_before = ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load();

    RoundReport progress;
    {
        ScopedGcLogCapture log(gc.loggerForTest());
        progress = runOneThrowingRound<std::runtime_error>(gc, store, [](const std::runtime_error & e)
        {
            EXPECT_STREQ(e.what(), "injected apply fault");
        });
        EXPECT_EQ(log.count("pending delete of blob"), 1u) << "the worker failure must be logged even though apply threw";
    }
    EXPECT_EQ(backend->removeFaultsFired(), 1u);
    EXPECT_EQ(progress.redelete_failed, 1u) << "a cleanup that only waits, without inspecting, loses this failure";
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CASGCRetiredRedeleteFailed].load() - failed_before, 1u);
}

TEST(CASGCRedeleteConcurrency, MetaSubmitFailureInsideApplyLeavesAPartialEntry)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto last_applied = std::make_shared<std::mutex>();
    auto last_ref = std::make_shared<std::optional<BlobRef>>();
    PoolConfig cfg;
    cfg.gc_redelete_apply_hook_for_test = [last_applied, last_ref](const BlobRef & ref)
    {
        std::lock_guard l(*last_applied);
        *last_ref = ref;
    };
    auto store = openPool(backend, 4, cfg);
    Gc gc(store, kGc);
    publishThenDrop(*backend, store, gc);
    runUntilDeletePending(gc, store);
    auto events = std::make_shared<SharedEventLog>();
    store->setEventSink([events](CasEvent e) { events->push(std::move(e)); });
    /// The next `scheduleConfirmedMetaDelete` reaches `submit` with `scheduleOrThrowOnError` throwing
    /// a pending-worker-style NETWORK_ERROR, which `submit` must propagate (the real path of 3.1).
    gc.armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::make_exception_ptr(
        Exception(ErrorCodes::NETWORK_ERROR, "injected pending worker exception")));

    const RoundReport progress = runOneThrowingRound<Exception>(gc, store, [](const Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::NETWORK_ERROR);
    });
    BlobRef partial;
    {
        std::lock_guard l(*last_applied);
        ASSERT_TRUE(last_ref->has_value());
        partial = **last_ref;
    }
    EXPECT_GE(progress.redeleted, 1u) << "the partial entry was counted before the meta call";
    EXPECT_GE(blobDeleteEvents(events->snapshot(), "deleted"), 1u) << "its event was emitted before the meta call";
    PersistedEtag token;
    for (const auto & e : gc.previewDeletes())
        if (e.ref == partial)
            token = e.token;
    GcMetaWriter::State & st = gc.metaStateForTest();
    EXPECT_TRUE(st.condemnMarkerConfirmedInProcess(partial, token)) << "the marker was not yet forgotten";

    const RoundReport next = runRegularRoundReclaiming(gc);
    store->renewWatermarkOnce();
    EXPECT_EQ(next.absent, kBlobs);
    EXPECT_FALSE(st.condemnMarkerConfirmedInProcess(partial, token)) << "the next round forgets it";
}

TEST(CASGCRedeleteConcurrency, SubmissionWindowBoundsOutstandingWork)
{
    constexpr uint64_t many = 40;   /// > 4 × concurrency
    {
        auto backend = std::make_shared<FaultBackend>();
        auto store = openPool(backend, 4);
        Gc gc(store, kGc);
        publishThenDrop(*backend, store, gc, many);
        runUntilDeletePending(gc, store);
        backend->armRemoveAgainstOtherThreads({});   /// no fault, just the scheduled-metric sampling
        runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        EXPECT_TRUE(allBlobsAbsent(*backend, *store, many));
        EXPECT_LE(gc.redeleteWindowPeakForTest(), 8u) << "more than 2 × concurrency futures outstanding";
        /// The pool's scheduled metric is released when the job object is destroyed, after the promise
        /// was fulfilled; a replenishment can therefore observe up to `concurrency` jobs still counted
        /// past the window. The bound the caller controls is the deque above; this one allows that lag.
        EXPECT_LE(backend->peakScheduledSeen(), 8 + 4) << "more than 2 × concurrency + concurrency jobs queued or running";
    }
    {
        auto backend = std::make_shared<FaultBackend>();
        PoolConfig cfg;
        cfg.gc_io_pool_refuse_at_for_test = 5;   /// the 5th enqueue (index 4) is refused
        auto store = openPool(backend, 4, cfg);
        Gc gc(store, kGc);
        publishThenDrop(*backend, store, gc, many);
        runUntilDeletePending(gc, store);
        RoundReport progress;
        {
            ScopedGcLogCapture log(gc.loggerForTest());
            progress = runOneThrowingRound<Exception>(gc, store, [](const Exception & e)
            {
                EXPECT_EQ(e.code(), ErrorCodes::CANNOT_SCHEDULE_TASK);
            });
            EXPECT_EQ(log.count("pending delete of blob"), 1u) << "the refusal is logged once";
        }
        EXPECT_EQ(progress.redeleted, 4u) << "entries before the refused index are applied";
        EXPECT_EQ(progress.redelete_failed, 1u) << "the refusal is counted once";
        size_t present = 0;
        for (uint64_t b = 1; b <= many; ++b)
            present += blobAbsent(*backend, store->layout(), DB::UInt128(b)) ? 0 : 1;
        EXPECT_EQ(present, many - 4) << "entries from the refused index on were never attempted";
    }
}
```

`orThrow` is the CAS helper used throughout `CasGc.cpp`; `encodeEnvelopeHeader` and `EnvelopeHeader` are what `writeBlobBody` in `cas_test_helpers.h` uses. `BlobRef` compares with `==` (it is used as a set key in the writer's `State`).

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1; echo EXIT=$?; tail -5 build/build.log`
Expected: build fails on `redelete_failed`, `CASGCRetiredRedeleteFailed`, the seams, and the hook's new signature.

- [ ] **Step 3: Implement**

`ProfileEvents.cpp`, after `M(CASGCRetiredRedeleted, ...)`:

```cpp
    M(CASGCRetiredRedeleteFailed, "Number of CAS GC pending deletes whose HEAD or conditional DELETE request failed, or that could not be submitted to the round's I/O pool. Each stays delete_pending and is retried next round. Growth without matching CASGCRetiredRedeleted growth means the object store is refusing the GC's deletes.", ValueType::Number) \
```

`setThreadName.h`, after `M(CAS_GC_SCHEDULER, "CasGcSched") \`: `M(CAS_GC_REDELETE, "CasGcRedelete") \`.

`CasGc.cpp` extern blocks: add `extern const Event CASGCRetiredRedeleteFailed;` to `ProfileEvents`, and `extern const int CANNOT_SCHEDULE_TASK;` (and `NETWORK_ERROR` if Task 3 did not) to `ErrorCodes`. Includes: `<Common/threadPoolCallbackRunner.h>`, `<Common/setThreadName.h>`, `<deque>`.

`CasGc.h`, `RoundReport`: replace the `redeleted` line and its comment with

```cpp
    /// Pending deletes APPLIED this round (exact-token blob deletes whose outcome was recorded). Counted
    /// per entry as it is applied, so a round that throws in `pending_deletes` reports the entries
    /// applied before the throw. Note the apply-phase window: an apply-side exception can leave entries
    /// whose I/O completed (the blob is gone) unapplied and uncounted, and can leave the failing entry
    /// counted with its condemn marker not yet forgotten; the throwing round commits nothing, and the
    /// next round re-observes those blobs absent and settles them then.
    size_t redeleted = 0;
    size_t redelete_failed = 0;          /// pending deletes whose request failed or was not submitted; retried next round
    uint64_t gc_concurrency = 0;         /// the effective per-round pool size, for the round log
    uint64_t redelete_jobs_scheduled = 0;   /// `pending_deletes` phase row
    uint64_t redelete_pool_wait_us = 0;     /// round-thread time blocked in its own future waits
```

`CasGc.h` declarations: delete the private nested `struct RedeleteIo` and declare it at `DB::Cas` namespace scope above `class Gc` (so the anonymous-namespace free function in `CasGc.cpp` can name it); replace the declarations of `performRedeleteIo`, `applyRedeleteOutcome`, `redeleteBlob` (if Task 3 left any trace), `redeleteBlobs` with the Interfaces block (`performRedeleteIo` leaves the class), and add

```cpp
    size_t redeleteWindowPeakForTest() const { return redelete_window_peak.load(); }
    void armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::exception_ptr e) { armed_meta_delete_failure = std::move(e); }
    LoggerPtr loggerForTest() const { return logger; }
private:
    std::atomic<size_t> redelete_window_peak{0};
    std::exception_ptr armed_meta_delete_failure;
```

`CasGcMetaWriter`: add `void armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::exception_ptr e) { armed_confirmed_delete_failure = std::move(e); }` and the member; at the top of `scheduleConfirmedMetaDelete`, before it builds and submits its operation: `if (armed_confirmed_delete_failure) { injected_schedule_failure = std::move(armed_confirmed_delete_failure); armed_confirmed_delete_failure = {}; }` so the failure surfaces inside `submit` through the existing injection point and takes `submit`'s real propagation path. `Gc` gets `void armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::exception_ptr e)` storing it in a member; in `runRegularRound`, right after constructing `round`: `if (armed_meta_delete_failure) { round.metaWriter().armScheduleFailureOnNextConfirmedMetaDeleteForTest(std::move(armed_meta_delete_failure)); armed_meta_delete_failure = {}; }`. Add `LoggerPtr loggerForTest() const { return logger; }` to `Gc`.

`PoolConfig::gc_redelete_apply_hook_for_test` type becomes `std::function<void(const BlobRef &)>`.

`CasGc.cpp`, anonymous namespace, `performRedeleteIo` as a free function with the restored comment:

```cpp
/// The condemned incarnation is a PERSISTED pair and cannot itself be a precondition, so the round
/// observes the blob and compares the two renderings. Observing first also settles the absent case
/// without spending a conditional delete against a key that is already gone.
RedeleteIo performRedeleteIo(const RetiredEntry & entry, const Layout & layout, CasOperation & op)
{
    RedeleteIo io;
    io.blob_key = layout.blobKey(entry.ref);
    const std::optional<Meta> observed = op.head(io.blob_key, Retry::standard());
    if (observed)
        io.del = entry.token.matches(observed->etag) ? op.remove(io.blob_key, observed->etag, Retry::standard()) : Removal::Mismatch;
    return io;
}
```

`applyRedeleteOutcome`:

```cpp
void Gc::applyRedeleteOutcome(const RetiredEntry & entry, const RedeleteIo & io, RedeleteRoundContext & ctx)
{
    if (const auto & hook = store->poolConfig().gc_redelete_apply_hook_for_test)
        hook(entry.ref);
    const OutcomeKind outcome_kind = io.del == Removal::Removed ? OutcomeKind::Deleted
        : io.del == Removal::Gone                               ? OutcomeKind::Absent
                                                                : OutcomeKind::Replaced;
    OutcomeEntry outcome{.kind = entry.kind, .ref = entry.ref, .token = entry.token, .outcome = outcome_kind};
    const String del_outcome{removalName(io.del)};
    /// The single content-delete site is attributable per row. A mismatch (a writer recreated the
    /// incarnation) is terminal-OK: the fresh incarnation is a live object.
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::BlobDelete;
        e.object_kind = CasEventObjectKind::Blob;
        e.object_hash = blobIdOf(entry.ref);
        e.token = renderIncarnation(entry.token);
        e.round = ctx.new_round;
        e.gen = ctx.round_generation;
        e.outcome = del_outcome;
        e.reason = "delete_pending published by a prior pass; exact-incarnation delete (pre-CAS)";
        e.detail = {{"condemn_round", std::to_string(entry.condemn_round)}, {"key", io.blob_key}};
    });
    /// The audit row is observability only -- the delete above already executed regardless of this
    /// cap. Skipping it here bounds the per-shard `GcOutcomes` body without skipping or deferring any
    /// destructive work.
    if (ctx.budget.outcomeEntryAvailable())
    {
        ctx.outcome_log.entries.push_back(std::move(outcome));
        ++ctx.budget.outcome_entries_used;
    }
    ++ctx.report.redeleted;
    ProfileEvents::increment(ProfileEvents::CASGCRetiredRedeleted);
    /// Drop the per-hash meta only on a removal or a proven absence -- a mismatch means a writer already
    /// resurrected a fresh incarnation at this hash, and that writer's own republication path already
    /// flipped the meta back to Clean; blindly deleting here would race that legitimate Clean write for
    /// no reason (the meta is advisory, but there is no reason to touch it on that path at all).
    ///
    /// `scheduleConfirmedMetaDelete` can propagate a framework failure surfacing through the meta
    /// writer's submit. This entry is then already counted and its event emitted, but its condemn
    /// marker is not yet forgotten: the round fails, nothing durable is committed, and the next round
    /// re-observes the blob absent and forgets the marker then (see `RoundReport::redeleted`).
    if (io.del == Removal::Removed || io.del == Removal::Gone)
        ctx.meta_writer.scheduleConfirmedMetaDelete(entry.ref);
    /// The entry left the pipeline -- drop its in-process condemn-marker confirmation.
    ctx.meta_writer.forgetCondemnMarker(entry.ref, entry.token);
}
```

`redeleteBlobs`:

```cpp
void Gc::redeleteBlobs(const std::vector<RetiredEntry> & entries, ThreadPool & pool, size_t concurrency,
                       CasOperation & op, RedeleteRoundContext & ctx)
{
    if (entries.empty())
        return;

    /// Declaration order is load-bearing: the runner, then the caller-owned futures, then the guard
    /// that joins them. Locals die in reverse order, so the guard runs first and waits while the deque
    /// and the runner are still alive; no callback can write into a destroyed slot on any exit path.
    struct Slot
    {
        size_t index;
        std::future<RedeleteIo> future;
    };
    ThreadPoolCallbackRunnerLocal<RedeleteIo> runner(pool, ThreadName::CAS_GC_REDELETE);
    std::deque<Slot> pending;
    Stopwatch wait_watch;
    uint64_t wait_us = 0;
    SCOPE_EXIT({
        for (Slot & slot : pending)
            if (slot.future.valid())
                slot.future.wait();
    });

    const uint64_t gen = op.generation();
    const size_t window = 2 * std::max<size_t>(1, concurrency);
    const Layout * layout = &store->layout();
    const PoolPtr store_ptr = store;
    const size_t refuse_at = store->poolConfig().gc_io_pool_refuse_at_for_test;

    /// Bookkeeping is bounded by the window, not by the batch. A failure is counted when it is seen and
    /// buffered for logging; the buffer is flushed only after every accepted job of the current window
    /// has been joined (spec 3.3: join before logging), and it never holds more than `window` entries
    /// because a full buffer forces a flush. Only the LOWEST-INDEX failure is kept for the rethrow, by
    /// index, so a refusal recorded early in time never masks an earlier entry's worker failure.
    struct Failure
    {
        size_t index;
        std::exception_ptr error;
    };
    std::vector<Failure> to_log;
    std::optional<Failure> first_failure;
    std::optional<size_t> refused_at;
    size_t submitted = 0;
    size_t enqueue_calls = 0;

    const auto join_pending = [&]
    {
        Stopwatch join_watch;   /// its own timer: this can run from inside a timed section
        for (Slot & slot : pending)
            if (slot.future.valid())
                slot.future.wait();
        wait_us += join_watch.elapsedMicroseconds();
    };
    const auto flush_log = [&]
    {
        if (to_log.empty())
            return;
        join_pending();
        for (const Failure & f : to_log)
            tryLogException(f.error, logger, fmt::format(
                "CAS gc: pending delete of blob {} (key {}, condemned at round {}) failed this round; it stays delete_pending and is retried",
                blobIdOf(entries[f.index].ref), layout->blobKey(entries[f.index].ref), entries[f.index].condemn_round),
                LogsLevel::warning);
        to_log.clear();
    };
    const auto note_failure = [&](size_t i, std::exception_ptr e)
    {
        ++ctx.report.redelete_failed;
        ProfileEvents::increment(ProfileEvents::CASGCRetiredRedeleteFailed);
        if (!first_failure || i < first_failure->index)
            first_failure = Failure{i, e};
        to_log.push_back(Failure{i, std::move(e)});
        if (to_log.size() >= window)
            flush_log();
    };

    /// Submits entries [submitted, limit). The entry copy happens before the slot exists, the slot is
    /// pushed BEFORE the enqueue so an allocation failure cannot strand an accepted task outside the join
    /// guard, and the slot is popped on ANY exception from the enqueue so no invalid future is ever
    /// consumed as a failed entry.
    const auto submit_up_to = [&](size_t limit)
    {
        while (submitted < limit && !refused_at)
        {
            const size_t i = submitted;
            const RetiredEntry entry = entries[i];   /// by value: the job must outlive nothing of ours; copied BEFORE the slot exists
            pending.push_back(Slot{.index = i, .future = {}});
            try
            {
                if (refuse_at != 0 && ++enqueue_calls == refuse_at)
                    throw Exception(ErrorCodes::CANNOT_SCHEDULE_TASK, "injected enqueue refusal");
                auto task = runner.enqueueAndGiveOwnership([store_ptr, gen, entry, layout]() -> RedeleteIo
                {
                    CasOperation job_op = store_ptr->openRequests().resume(gen);
                    return performRedeleteIo(entry, *layout, job_op);
                });
                pending.back().future = std::move(task->future);
            }
            catch (const Exception & e)
            {
                pending.pop_back();
                if (e.code() != ErrorCodes::CANNOT_SCHEDULE_TASK)
                    throw;   /// a framework failure surfacing through scheduling: propagate as such
                refused_at = i;
                note_failure(i, std::current_exception());
                return;
            }
            catch (...)
            {
                pending.pop_back();
                throw;
            }
            ++submitted;
            size_t peak = redelete_window_peak.load();
            while (pending.size() > peak && !redelete_window_peak.compare_exchange_weak(peak, pending.size()))
            {
            }
        }
    };

    std::exception_ptr apply_error;
    try
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            submit_up_to(std::min(entries.size(), i + window));
            if (refused_at && i >= *refused_at)
                break;
            Slot slot = std::move(pending.front());
            pending.pop_front();
            chassert(slot.index == i);
            wait_watch.restart();
            RedeleteIo io;
            try
            {
                io = slot.future.get();
            }
            catch (...)
            {
                wait_us += wait_watch.elapsedMicroseconds();
                note_failure(i, std::current_exception());
                continue;
            }
            wait_us += wait_watch.elapsedMicroseconds();
            applyRedeleteOutcome(entries[i], io, ctx);
        }
    }
    catch (...)
    {
        /// Apply-phase failure: join and inspect what is still outstanding so no worker failure goes
        /// unlogged or uncounted, then let the apply exception win.
        apply_error = std::current_exception();
        while (!pending.empty())
        {
            Slot slot = std::move(pending.front());
            pending.pop_front();
            Stopwatch one_wait;   /// per future, so a flush inside note_failure cannot double count
            std::exception_ptr failure;
            try
            {
                (void)slot.future.get();
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            wait_us += one_wait.elapsedMicroseconds();
            if (failure)
                note_failure(slot.index, std::move(failure));
        }
    }
    flush_log();   /// everything accepted is joined by now; emit whatever is still buffered

    ctx.report.redelete_jobs_scheduled += submitted;
    ctx.report.redelete_pool_wait_us += wait_us;

    if (apply_error)
        std::rethrow_exception(apply_error);
    if (first_failure)
        std::rethrow_exception(first_failure->error);
}
```

Call site: per shard, `RedeleteRoundContext ctx{.new_round = new_round, .round_generation = generation, .budget = round_work_budget, .report = report, .outcome_log = outcomes[shard], .meta_writer = round.metaWriter()}; redeleteBlobs(redelete_now, round.ioPool(), round.concurrency(), op, ctx);`. Set `report.gc_concurrency = round.concurrency();` right after constructing `round`.

Phase metrics, so failing rounds get them too: immediately after `pending_deletes_timer.emplace(phase_sink, "pending_deletes");` add

```cpp
    /// Requests performed on I/O pool workers land on those workers' ProfileEvents, not on this row's
    /// delta (the gap `meta_pool_wait` documents); read `jobs_scheduled`, `jobs_failed` and `pool_wait_ms`
    /// for this phase's shape. Set from a guard so a round that throws inside the phase still emits them.
    /// Pre-insert the three keys now, on the normal path, so the guard below only overwrites existing
    /// map entries and cannot allocate; a scope guard's destructor is noexcept and an allocation
    /// failure there would terminate instead of preserving the round's exception.
    pending_deletes_timer->metric("jobs_scheduled", 0);
    pending_deletes_timer->metric("jobs_failed", 0);
    pending_deletes_timer->metric("pool_wait_ms", 0);
    const auto set_pending_deletes_metrics = [&]() noexcept
    {
        try
        {
            if (!pending_deletes_timer)
                return;
            pending_deletes_timer->metric("jobs_scheduled", report.redelete_jobs_scheduled);
            pending_deletes_timer->metric("jobs_failed", report.redelete_failed);
            pending_deletes_timer->metric("pool_wait_ms", report.redelete_pool_wait_us / 1000);
        }
        catch (...)
        {
        }
    };
    SCOPE_EXIT({ set_pending_deletes_metrics(); });
```

and call `set_pending_deletes_metrics();` right before the existing `pending_deletes_timer.reset();`.

- [ ] **Step 4: Run**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CASGCRedeleteConcurrency.*:CASGCRoundPools.*' > build/test_task4.log 2>&1; echo EXIT=$?; grep -E '^\[  (PASSED|FAILED)  \]' build/test_task4.log`
Expected: EXIT=0, PASSED with a non-zero count.

- [ ] **Step 5: Commit**

```bash
git commit -m "cas: pending_deletes fan-out on the round's I/O pool with a bounded window; per-slot failures logged and counted" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMetaWriter.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Common/ProfileEvents.cpp src/Common/setThreadName.h src/Disks/tests/gtest_cas_gc_redelete_concurrency.cpp
```

---

### Task 5: Round log and `SYSTEM CAS GC RUN` carry `entries_redelete_failed` and `gc_concurrency` (spec 3.5) {#task-5-round-log-and-system-cas-gc-run-carry-entries-redelete-failed-and-gc-concurrency-spec-3-5}

**Files:**
- Modify: `Gc/CasGcScheduler.h` (grep `UInt64 entries_redeleted = 0;`, line 57 in `antalya-26.6`), `Gc/CasGcScheduler.cpp` (grep `fin.entries_redeleted = r.redeleted;` inside `fill_counters`)
- Modify: `ContentAddressedMetadataStorage.cpp` (grep `e.entries_redeleted = r.entries_redeleted;`)
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.h` (grep `UInt64 entries_redeleted = 0;`), `.cpp` (grep `{"entries_redeleted",` and `columns[i++]->insert(entries_redeleted);`)
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (grep `{"entries_redeleted", std::make_shared<DataTypeUInt64>()},` and `res_columns[i++]->insert(rep.redeleted);`)
- Test: `src/Disks/tests/gtest_cas_gc_log.cpp`, `TEST(CASGCLog, EmitsStartFinishWithCounts)`

**Interfaces:**
- Consumes: `RoundReport::redelete_failed`, `RoundReport::gc_concurrency` (Task 4).
- Produces: `GcRoundLogRecord::entries_redelete_failed`, `GcRoundLogRecord::gc_concurrency` (both `UInt64`); log element fields and columns `entries_redelete_failed`, `gc_concurrency` placed right after `entries_redeleted` in every column list; the same two on the `SYSTEM CAS GC RUN` result.

- [ ] **Step 1: Write the failing test**

In `TEST(CASGCLog, EmitsStartFinishWithCounts)`, where the test inspects the FINISH row of a successful round (it already asserts `deleted`/`marked` counters on `rows`), add for the deleting round's FINISH row:

```cpp
    EXPECT_EQ(rows[deleting_finish_idx].entries_redelete_failed, 0u);
    EXPECT_EQ(rows[deleting_finish_idx].gc_concurrency, 16u);
```

- [ ] **Step 2: Run to verify failure**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1; echo EXIT=$?; tail -3 build/build.log`
Expected: build fails, no member `entries_redelete_failed`.

- [ ] **Step 3: Implement**

`CasGcScheduler.h` after `entries_redeleted`:

```cpp
    UInt64 entries_redelete_failed = 0;   /// pending deletes whose request failed on a worker, plus one per refused submission
    UInt64 gc_concurrency = 0;            /// worker count of each of the round's two GC pools
```

`fill_counters`: after `fin.entries_redeleted = r.redeleted;` add `fin.entries_redelete_failed = r.redelete_failed; fin.gc_concurrency = r.gc_concurrency;`.
`ContentAddressedMetadataStorage.cpp`: after `e.entries_redeleted = r.entries_redeleted;` add `e.entries_redelete_failed = r.entries_redelete_failed; e.gc_concurrency = r.gc_concurrency;`.
Log element header: the two fields after `entries_redeleted`. `getColumnsDescription`, after the `entries_redeleted` row:

```cpp
        {"entries_redelete_failed", std::make_shared<DataTypeUInt64>(), "Pending exact-token blob deletes counted as failed this round: requests that failed on a pool worker, plus one for a refused submission. Entries never submitted after a refusal are not counted. Each stays delete_pending and is retried next round."},
        {"gc_concurrency", std::make_shared<DataTypeUInt64>(), "Worker count of each of the round's two GC thread pools (the effective cas_gc_concurrency)."},
```

`appendToBlock`: `columns[i++]->insert(entries_redelete_failed); columns[i++]->insert(gc_concurrency);` after `entries_redeleted`. `InterpreterSystemQuery.cpp`: the two column entries after `entries_redeleted` in the round-columns list, and `res_columns[i++]->insert(rep.redelete_failed); res_columns[i++]->insert(rep.gc_concurrency);` after `res_columns[i++]->insert(rep.redeleted);`. Then `git grep -ln entries_redeleted tests/queries docs` and add the two columns wherever a reference file or doc enumerates the log's columns.

- [ ] **Step 4: Run**

Run: `ninja -C build unit_tests_dbms > build/build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CASGCLog.*:CASGCScheduler*:CASGCRedeleteConcurrency.*' > build/test_task5.log 2>&1; echo EXIT=$?; grep -E '^\[  (PASSED|FAILED)  \]' build/test_task5.log`
Expected: EXIT=0, PASSED with a non-zero count.

- [ ] **Step 5: Commit**

```bash
git commit -m "cas: round log and SYSTEM CAS GC RUN report entries_redelete_failed and gc_concurrency" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Interpreters/ContentAddressedGarbageCollectionLog.h src/Interpreters/ContentAddressedGarbageCollectionLog.cpp \
  src/Interpreters/InterpreterSystemQuery.cpp src/Disks/tests/gtest_cas_gc_log.cpp
```

---

### Task 6: Docs, changelog, PR body (spec 5) {#task-6-docs-changelog-pr-body-spec-5}

**Files:**
- Modify: `docs/en/operations/storing-data.md` (the `cas_gc_meta_pool_size` and `cas_gc_read_concurrency` bullets at lines 550 to 555 and the two bullets the PR added below them), `docs/en/antalya/cas/configuration.md` (rows at lines 108 to 111), `docs/en/antalya/cas/architecture/garbage-collection.md` (rows at lines 228 to 231)
- Modify: the PR description on GitHub

Each of the three pages gets all three pieces: the new row or bullet, the migration note, and the failure sentence.

- [ ] **Step 1: `configuration.md`**

Delete the four rows; add

```markdown
| `cas_gc_concurrency` | `16` | Worker count of each of the two thread pools a GC round creates and destroys: the I/O pool (fold read-ahead, orphan-manifest sweep, `pending_deletes` `HEAD` + conditional `DELETE` fan-out) and the meta-write pool. A pool-worker limit; `1` runs every fan-out one job at a time. No GC pool thread exists between rounds |
```

and after the table:

```markdown
Migration: `cas_gc_read_concurrency` and `cas_gc_meta_pool_size` are replaced by `cas_gc_concurrency`, and the
intermediate keys `cas_gc_redelete_concurrency` and `cas_gc_redelete_min_batch_size` are gone; a config that still
names any of the four fails to load with an unknown-setting error. A `pending_deletes` entry is recorded as
deleted only if its own `HEAD` and delete ran; entries whose request failed, or that were never submitted, stay
pending and are retried next round.
```

- [ ] **Step 2: `garbage-collection.md`**

Delete the four rows; add the same row in that table's style, and the same migration paragraph (all four removed keys) and failure sentence after the table, plus one sentence: "The pools exist only for the duration of a round."

- [ ] **Step 3: `storing-data.md`**

Replace the four bullets with

```markdown
- `cas_gc_concurrency` — `16` by default. Worker count of each of the two thread pools a GC round creates
  and destroys: the I/O pool, used by the fold's read-ahead, the orphan-manifest sweep and the
  `pending_deletes` phase (one `HEAD` and one conditional `DELETE` per blob), and the meta-write pool. It
  limits pool workers, not the round's simultaneous requests; `1` runs every fan-out one job at a time.
  Between rounds a disk holds no GC pool thread. An entry is recorded as deleted only if its own `HEAD`
  and delete ran; entries whose request failed, or that were never submitted, stay pending and are
  retried next round. Replaces `cas_gc_read_concurrency` and `cas_gc_meta_pool_size` (and the intermediate
  `cas_gc_redelete_concurrency` and `cas_gc_redelete_min_batch_size`); a config that still names any of the
  four fails to load with an unknown-setting error.
```

- [ ] **Step 4: PR body**

Follow `.github/PULL_REQUEST_TEMPLATE.md`. Category: Performance Improvement. Entry: "GC `pending_deletes` runs its `HEAD` and conditional `DELETE` requests in parallel on the round's I/O pool, shared with the fold's read-ahead and sized by the new `cas_gc_concurrency` (default 16). GC thread pools now live only for the duration of a round; idle GC pool workers per CAS disk drop to zero between rounds. Removed `cas_gc_read_concurrency` and `cas_gc_meta_pool_size`." Add `Related:` lines for the backlog item (the `BACKLOG.md` entry "GC per-disk thread pools → one server-wide pool" on `cas-gc-rebuild`) and for the spec path. Do not tick the TSan exclusion.

- [ ] **Step 5: Commit**

```bash
git commit -m "docs(cas): cas_gc_concurrency replaces the per-pool GC settings; round-scoped pools" -- \
  docs/en/operations/storing-data.md docs/en/antalya/cas/configuration.md docs/en/antalya/cas/architecture/garbage-collection.md
```

---

### Task 7: Acceptance before requesting review (spec 6 and 7) {#task-7-acceptance-before-requesting-review-spec-6-and-7}

- [ ] **Step 1: Sanitizer gates on the exact pushed tree**

```bash
mkdir -p build_asan build_tsan
cmake -S . -B build_asan -G Ninja -DSANITIZE=address > build_asan/cmake.log 2>&1 && ninja -C build_asan unit_tests_dbms > build_asan/build.log 2>&1 && build_asan/src/unit_tests_dbms --gtest_filter='CAS*' > build_asan/test_cas_gate.log 2>&1; echo ASAN_EXIT=$?
cmake -S . -B build_tsan -G Ninja -DSANITIZE=thread  > build_tsan/cmake.log 2>&1 && ninja -C build_tsan unit_tests_dbms > build_tsan/build.log 2>&1 && build_tsan/src/unit_tests_dbms --gtest_filter='CAS*' > build_tsan/test_cas_gate.log 2>&1; echo TSAN_EXIT=$?
grep -c "WARNING: ThreadSanitizer" build_tsan/test_cas_gate.log; grep -c "ERROR: AddressSanitizer" build_asan/test_cas_gate.log
```

Both exits 0, both greps 0, both logs ending in a `[  PASSED  ]` line with a non-zero count. Use a subagent to read each log and report only the summary. The "logged" requirement is asserted by the log-capture counts inside the tests (one line in `WorkerRemoveFault...`, `WorkerHeadFault...` and `ApplyPhaseFaultWithLaterWorkerFailure...`, two in `TwoFailures...` and `WorkerFailureBeforeARefusal...`); no manual log reading is needed.

- [ ] **Step 2: Thread census**

Server config with three CAS disks, modelled on `tests/config/config.d/cas_s3_storage_policy_for_merge_tree_by_default.xml` (the disk type is `object_storage` with `metadata_type` `cas`; the endpoint is the RustFS the stateless CAS lane uses, MinIO OSS does not enforce `If-Match` on `DELETE`):

```xml
<clickhouse>
  <storage_configuration>
    <disks>
      <cas_a>
        <type>object_storage</type><object_storage_type>s3</object_storage_type><metadata_type>cas</metadata_type>
        <http_keep_alive_timeout>30</http_keep_alive_timeout><http_keep_alive_max_requests>10000</http_keep_alive_max_requests>
        <cas_server_root_id>census-a</cas_server_root_id>
        <endpoint>http://localhost:11121/test/census_a/</endpoint>
        <access_key_id>clickhouse</access_key_id><secret_access_key>clickhouse</secret_access_key>
        <cas_scratch_path>census_a_scratch/</cas_scratch_path>
        <cas_gc_enabled>1</cas_gc_enabled><cas_gc_interval_sec>20</cas_gc_interval_sec>
      </cas_a>
      <cas_b>
        <type>object_storage</type><object_storage_type>s3</object_storage_type><metadata_type>cas</metadata_type>
        <http_keep_alive_timeout>30</http_keep_alive_timeout><http_keep_alive_max_requests>10000</http_keep_alive_max_requests>
        <cas_server_root_id>census-b</cas_server_root_id>
        <endpoint>http://localhost:11121/test/census_b/</endpoint>
        <access_key_id>clickhouse</access_key_id><secret_access_key>clickhouse</secret_access_key>
        <cas_scratch_path>census_b_scratch/</cas_scratch_path>
        <cas_gc_enabled>1</cas_gc_enabled><cas_gc_interval_sec>20</cas_gc_interval_sec>
      </cas_b>
      <cas_c>
        <type>object_storage</type><object_storage_type>s3</object_storage_type><metadata_type>cas</metadata_type>
        <http_keep_alive_timeout>30</http_keep_alive_timeout><http_keep_alive_max_requests>10000</http_keep_alive_max_requests>
        <cas_server_root_id>census-c</cas_server_root_id>
        <endpoint>http://localhost:11121/test/census_c/</endpoint>
        <access_key_id>clickhouse</access_key_id><secret_access_key>clickhouse</secret_access_key>
        <cas_scratch_path>census_c_scratch/</cas_scratch_path>
        <cas_gc_enabled>1</cas_gc_enabled><cas_gc_interval_sec>20</cas_gc_interval_sec>
      </cas_c>
    </disks>
    <policies>
      <cas_a><volumes><main><disk>cas_a</disk></main></volumes></cas_a>
      <cas_b><volumes><main><disk>cas_b</disk></main></volumes></cas_b>
      <cas_c><volumes><main><disk>cas_c</disk></main></volumes></cas_c>
    </policies>
  </storage_configuration>
</clickhouse>
```

Start the server, wait `cas_gc_interval_sec` plus 10 s, run
`SELECT metric, value FROM system.metrics WHERE metric LIKE 'CASGC%PoolThreads'`
Expected: both rows 0. Record the output in the PR body.

- [ ] **Step 3: Bench**

Reproduce a `pending_deletes` backlog of at least 21k blobs on `cas_a` and time the phase at three settings. The per-round cohort caps default to 5000 (`cas_gc_round_graduation_budget`, `cas_gc_round_redelete_budget`, `cas_gc_round_outcome_entry_budget`), which would split the backlog over five rounds; set all three to `100000` on `cas_a` for the bench so one round carries the whole cohort. One part carries several blobs (columns, marks, primary index, checksums), so 21000 one-row parts give well over 21k blobs; the exact backlog is read back from the phase row's `jobs_scheduled`, which is the number to report.

```sql
CREATE TABLE bench (n UInt64) ENGINE = MergeTree ORDER BY n PARTITION BY n % 16
SETTINGS storage_policy = 'cas_a', parts_to_throw_insert = 1000000, parts_to_delay_insert = 1000000, inactive_parts_to_throw_insert = 1000000;
SYSTEM STOP MERGES bench;
INSERT INTO bench SELECT number FROM numbers(21000) SETTINGS max_block_size = 1, min_insert_block_size_rows = 1, min_insert_block_size_bytes = 0, max_insert_threads = 1;
SELECT count() FROM system.parts WHERE table = 'bench' AND active;   -- expect 21000
DROP TABLE bench SYNC;
```

Then drive GC by hand until the redelete round happens (the pipeline needs the condemn round, the graduation round and the redelete round, plus watermark renewals between them):

```sql
SYSTEM CAS GC RUN 'cas_a';   -- repeat, reading the result row, until entries_redeleted > 0
SYSTEM FLUSH LOGS;   -- cas_gc_log flushes every 7.5 s by default; do not read it before flushing
SELECT p.round_id, p.event_time, p.phase_duration_microseconds / 1e6 AS s,
       p.phase_metrics['jobs_scheduled'] AS blobs, p.phase_metrics['jobs_failed'] AS failed, p.phase_metrics['pool_wait_ms'] AS wait_ms, f.gc_concurrency
FROM system.cas_gc_log AS p
INNER JOIN system.cas_gc_log AS f ON f.round_id = p.round_id AND f.event_type = 'Finish'
WHERE p.disk_name = 'cas_a' AND p.phase = 'pending_deletes' AND p.phase_metrics['jobs_scheduled'] >= 21000
  AND f.gc_concurrency = 4   -- the setting of THIS run; rerun with 1 and 16
ORDER BY p.event_time DESC LIMIT 1;
-- the FINISH row's gc_concurrency ties the phase row to the run being measured; report `blobs` as the backlog size
```

Repeat the whole sequence with `cas_gc_concurrency` 1, 4 and 16 on the disk (restart between runs; the table recreate rebuilds the backlog), and record next to the results the disk's S3 client max-connections setting (`max_connections` on the disk; default from `S3::ClientSettings`). Put the table in the PR body in place of the original bench table.

- [ ] **Step 4: CI**

Push; every red on the stateless CAS S3 lanes must be traced to a cause outside the PR before requesting review. Known lane issues, not this PR's: the ASan memory-accounting storm (fix in PR #2349) and the ASan fake-stack degradation on fiber threads (backlog item "ASan memory ceiling on stateless lanes = thread count × fake stack").

- [ ] **Step 5: Spec section 7 verification items**

Record in the PR body: (1) `rebuildBaseline` uses no meta writer; its `GcRound` still builds one, which is symmetric and costs no thread until a job is submitted. (2) `ThreadPoolCallbackRunnerLocal` attaches the submitting thread's group (null on a bare test thread, accepted by `ThreadGroupSwitcher`); confirm `CASRequestAttempt` in `system.events` grows across a round on the census server, which shows `CasOperation` counts requests on the workers as before. (3) `git grep -n "metaWriter()\|meta_writer" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed` shows uses only inside `fold`, `applyRedeleteOutcome`, `runRegularRound`, `rebuildBaseline` and `GcRound`.
