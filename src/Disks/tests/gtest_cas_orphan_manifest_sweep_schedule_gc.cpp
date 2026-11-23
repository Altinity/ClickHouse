#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include "cas_test_helpers.h"

#include <exception>
#include <functional>
#include <map>
#include <utility>

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace
{

class ScheduleBackend : public CountingBackend
{
public:
    String manifests_prefix;
    String state_key;
    String fail_list_prefix;
    std::function<void()> before_manifest_list;
    std::function<void()> before_round_commit;

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        if (prefix == manifests_prefix && before_manifest_list)
            std::exchange(before_manifest_list, {})();
        if (!fail_list_prefix.empty() && prefix == fail_list_prefix)
        {
            fail_list_prefix.clear();
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test generation list failure");
        }
        return CountingBackend::list(prefix, cursor, limit, access);
    }

    std::expected<String, RawConflict> write(
        const String & key, const String & bytes, const std::optional<String> & expected, TransportAccess & access) override
    {
        if (key == state_key && before_round_commit && decodeGcState(bytes).round > 0)
            std::exchange(before_round_commit, {})();
        return CountingBackend::write(key, bytes, expected, access);
    }
};

struct ScheduleFixture
{
    std::shared_ptr<ScheduleBackend> backend = std::make_shared<ScheduleBackend>();
    PoolConfig config;
    PoolPtr store;
    uint64_t now = 1000;
    std::map<String, GcPhaseRecord> phases;
    std::unique_ptr<Gc> gc;

    explicit ScheduleFixture(uint64_t list_budget = 1000, uint64_t interval = 3600, uint64_t defer_rounds = 0)
    {
        config.pool_prefix = "p";
        config.server_root_id = "test";
        config.manifest_sweep_list_budget_keys = list_budget;
        config.orphan_manifest_sweep_interval_sec = interval;
        config.gc_fold_max_defer_rounds = defer_rounds;
        store = Pool::open(backend, config);
        backend->manifests_prefix = store->layout().casManifestsPrefix();
        backend->state_key = store->layout().gcStateKey();
        makeGc();
    }

    void makeGc(UInt128 id = UInt128(0xAB))
    {
        gc = std::make_unique<Gc>(store, id, [this] { return now; });
        gc->setPhaseSink([this](const GcPhaseRecord & record) { phases[record.phase] = record; });
    }

    RoundReport run(UniversePolicy policy = UniversePolicy::Authoritative)
    {
        phases.clear();
        return gc->runRegularRound({}, true, policy);
    }

    GcState state()
    {
        OperationForTest op(*backend);
        auto got = (*op).read(backend->state_key, Retry::once());
        return got ? decodeGcState(got->bytes) : GcState{};
    }

    void setState(const GcState & value)
    {
        OperationForTest op(*backend);
        auto got = (*op).read(backend->state_key, Retry::once());
        if (got)
            orThrow((*op).replace(backend->state_key, encodeGcState(value), got->etag, Retry::once()), "test state replace");
        else
        {
            auto initial = value;
            initial.lease = GcLease{UInt128(0xAB), 1};
            orThrow((*op).create(backend->state_key, encodeGcState(initial), Retry::once()), "test state create");
        }
    }

    void addKeys()
    {
        OperationForTest op(*backend);
        orThrow((*op).create(backend->manifests_prefix + "invalid-a", "a", Retry::once()), "test key a");
        orThrow((*op).create(backend->manifests_prefix + "invalid-b", "b", Retry::once()), "test key b");
    }

    uint64_t metric(const String & phase, const String & name)
    {
        const auto & metrics = phases.at(phase).metrics;
        EXPECT_TRUE(metrics.contains(name)) << phase << ":" << name;
        return metrics.contains(name) ? metrics.at(name) : 0;
    }
};

void expectSameSchedule(const GcState & actual, const GcState & expected)
{
    EXPECT_EQ(actual.manifest_sweep_cursor, expected.manifest_sweep_cursor);
    EXPECT_EQ(actual.orphan_manifest_sweep_completed_at_ms, expected.orphan_manifest_sweep_completed_at_ms);
}

}

