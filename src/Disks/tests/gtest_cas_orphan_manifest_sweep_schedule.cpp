#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweepSchedule.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/Exception.h>

#include <limits>

using namespace DB::Cas;

namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsUInt64 orphan_manifest_sweep_interval_sec;
}

TEST(CASOrphanManifestSweepSchedule, DefaultsWaitOneHour)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    PoolConfig pool_config;
    DB::ContentAddressedSettings settings;
    for (const auto interval : {
             pool_config.orphan_manifest_sweep_interval_sec,
             settings[DB::ContentAddressedSetting::orphan_manifest_sweep_interval_sec].value})
    {
        EXPECT_EQ(orphanManifestSweepDecisionOf(state, interval, 1000, 3600999), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
        EXPECT_EQ(orphanManifestSweepDecisionOf(state, interval, 1000, 3601000), OrphanManifestSweepDecision::StartSweep);
    }
}

TEST(CASOrphanManifestSweepSchedule, FirstSweepStartsImmediately)
{
    EXPECT_EQ(orphanManifestSweepDecisionOf(GcState{}, 3600, 1000, 1000), OrphanManifestSweepDecision::StartSweep);
}

TEST(CASOrphanManifestSweepSchedule, UnfinishedTraversalIgnoresInterval)
{
    GcState state;
    state.manifest_sweep_cursor = "cursor";
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 1001), OrphanManifestSweepDecision::ContinueSweep);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 999), OrphanManifestSweepDecision::ContinueSweep);
}

TEST(CASOrphanManifestSweepSchedule, WaitsUntilExactIntervalBoundary)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 3600999), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 3601000), OrphanManifestSweepDecision::StartSweep);
}

TEST(CASOrphanManifestSweepSchedule, ZeroIntervalStartsImmediately)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 1000, 1000), OrphanManifestSweepDecision::StartSweep);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 1000, 999), OrphanManifestSweepDecision::StartSweep);
}

TEST(CASOrphanManifestSweepSchedule, DisabledPreservesState)
{
    GcState state;
    state.manifest_sweep_cursor = "cursor";
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 0, 1000), OrphanManifestSweepDecision::Disabled);
    EXPECT_EQ(state.manifest_sweep_cursor, "cursor");
}

TEST(CASOrphanManifestSweepSchedule, ClockRegressionWaits)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 999), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
}

TEST(CASOrphanManifestSweepSchedule, IntervalComparisonDoesNotOverflow)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1;
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, max, 1000, max), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
    state.orphan_manifest_sweep_completed_at_ms = max - 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 1, 1000, max - 1), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 1, 1000, max), OrphanManifestSweepDecision::StartSweep);
}

TEST(CASOrphanManifestSweepSchedule, CompletionIsRecordedOnlyAtEndOfTraversal)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 500;
    state.manifest_sweep_cursor = "cursor";
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::StartSweep, false, 1000);
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 500);
    EXPECT_EQ(state.manifest_sweep_cursor, "cursor");

    state.manifest_sweep_cursor.clear();
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::ContinueSweep, true, 1001);
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 1001);
}

TEST(CASOrphanManifestSweepSchedule, EmptyPrefixCanCompleteFirstSweep)
{
    GcState state;
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::StartSweep, true, 1000);
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 1000);
}

TEST(CASOrphanManifestSweepSchedule, NonemptyCursorContinuesWithoutActiveFlag)
{
    GcState state;
    state.manifest_sweep_cursor = "cursor";
    state.orphan_manifest_sweep_completed_at_ms = 500;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 999), OrphanManifestSweepDecision::ContinueSweep);
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::ContinueSweep, false, 1000);
    EXPECT_EQ(state.manifest_sweep_cursor, "cursor");
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 500);
}

TEST(CASOrphanManifestSweepSchedule, RejectsUnperformedOrInvalidProgress)
{
    GcState state;
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::SkipUntilIntervalElapsed, true, 1000), DB::Exception);
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::Disabled, true, 1000), DB::Exception);
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::StartSweep, true, 0), DB::Exception);
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::StartSweep, false, 1000), DB::Exception);
    state.manifest_sweep_cursor = "cursor";
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepDecision::StartSweep, true, 1000), DB::Exception);
}
