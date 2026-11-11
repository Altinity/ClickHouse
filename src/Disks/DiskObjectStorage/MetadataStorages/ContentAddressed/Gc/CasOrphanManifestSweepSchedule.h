#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>

namespace DB::Cas
{

enum class OrphanManifestSweepScheduleDecision
{
    Disabled,
    Waiting,
    Start,
    Continue,
};

OrphanManifestSweepScheduleDecision orphanManifestSweepDecisionOf(
    const GcState & state, uint64_t interval_sec, uint64_t list_budget_keys, uint64_t now_ms);

void updateOrphanManifestSweepSchedule(
    GcState & next, OrphanManifestSweepScheduleDecision decision, bool wrapped, uint64_t progress_at_ms);

}