TEST(CASOrphanManifestSweepScheduleGc, IdleSweepFollowsOrdinaryFoldCadence)
{
    for (const uint64_t interval : {0, 3600})
    {
        SCOPED_TRACE(interval);
        ScheduleFixture f(1, interval, 8);
        f.addKeys();
        for (size_t page = 0; page < 3; ++page)
        {
            SCOPED_TRACE(page);
            if (page == 1)
                f.now = 1001;
            if (page == 2)
                f.now = 1001 + interval * 1000;
            const auto adopted = f.state();
            f.backend->resetCounts();
            for (size_t tick = 0; tick < 8; ++tick)
            {
                const auto deferred = f.run();
                ASSERT_TRUE(deferred.acquired_lease);
                ASSERT_TRUE(deferred.deferred);
                EXPECT_EQ(f.metric("defer_decision", "run_orphan_manifest_sweep"), 1);
                EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
                expectSameSchedule(f.state(), adopted);
            }
            EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
            const auto folded = f.run();
            ASSERT_TRUE(folded.acquired_lease);
            ASSERT_FALSE(folded.deferred);
            EXPECT_EQ(f.state().snap_generation, adopted.snap_generation + 1);
            EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
            EXPECT_EQ(f.state().manifest_sweep_cursor.empty(), page == 1);
            EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, page == 0 ? 0 : 1001);
        }
    }
}

TEST(CASOrphanManifestSweepScheduleGc, WaitingPreservesOldDeferDecision)
{
    ScheduleFixture f(1000, 3600, 1000);
    auto state = f.state();
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    f.setState(state);
    EXPECT_TRUE(f.run().deferred);
    EXPECT_EQ(f.metric("defer_decision", "run_orphan_manifest_sweep"), 0);
    expectSameSchedule(f.state(), state);
}

TEST(CASOrphanManifestSweepScheduleGc, TwoPagesDoNotWaitAnHour)
{
    ScheduleFixture f(1);
    f.addKeys();
    ASSERT_TRUE(f.run().acquired_lease);
    const auto first = f.state();
    EXPECT_EQ(first.orphan_manifest_sweep_completed_at_ms, 0);
    EXPECT_FALSE(first.manifest_sweep_cursor.empty());
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    const auto second = f.state();
    EXPECT_TRUE(second.manifest_sweep_cursor.empty());
    EXPECT_EQ(second.orphan_manifest_sweep_completed_at_ms, 1001);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, NominationBudgetPreservesPreviousCompletion)
{
    ScheduleFixture f;
    f.gc.reset();
    f.store.reset();
    f.config.manifest_sweep_delete_budget_keys = 1;
    f.store = Pool::open(f.backend, f.config);
    f.makeGc();

    const RootNamespace ns{"orphan/table"};
    writeManifestRaw(*f.backend, f.store->layout(), ns, ManifestRef{1, 1, 1}, {});
    writeManifestRaw(*f.backend, f.store->layout(), ns, ManifestRef{1, 2, 2}, {});
    setWatermarkMinActive(*f.backend, f.store->layout(), "orphan", 1, 3);
    auto state = f.state();
    state.orphan_manifest_sweep_completed_at_ms = 500;
    f.setState(state);
    f.now = 3600500;

    ASSERT_TRUE(f.run().acquired_lease);
    const auto first = f.state();
    EXPECT_FALSE(first.manifest_sweep_cursor.empty());
    EXPECT_EQ(first.orphan_manifest_sweep_completed_at_ms, 500);
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    const auto second = f.state();
    EXPECT_TRUE(second.manifest_sweep_cursor.empty());
    EXPECT_EQ(second.orphan_manifest_sweep_completed_at_ms, f.now);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, WaitingDoesNoSweepIo)
{
    ScheduleFixture f;
    ASSERT_TRUE(f.run().acquired_lease);
    const auto before = f.state();
    f.backend->resetCounts();
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
    EXPECT_EQ(f.metric("orphan_sweep", "orphan_manifest_sweep_page_listed"), 0);
    expectSameSchedule(f.state(), before);
}

