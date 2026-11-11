#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include "cas_test_helpers.h"

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
    std::function<void()> before_manifest_list;
    std::function<void()> before_round_commit;

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        if (prefix == manifests_prefix && before_manifest_list)
            std::exchange(before_manifest_list, {})();
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
    EXPECT_EQ(actual.orphan_manifest_sweep_active, expected.orphan_manifest_sweep_active);
    EXPECT_EQ(actual.orphan_manifest_sweep_started_at_ms, expected.orphan_manifest_sweep_started_at_ms);
    EXPECT_EQ(actual.orphan_manifest_sweep_completed_at_ms, expected.orphan_manifest_sweep_completed_at_ms);
}

}

TEST(CASOrphanManifestSweepScheduleGc, DueForcesIdleFold)
{
    ScheduleFixture f(1000, 3600, 1000);
    EXPECT_FALSE(f.run().deferred);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);
}

TEST(CASOrphanManifestSweepScheduleGc, ContinueForcesIdleFold)
{
    ScheduleFixture f(1, 3600, 1000);
    f.addKeys();
    auto state = f.state();
    state.orphan_manifest_sweep_active = true;
    f.setState(state);
    const auto report = f.run();
    ASSERT_TRUE(report.acquired_lease);
    EXPECT_FALSE(report.deferred);
    EXPECT_TRUE(f.state().orphan_manifest_sweep_active);
}

TEST(CASOrphanManifestSweepScheduleGc, WaitingPreservesOldDeferDecision)
{
    ScheduleFixture f(1000, 3600, 1000);
    auto state = f.state();
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    f.setState(state);
    EXPECT_TRUE(f.run().deferred);
    expectSameSchedule(f.state(), state);
}

TEST(CASOrphanManifestSweepScheduleGc, DisabledDoesNotForceFold)
{
    ScheduleFixture f(0, 3600, 1000);
    EXPECT_TRUE(f.run().deferred);
}

TEST(CASOrphanManifestSweepScheduleGc, TwoPagesDoNotWaitAnHour)
{
    ScheduleFixture f(1);
    f.addKeys();
    ASSERT_TRUE(f.run().acquired_lease);
    const auto first = f.state();
    EXPECT_TRUE(first.orphan_manifest_sweep_active);
    EXPECT_EQ(first.orphan_manifest_sweep_completed_at_ms, 0);
    EXPECT_FALSE(first.manifest_sweep_cursor.empty());
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    const auto second = f.state();
    EXPECT_FALSE(second.orphan_manifest_sweep_active);
    EXPECT_TRUE(second.manifest_sweep_cursor.empty());
    EXPECT_EQ(second.orphan_manifest_sweep_started_at_ms, 1000);
    EXPECT_EQ(second.orphan_manifest_sweep_completed_at_ms, 1001);
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
    state.orphan_manifest_sweep_active = true;
    state.manifest_sweep_cursor = "cursor";
    state.orphan_manifest_sweep_started_at_ms = 500;
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
    ASSERT_TRUE(before.orphan_manifest_sweep_active);
    ++f.now;
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    expectSameSchedule(f.state(), before);
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
    const RootNamespace ns{"test/orphan"};
    const ManifestRef ref{1, 1, 1};
    writeManifestRaw(*f.backend, f.store->layout(), ns, ref, {});
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
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 0);
    OperationForTest op(*f.backend);
    EXPECT_TRUE((*op).read(f.store->layout().manifestKey(ManifestId{ns, ref}), Retry::once()).has_value());
}

TEST(CASOrphanManifestSweepScheduleGc, TakeoverResumesCursor)
{
    ScheduleFixture f(1);
    f.addKeys();
    ASSERT_TRUE(f.run().acquired_lease);
    const auto first = f.state();
    f.makeGc(UInt128(0xAC));
    EXPECT_FALSE(f.run().acquired_lease);
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_FALSE(f.state().orphan_manifest_sweep_active);
    EXPECT_EQ(f.state().orphan_manifest_sweep_started_at_ms, first.orphan_manifest_sweep_started_at_ms);
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
    f.gc.reset();
    f.store.reset();
    f.store = Pool::open(f.backend, f.config);
    f.makeGc();
    ++f.now;
    ASSERT_TRUE(f.run().acquired_lease);
    EXPECT_FALSE(f.state().orphan_manifest_sweep_active);
    EXPECT_EQ(f.state().orphan_manifest_sweep_started_at_ms, before.orphan_manifest_sweep_started_at_ms);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, RebuildMakesNextSweepDue)
{
    ScheduleFixture f;
    ASSERT_TRUE(f.run().acquired_lease);
    ASSERT_NE(f.state().orphan_manifest_sweep_completed_at_ms, 0);
    ASSERT_TRUE(f.gc->rebuildBaseline(true).performed);
    const auto state = f.state();
    EXPECT_FALSE(state.orphan_manifest_sweep_active);
    EXPECT_TRUE(state.manifest_sweep_cursor.empty());
    EXPECT_EQ(state.orphan_manifest_sweep_started_at_ms, 0);
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
    ASSERT_TRUE(f.run().acquired_lease);
    ASSERT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 1000);

    OperationForTest op(*f.backend);
    const String checkpoint_key = layout.refCkptKey(fixture::fixtureLife(ns));
    const auto checkpoint = (*op).read(checkpoint_key, Retry::once());
    ASSERT_TRUE(checkpoint.has_value());
    orThrow((*op).replace(checkpoint_key, "invalid checkpoint", checkpoint->etag, Retry::once()), "test corrupt checkpoint");
    f.now = 3601000;
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

