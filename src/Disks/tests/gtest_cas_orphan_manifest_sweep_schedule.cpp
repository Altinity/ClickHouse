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
        EXPECT_EQ(orphanManifestSweepDecisionOf(state, interval, 1000, 3600999), OrphanManifestSweepScheduleDecision::Waiting);
        EXPECT_EQ(orphanManifestSweepDecisionOf(state, interval, 1000, 3601000), OrphanManifestSweepScheduleDecision::Start);
    }
}

TEST(CASOrphanManifestSweepSchedule, FirstSweepStartsImmediately)
{
    EXPECT_EQ(orphanManifestSweepDecisionOf(GcState{}, 3600, 1000, 1000), OrphanManifestSweepScheduleDecision::Start);
}

TEST(CASOrphanManifestSweepSchedule, ActiveSweepIgnoresInterval)
{
    GcState state;
    state.orphan_manifest_sweep_active = true;
    state.manifest_sweep_cursor = "cursor";
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 1001), OrphanManifestSweepScheduleDecision::Continue);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 999), OrphanManifestSweepScheduleDecision::Continue);
}

TEST(CASOrphanManifestSweepSchedule, WaitsUntilExactIntervalBoundary)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 3600999), OrphanManifestSweepScheduleDecision::Waiting);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 3601000), OrphanManifestSweepScheduleDecision::Start);
}

TEST(CASOrphanManifestSweepSchedule, ZeroIntervalStartsImmediately)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 1000, 1000), OrphanManifestSweepScheduleDecision::Start);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 1000, 999), OrphanManifestSweepScheduleDecision::Start);
}

TEST(CASOrphanManifestSweepSchedule, DisabledPreservesState)
{
    GcState state;
    state.orphan_manifest_sweep_active = true;
    state.manifest_sweep_cursor = "cursor";
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 0, 1000), OrphanManifestSweepScheduleDecision::Disabled);
    EXPECT_TRUE(state.orphan_manifest_sweep_active);
    EXPECT_EQ(state.manifest_sweep_cursor, "cursor");
}

TEST(CASOrphanManifestSweepSchedule, ClockRegressionWaits)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 999), OrphanManifestSweepScheduleDecision::Waiting);
}

TEST(CASOrphanManifestSweepSchedule, IntervalComparisonDoesNotOverflow)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1;
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, max, 1000, max), OrphanManifestSweepScheduleDecision::Waiting);
    state.orphan_manifest_sweep_completed_at_ms = max - 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 1, 1000, max - 1), OrphanManifestSweepScheduleDecision::Waiting);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 1, 1000, max), OrphanManifestSweepScheduleDecision::Start);
}

TEST(CASOrphanManifestSweepSchedule, StartAndCompletionAreRecordedWithProgress)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 500;
    state.manifest_sweep_cursor = "cursor";
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Start, false, 1000);
    EXPECT_TRUE(state.orphan_manifest_sweep_active);
    EXPECT_EQ(state.orphan_manifest_sweep_started_at_ms, 1000);
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 500);
    EXPECT_EQ(state.manifest_sweep_cursor, "cursor");

    state.manifest_sweep_cursor.clear();
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Continue, true, 1001);
    EXPECT_FALSE(state.orphan_manifest_sweep_active);
    EXPECT_EQ(state.orphan_manifest_sweep_started_at_ms, 1000);
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 1001);
}

TEST(CASOrphanManifestSweepSchedule, EmptyPrefixCanCompleteFirstSweep)
{
    GcState state;
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Start, true, 1000);
    EXPECT_FALSE(state.orphan_manifest_sweep_active);
    EXPECT_EQ(state.orphan_manifest_sweep_started_at_ms, 1000);
    EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, 1000);
}

TEST(CASOrphanManifestSweepSchedule, ContinueKeepsUnknownLegacyStart)
{
    GcState state;
    state.orphan_manifest_sweep_active = true;
    state.manifest_sweep_cursor = "cursor";
    updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Continue, false, 1000);
    EXPECT_TRUE(state.orphan_manifest_sweep_active);
    EXPECT_EQ(state.orphan_manifest_sweep_started_at_ms, 0);
}

TEST(CASOrphanManifestSweepSchedule, RejectsUnperformedOrInvalidProgress)
{
    GcState state;
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Waiting, true, 1000), DB::Exception);
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Disabled, true, 1000), DB::Exception);
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Start, true, 0), DB::Exception);
    state.manifest_sweep_cursor = "cursor";
    EXPECT_THROW(updateOrphanManifestSweepSchedule(state, OrphanManifestSweepScheduleDecision::Start, true, 1000), DB::Exception);
}