TEST(CASOrphanManifestSweepScheduleGc, ZeroIntervalRestoresCadence)
{
    ScheduleFixture f(1000, 0);
    ASSERT_TRUE(f.run().acquired_lease);
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
    EXPECT_EQ(f.metric("orphan_sweep", "orphan_manifest_sweep_page_listed"), 1);
}

TEST(CASOrphanManifestSweepScheduleGc, SkippedResultDoesNotResetCursor)
{
    ScheduleFixture f(0);
    auto state = f.state();
    state.manifest_sweep_cursor = "cursor";
    f.setState(state);
    ASSERT_TRUE(f.run().acquired_lease);
    expectSameSchedule(f.state(), state);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
}

TEST(CASOrphanManifestSweepScheduleGc, SuppressedRoundPreservesSchedule)
{
    ScheduleFixture f(1);
    f.addKeys();
    ASSERT_TRUE(f.run().acquired_lease);
    const auto before = f.state();
    ASSERT_FALSE(before.manifest_sweep_cursor.empty());
    ++f.now;
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    expectSameSchedule(f.state(), before);
}

TEST(CASOrphanManifestSweepScheduleGc, ZeroClockCommitsPagesAndResumesInterval)
{
    ScheduleFixture f(1);
    f.addKeys();
    f.backend->before_manifest_list = [&] { f.now = 0; };
    ASSERT_NO_THROW(f.run());
    const auto first_page = f.state();
    EXPECT_EQ(first_page.snap_generation, 1);
    ASSERT_FALSE(first_page.manifest_sweep_cursor.empty());
    EXPECT_EQ(first_page.orphan_manifest_sweep_completed_at_ms, 0);

    ASSERT_NO_THROW(f.run());
    const auto completed = f.state();
    EXPECT_EQ(completed.snap_generation, 2);
    EXPECT_TRUE(completed.manifest_sweep_cursor.empty());
    EXPECT_EQ(completed.orphan_manifest_sweep_completed_at_ms, 0);

    ASSERT_NO_THROW(f.run());
    EXPECT_EQ(f.state().snap_generation, 3);
    ASSERT_FALSE(f.state().manifest_sweep_cursor.empty());
    f.now = 1000;
    ASSERT_NO_THROW(f.run());
    const auto timed = f.state();
    EXPECT_EQ(timed.snap_generation, 4);
    EXPECT_TRUE(timed.manifest_sweep_cursor.empty());
    EXPECT_EQ(timed.orphan_manifest_sweep_completed_at_ms, 1000);

    f.backend->resetCounts();
    ++f.now;
    EXPECT_FALSE(f.run().deferred);
    EXPECT_EQ(f.state().snap_generation, timed.snap_generation + 1);
    expectSameSchedule(f.state(), timed);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
}

TEST(CASOrphanManifestSweepScheduleGc, ZeroClockPreservesPreviousCompletion)
{
    ScheduleFixture f(1);
    ASSERT_TRUE(f.run().acquired_lease);
    const auto baseline = f.state();
    ASSERT_EQ(baseline.orphan_manifest_sweep_completed_at_ms, 1000);
    f.addKeys();
    f.now = 3601000;
    f.backend->before_manifest_list = [&] { f.now = 0; };
    ASSERT_NO_THROW(f.run());
    EXPECT_EQ(f.state().snap_generation, baseline.snap_generation + 1);
    ASSERT_FALSE(f.state().manifest_sweep_cursor.empty());
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);

    ASSERT_NO_THROW(f.run());
    const auto completed = f.state();
    EXPECT_EQ(completed.snap_generation, baseline.snap_generation + 2);
    EXPECT_TRUE(completed.manifest_sweep_cursor.empty());
    EXPECT_EQ(completed.orphan_manifest_sweep_completed_at_ms, 1000);
    f.backend->resetCounts();
    EXPECT_FALSE(f.run().deferred);
    expectSameSchedule(f.state(), completed);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);

    f.now = 3601000;
    ASSERT_NO_THROW(f.run());
    ASSERT_FALSE(f.state().manifest_sweep_cursor.empty());
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
    ++f.now;
    ASSERT_NO_THROW(f.run());
    const auto timed = f.state();
    EXPECT_EQ(timed.snap_generation, baseline.snap_generation + 5);
    EXPECT_TRUE(timed.manifest_sweep_cursor.empty());
    EXPECT_EQ(timed.orphan_manifest_sweep_completed_at_ms, 3601001);
    f.backend->resetCounts();
    ++f.now;
    EXPECT_FALSE(f.run().deferred);
    expectSameSchedule(f.state(), timed);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
}

