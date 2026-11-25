#include <gtest/gtest.h>
#include <Disks/tests/cas_test_helpers.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweepSchedule.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/Exception.h>
#include <fmt/format.h>

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

TEST(CASOrphanManifestSweepSchedule, UnfinishedTraversalPrecedesImmediateStart)
{
    GcState state;
    state.manifest_sweep_cursor = "cursor";
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 0, 1000, 1000), OrphanManifestSweepDecision::ContinueSweep);
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

TEST(CASOrphanManifestSweepSchedule, DisabledPrecedesElapsedInterval)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 1000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 0, 3601000), OrphanManifestSweepDecision::Disabled);
}

TEST(CASOrphanManifestSweepSchedule, FutureCompletionStartsOnlyBeyondInterval)
{
    GcState state;
    state.orphan_manifest_sweep_completed_at_ms = 3601000;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 1001), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 1000), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 3600, 1000, 999), OrphanManifestSweepDecision::StartSweep);
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
    state.orphan_manifest_sweep_completed_at_ms = max;
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, max, 1000, 1), OrphanManifestSweepDecision::SkipUntilIntervalElapsed);
    EXPECT_EQ(orphanManifestSweepDecisionOf(state, 1, 1000, max - 1001), OrphanManifestSweepDecision::StartSweep);
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
    const auto expect_rejected = [&](OrphanManifestSweepDecision decision, bool wrapped, std::string_view expected_fields)
    {
        for (const uint64_t progress_at_ms : {0, 1000})
        {
            CAS_EXPECT_DEATH(
                {
                    DB::abort_on_logical_error.store(true, std::memory_order_relaxed);
                    updateOrphanManifestSweepSchedule(state, decision, wrapped, progress_at_ms);
                },
                fmt::format("{}, progress_at_ms={}", expected_fields, progress_at_ms));
        }
    };
    expect_rejected(OrphanManifestSweepDecision::SkipUntilIntervalElapsed, true, "decision=SkipUntilIntervalElapsed, wrapped=true, cursor_empty=true");
    expect_rejected(OrphanManifestSweepDecision::Disabled, true, "decision=Disabled, wrapped=true, cursor_empty=true");
    expect_rejected(OrphanManifestSweepDecision::StartSweep, false, "decision=StartSweep, wrapped=false, cursor_empty=true");
    state.manifest_sweep_cursor = "cursor";
    expect_rejected(OrphanManifestSweepDecision::StartSweep, true, "decision=StartSweep, wrapped=true, cursor_empty=false");
}

TEST(CASOrphanManifestSweepSchedule, ZeroClockPreservesCompletion)
{
    for (const uint64_t previous_completion : {0, 500})
    {
        SCOPED_TRACE(previous_completion);
        for (const bool wrapped : {false, true})
        {
            SCOPED_TRACE(wrapped);
            for (const auto decision : {OrphanManifestSweepDecision::StartSweep, OrphanManifestSweepDecision::ContinueSweep})
            {
                SCOPED_TRACE(static_cast<int>(decision));
                GcState state;
                state.orphan_manifest_sweep_completed_at_ms = previous_completion;
                state.manifest_sweep_cursor = wrapped ? "" : "cursor";
                ASSERT_NO_THROW(updateOrphanManifestSweepSchedule(state, decision, wrapped, 0));
                EXPECT_EQ(state.orphan_manifest_sweep_completed_at_ms, previous_completion);
                EXPECT_EQ(state.manifest_sweep_cursor, wrapped ? "" : "cursor");
            }
        }
    }
}
