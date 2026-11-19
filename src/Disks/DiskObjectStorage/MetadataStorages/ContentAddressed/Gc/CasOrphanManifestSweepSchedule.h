#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>

namespace DB::Cas
{

enum class OrphanManifestSweepDecision
{
    Disabled,
    SkipUntilIntervalElapsed,
    StartSweep,
    ContinueSweep,
};

OrphanManifestSweepDecision orphanManifestSweepDecisionOf(
    const GcState & state, uint64_t interval_sec, uint64_t list_budget_keys, uint64_t now_ms);

/// The caller sets the next cursor before validation and commits it with the schedule.
/// A zero clock sample preserves the previous completion time, whose zero value means unknown.
void updateOrphanManifestSweepSchedule(
    GcState & next, OrphanManifestSweepDecision decision, bool wrapped, uint64_t progress_at_ms);

}