TEST(CASOrphanManifestSweepScheduleGc, ZeroClockWithZeroIntervalPreservesCompletion)
{
    ScheduleFixture f(1000, 0);
    ASSERT_TRUE(f.run().acquired_lease);
    const auto baseline = f.state();
    ASSERT_EQ(baseline.orphan_manifest_sweep_completed_at_ms, 1000);
    f.backend->resetCounts();
    f.backend->before_manifest_list = [&] { f.now = 0; };
    ASSERT_NO_THROW(f.run());
    EXPECT_EQ(f.state().snap_generation, baseline.snap_generation + 1);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
    ASSERT_NO_THROW(f.run());
    EXPECT_EQ(f.state().snap_generation, baseline.snap_generation + 2);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);

    f.now = 1001;
    ASSERT_NO_THROW(f.run());
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1001);
}

TEST(CASOrphanManifestSweepScheduleGc, ZeroClockLostCasDoesNotAdvanceCursor)
{
    ScheduleFixture f(1);
    f.addKeys();
    f.backend->before_manifest_list = [&] { f.now = 0; };
    bool fired = false;
    f.backend->before_round_commit = [&]
    {
        auto state = f.state();
        ++state.lease.seq;
        f.setState(state);
        fired = true;
    };
    EXPECT_THROW(f.run(), DB::Exception);
    ASSERT_TRUE(fired);
    const auto refused = f.state();
    EXPECT_EQ(refused.snap_generation, 0);
    EXPECT_EQ(refused.round, 0);
    EXPECT_TRUE(refused.manifest_sweep_cursor.empty());
    EXPECT_EQ(refused.orphan_manifest_sweep_completed_at_ms, 0);

    EXPECT_FALSE(f.run().deferred);
    EXPECT_EQ(f.state().snap_generation, 1);
    EXPECT_FALSE(f.state().manifest_sweep_cursor.empty());
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 0);
}

TEST(CASOrphanManifestSweepScheduleGc, FailedFoldDoesNotComplete)
{
    ScheduleFixture f;
    const auto before = f.state();
    f.backend->before_manifest_list = []
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test sweep list failure");
    };
    EXPECT_THROW(f.run(), DB::Exception);
    expectSameSchedule(f.state(), before);
}

TEST(CASOrphanManifestSweepScheduleGc, LostCasDoesNotAdvanceOrDelete)
{
    ScheduleFixture f;
    const RootNamespace ns{"orphan/table"};
    const ManifestRef ref{1, 1, 1};
    writeManifestRaw(*f.backend, f.store->layout(), ns, ref, {});
    setWatermarkMinActive(*f.backend, f.store->layout(), "orphan", 1, 2);
    const auto before = f.state();
    bool fired = false;
    f.backend->before_round_commit = [&]
    {
        fired = true;
        auto state = f.state();
        ++state.lease.seq;
        f.setState(state);
    };
    EXPECT_THROW(f.run(), DB::Exception);
    EXPECT_TRUE(fired);
    expectSameSchedule(f.state(), before);
    OperationForTest op(*f.backend);
    EXPECT_TRUE((*op).read(f.store->layout().manifestKey(ManifestId{ns, ref}), Retry::once()).has_value());
}