TEST(CASOrphanManifestSweepScheduleGc, SuccessfulPageRestoresImmediateContinuation)
{
    ScheduleFixture f(1, 0, 2);
    f.addKeys();
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    for (size_t tick = 0; tick < 2; ++tick)
        ASSERT_TRUE(f.run().deferred);
    const auto first = f.run();
    ASSERT_TRUE(first.acquired_lease);
    ASSERT_FALSE(first.deferred);
    ASSERT_TRUE(f.state().orphan_manifest_sweep_active);
    const auto continued = f.run();
    ASSERT_TRUE(continued.acquired_lease);
    EXPECT_FALSE(continued.deferred);
    EXPECT_FALSE(f.state().orphan_manifest_sweep_active);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, ChangedRefsBypassSweepRetryDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
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

TEST(CASOrphanManifestSweepScheduleGc, RestartRetriesSuppressedSweepImmediately)
{
    ScheduleFixture f(1000, 3600, 2);
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    f.makeGc();
    const auto restarted = f.run();
    ASSERT_TRUE(restarted.acquired_lease);
    EXPECT_FALSE(restarted.deferred);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
}

TEST(CASOrphanManifestSweepScheduleGc, NewGenerationRetriesSuppressedSweepImmediately)
{
    ScheduleFixture f(1, 3600, 2);
    f.addKeys();
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    const auto suppressed = f.state();
    Gc other(f.store, UInt128(0xAC), [&] { return f.now; });
    ASSERT_FALSE(other.runRegularRound({}, true).acquired_lease);
    ++f.now;
    ASSERT_TRUE(other.runRegularRound({}, true).acquired_lease);
    ASSERT_GT(f.state().snap_generation, suppressed.snap_generation);
    ASSERT_TRUE(f.state().orphan_manifest_sweep_active);
    ++f.now;
    ASSERT_FALSE(f.run().acquired_lease);
    ++f.now;
    const auto resumed = f.run();
    ASSERT_TRUE(resumed.acquired_lease);
    EXPECT_FALSE(resumed.deferred);
    EXPECT_FALSE(f.state().orphan_manifest_sweep_active);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 2);
}

TEST(CASOrphanManifestSweepScheduleGc, RebuildRetriesSuppressedSweepImmediately)
{
    ScheduleFixture f(1000, 3600, 2);
    ASSERT_TRUE(f.run(UniversePolicy::StageA_Suppressed).acquired_lease);
    const auto suppressed = f.state();
    ASSERT_TRUE(f.gc->rebuildBaseline(true).performed);
    ASSERT_GT(f.state().snap_generation, suppressed.snap_generation);
    const auto rebuilt = f.run();
    ASSERT_TRUE(rebuilt.acquired_lease);
    EXPECT_FALSE(rebuilt.deferred);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 1);
}

TEST(CASOrphanManifestSweepScheduleGc, LostCasDoesNotArmSweepRetryDeferral)
{
    ScheduleFixture f(1000, 3600, 2);
    bool fired = false;
    f.backend->before_round_commit = [&]
    {
        fired = true;
        auto state = f.state();
        ++state.lease.seq;
        f.setState(state);
    };
    EXPECT_THROW(f.run(UniversePolicy::StageA_Suppressed), DB::Exception);
    ASSERT_TRUE(fired);
    EXPECT_EQ(f.state().snap_generation, 0);
    const auto retried = f.run(UniversePolicy::StageA_Suppressed);
    ASSERT_TRUE(retried.acquired_lease);
    EXPECT_FALSE(retried.deferred);
    EXPECT_EQ(f.backend->listCount(f.backend->manifests_prefix), 0);
    EXPECT_EQ(f.state().orphan_manifest_sweep_completed_at_ms, 0);
}