TEST(CASOrphanManifestSweepScheduleGc, CommittedRoundDeletesEligibleOrphan)
{
    ScheduleFixture f;
    const RootNamespace ns{"orphan/table"};
    const ManifestRef ref{1, 1, 1};
    writeManifestRaw(*f.backend, f.store->layout(), ns, ref, {});
    setWatermarkMinActive(*f.backend, f.store->layout(), "orphan", 1, 2);
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
    OperationForTest op(*f.backend);
    EXPECT_FALSE((*op).read(f.store->layout().manifestKey(ManifestId{ns, ref}), Retry::once()).has_value());
}

TEST(CASOrphanManifestSweepScheduleGc, TakeoverResumesCursor)
{
    ScheduleFixture f(1);
    f.addKeys();
    ASSERT_TRUE(f.run().acquired_lease);
    const auto first = f.state();
    ASSERT_FALSE(first.manifest_sweep_cursor.empty());
    f.makeGc(UInt128(0xAC));
    EXPECT_FALSE(f.run().acquired_lease);
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_TRUE(f.state().manifest_sweep_cursor.empty());
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, f.now);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, LeaseRenewalPreservesScheduleFields)
{
    ScheduleFixture f;
    ASSERT_TRUE(f.run().acquired_lease);
    const auto before = f.state();
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    const auto after = f.state();
    EXPECT_GT(after.lease.seq, before.lease.seq);
    expectSameSchedule(after, before);
}

TEST(CASOrphanManifestSweepScheduleGc, RestartReadsCommittedSchedule)
{
    ScheduleFixture f(1);
    f.addKeys();
    ASSERT_TRUE(f.run().acquired_lease);
    const auto before = f.state();
    ASSERT_FALSE(before.manifest_sweep_cursor.empty());
    f.gc.reset();
    f.store.reset();
    f.store = Pool::open(f.backend, f.config);
    f.makeGc();
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_TRUE(f.state().manifest_sweep_cursor.empty());
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, f.now);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, RebuildMakesNextSweepDue)
{
    ScheduleFixture f;
    ASSERT_TRUE(f.run().acquired_lease);
    ASSERT_NE(f.state().orphan_manifest_sweep_completed_at_ms, 0);
    ASSERT_TRUE(f.gc->rebuildBaseline(true).performed);
    const auto state = f.state();
    EXPECT_TRUE(state.manifest_sweep_cursor.empty());
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 0);
}

TEST(CASOrphanManifestSweepScheduleGc, LongRoundStampsCompletionNearCommit)
{
    ScheduleFixture f;
    f.backend->before_manifest_list = [&] { f.now = 3601000; };
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 3601000);
    f.backend->resetCounts();
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
}

TEST(CASOrphanManifestSweepScheduleGc, ListOnlyCanCompleteTraversal)
{
    ScheduleFixture f;
    f.addKeys();
    f.gc.reset();
    f.store.reset();
    f.config.manifest_sweep_delete_budget_keys = 0;
    f.store = Pool::open(f.backend, f.config);
    f.makeGc();
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
    EXPECT_EQ(f.metric("orphan_sweep", "orphan_manifest_sweep_page_listed"), 1);
}

TEST(CASOrphanManifestSweepScheduleGc, SuppressedSweepUsesBoundedDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).deferred);
    const auto first = f.run(UniversePolicy::StageA_Suppressed);
    ASSERT_TRUE(first.acquired_lease);
    ASSERT_FALSE(first.deferred);
    auto adopted = f.state();
    ASSERT_GT(adopted.snap_generation, 0);

    for (size_t cycle = 0; cycle < 2; ++cycle)
    {
        for (size_t tick = 0; tick < 2; ++tick)
        {
            const auto deferred = f.run(UniversePolicy::StageA_Suppressed);
            ASSERT_TRUE(deferred.acquired_lease);
            ASSERT_TRUE(deferred.deferred);
            EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
            expectSameSchedule(f.state(), adopted);
        }
        const auto retried = f.run(UniversePolicy::StageA_Suppressed);
        ASSERT_TRUE(retried.acquired_lease);
        EXPECT_FALSE(retried.deferred);
        EXPECT_GT(f.state().snap_generation, adopted.snap_generation);
        expectSameSchedule(f.state(), adopted);
        adopted = f.state();
    }
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
}

TEST(CASOrphanManifestSweepScheduleGc, FailedSweepUsesBoundedDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    const RootNamespace ns{"test/checkpoint"};
    casAdmitRecoverableEntry(*f.backend, f.store->layout(), ns);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    ASSERT_TRUE(f.run().acquired_lease);
    const auto adopted = f.state();
    ASSERT_EQ(adopted.orphan_manifest_sweep_completed_at_ms, 1000);
    const String checkpoint_key = f.store->layout().refCkptKey(fixture::fixtureLife(ns));
    f.now = 3601000;
    size_t attempts = 0;
    const auto fail_list = [&]
    {
        ++attempts;
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test sweep list failure");
    };
    f.backend->before_manifest_list = fail_list;
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    EXPECT_THROW(f.run(), DB::Exception);
    ASSERT_EQ(attempts, 1);

    for (size_t cycle = 0; cycle < 2; ++cycle)
    {
        f.backend->resetCounts();
        f.backend->before_manifest_list = fail_list;
        for (size_t tick = 0; tick < 2; ++tick)
        {
            const auto deferred = f.run();
            ASSERT_TRUE(deferred.acquired_lease);
            ASSERT_TRUE(deferred.deferred);
            EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
            expectSameSchedule(f.state(), adopted);
        }
        EXPECT_EQ(attempts, cycle + 1);
        EXPECT_EQ(f.backend->getCount(checkpoint_key), 0);
        EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
        EXPECT_THROW(f.run(), DB::Exception);
        EXPECT_EQ(attempts, cycle + 2);
        EXPECT_GT(f.backend->getCount(checkpoint_key), 0);
        EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
        expectSameSchedule(f.state(), adopted);
    }
}

TEST(CASOrphanManifestSweepScheduleGc, SealWriteFailureUsesBoundedDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    ASSERT_TRUE(f.run().acquired_lease);
    const auto adopted = f.state();
    f.now = 3601000;
    String seal_key;
    f.backend->before_manifest_list = [&]
    {
        const auto leased = f.state();
        seal_key = f.store->layout().foldSealKey(leased.snap_generation + 1, leased.lease.seq);
        f.backend->failNextWriteWith(seal_key,
            std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test seal write failure")));
    };
    f.backend->resetCounts();
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    EXPECT_THROW(f.run(), DB::Exception);
    ASSERT_FALSE(seal_key.empty());
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
    EXPECT_EQ(f.backend->writeCount(seal_key), 1);
    EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
    EXPECT_EQ(f.state().snap_attempt, adopted.snap_attempt);
    expectSameSchedule(f.state(), adopted);

    f.backend->resetCounts();
    for (size_t tick = 0; tick < 2; ++tick)
    {
        const auto deferred = f.run();
        ASSERT_TRUE(deferred.acquired_lease);
        ASSERT_TRUE(deferred.deferred);
        EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
        expectSameSchedule(f.state(), adopted);
    }
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
    const auto retried = f.run();
    ASSERT_TRUE(retried.acquired_lease);
    EXPECT_FALSE(retried.deferred);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
    EXPECT_EQ(f.state().snap_generation, adopted.snap_generation + 1);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, f.now);
}

TEST(CASOrphanManifestSweepScheduleGc, PruneFailureUsesBoundedDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    for (size_t generation = 0; generation < 3; ++generation)
    {
        for (size_t tick = 0; tick < 2; ++tick)
            ASSERT_TRUE(f.run().deferred);
        ASSERT_TRUE(f.run().acquired_lease);
        f.now += 3600000;
    }
    const auto adopted = f.state();
    ASSERT_EQ(adopted.snap_generation, 3);
    f.backend->fail_list_prefix = f.store->layout().gcGenPrefix(1);
    String seal_key;
    f.backend->before_manifest_list = [&]
    {
        const auto leased = f.state();
        seal_key = f.store->layout().foldSealKey(leased.snap_generation + 1, leased.lease.seq);
    };
    f.backend->resetCounts();
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    EXPECT_THROW(f.run(), DB::Exception);
    EXPECT_TRUE(f.backend->fail_list_prefix.empty());
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
    ASSERT_FALSE(seal_key.empty());
    {
        OperationForTest op(*f.backend);
        EXPECT_TRUE((*op).head(seal_key, Retry::once()).has_value());
    }
    EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
    EXPECT_EQ(f.state().snap_attempt, adopted.snap_attempt);
    expectSameSchedule(f.state(), adopted);

    f.backend->resetCounts();
    for (size_t tick = 0; tick < 2; ++tick)
    {
        const auto deferred = f.run();
        ASSERT_TRUE(deferred.acquired_lease);
        ASSERT_TRUE(deferred.deferred);
        EXPECT_EQ(f.state().snap_generation, adopted.snap_generation);
        expectSameSchedule(f.state(), adopted);
    }
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
    const auto retried = f.run();
    ASSERT_TRUE(retried.acquired_lease);
    EXPECT_FALSE(retried.deferred);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
    EXPECT_EQ(f.state().snap_generation, adopted.snap_generation + 1);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, f.now);
}

TEST(CASOrphanManifestSweepScheduleGc, SuccessfulPagesAfterFailureFollowOrdinaryFoldCadence)
{
    ScheduleFixture f(1, 3600, 2);
    f.addKeys();
    f.backend->before_manifest_list = []
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test sweep list failure");
    };
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    EXPECT_THROW(f.run(), DB::Exception);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    const auto recovered = f.run();
    ASSERT_TRUE(recovered.acquired_lease);
    ASSERT_FALSE(recovered.deferred);
    ASSERT_FALSE(f.state().manifest_sweep_cursor.empty());
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    const auto continued = f.run();
    ASSERT_TRUE(continued.acquired_lease);
    EXPECT_FALSE(continued.deferred);
    EXPECT_TRUE(f.state().manifest_sweep_cursor.empty());
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, ChangedRefsBypassFailedSweepDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    f.backend->before_manifest_list = []
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test sweep list failure");
    };
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    EXPECT_THROW(f.run(), DB::Exception);
    const RootNamespace ns{"test/changed"};
    const ManifestRef ref{1, 1, 1};
    writeManifestRaw(*f.backend, f.store->layout(), ns, ref, {});
    publishCommittedTransition(*f.backend, f.store->layout(), ns, "table", std::nullopt, ref);
    const auto changed = f.run();
    ASSERT_TRUE(changed.acquired_lease);
    EXPECT_FALSE(changed.deferred);
    EXPECT_EQ(f.metric("defer_decision", "changed_shards"), 1);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
}

TEST(CASOrphanManifestSweepScheduleGc, FailedSweepWithZeroDeferRetriesImmediately)
{
    ScheduleFixture f;
    f.backend->before_manifest_list = []
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "test sweep list failure");
    };
    EXPECT_THROW(f.run(), DB::Exception);
    const auto retried = f.run();
    ASSERT_TRUE(retried.acquired_lease);
    EXPECT_FALSE(retried.deferred);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
}

TEST(CASOrphanManifestSweepScheduleGc, CorruptCheckpointLimitsFoldReads)
{
    ScheduleFixture f(1000, 3600, 2);
    const RootNamespace ns{"test/checkpoint"};
    const ManifestRef ref{1, 1, 1};
    const auto & layout = f.store->layout();
    writeManifestRaw(*f.backend, layout, ns, ref, {});
    publishCommittedTransition(*f.backend, layout, ns, "table", std::nullopt, ref);
    ASSERT_TRUE(f.run().acquired_lease);
    const auto completed = f.state();
    ASSERT_EQ(completed.orphan_manifest_sweep_completed_at_ms, 1000);

    OperationForTest op(*f.backend);
    const String checkpoint_key = layout.refCkptKey(fixture::fixtureLife(ns));
    const auto checkpoint = (*op).read(checkpoint_key, Retry::once());
    ASSERT_TRUE(checkpoint.has_value());
    orThrow((*op).replace(checkpoint_key, "invalid checkpoint", checkpoint->etag, Retry::once()), "test corrupt checkpoint");
    f.now = 3601000;
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    const auto suppressed = f.run();
    ASSERT_TRUE(suppressed.acquired_lease);
    ASSERT_FALSE(suppressed.deferred);
    EXPECT_EQ(f.metric("orphan_sweep", "suppressed"), 1);
    const auto held = f.state();
    expectSameSchedule(held, completed);

    f.backend->resetCounts();
    for (size_t tick = 0; tick < 2; ++tick)
    {
        const auto deferred = f.run();
        ASSERT_TRUE(deferred.acquired_lease);
        ASSERT_TRUE(deferred.deferred);
        EXPECT_EQ(f.state().snap_generation, held.snap_generation);
        expectSameSchedule(f.state(), completed);
    }
    EXPECT_EQ(f.backend->getCount(checkpoint_key), 0);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);

    const auto retry = f.run();
    ASSERT_TRUE(retry.acquired_lease);
    ASSERT_FALSE(retry.deferred);
    EXPECT_GT(f.backend->getCount(checkpoint_key), 0);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
    EXPECT_EQ(f.metric("orphan_sweep", "suppressed"), 1);
    expectSameSchedule(f.state(), completed);
}

TEST(CASOrphanManifestSweepScheduleGc, RepairedCheckpointResumesSweepAtBoundedFold)
{
    ScheduleFixture f(1000, 3600, 2);
    const RootNamespace ns{"test/empty_checkpoint"};
    const auto & layout = f.store->layout();
    casAdmitRecoverableEntry(*f.backend, layout, ns);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    ASSERT_TRUE(f.run().acquired_lease);
    ASSERT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);

    OperationForTest op(*f.backend);
    const String checkpoint_key = layout.refCkptKey(fixture::fixtureLife(ns));
    const auto checkpoint = (*op).read(checkpoint_key, Retry::once());
    ASSERT_TRUE(checkpoint.has_value());
    orThrow((*op).replace(checkpoint_key, "invalid checkpoint", checkpoint->etag, Retry::once()), "test corrupt checkpoint");
    f.now = 3601000;
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    ASSERT_TRUE(f.run().acquired_lease);
    ASSERT_EQ(f.metric("orphan_sweep", "suppressed"), 1);
    const auto corrupted = (*op).read(checkpoint_key, Retry::once());
    ASSERT_TRUE(corrupted.has_value());
    orThrow((*op).replace(checkpoint_key, checkpoint->bytes, corrupted->etag, Retry::once()), "test restore checkpoint");
    f.backend->resetCounts();
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    EXPECT_EQ(f.backend->getCount(checkpoint_key), 0);
    const auto recovered = f.run();
    ASSERT_TRUE(recovered.acquired_lease);
    EXPECT_FALSE(recovered.deferred);
    EXPECT_EQ(f.metric("orphan_sweep", "suppressed"), 0);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 3601000);
}

TEST(CASOrphanManifestSweepScheduleGc, ChangedRefsBypassSweepRetryDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).deferred);
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    const RootNamespace ns{"test/changed"};
    const ManifestRef ref{1, 1, 1};
    writeManifestRaw(*f.backend, f.store->layout(), ns, ref, {});
    publishCommittedTransition(*f.backend, f.store->layout(), ns, "table", std::nullopt, ref);
    const auto changed = f.run();
    ASSERT_TRUE(changed.acquired_lease);
    EXPECT_FALSE(changed.deferred);
    EXPECT_EQ(f.metric("defer_decision", "changed_shards"), 1);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
}
