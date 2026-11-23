#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweepSchedule.h>
#include <Common/Exception.h>
#include <magic_enum.hpp>

namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

namespace DB::Cas
{

OrphanManifestSweepDecision orphanManifestSweepDecisionOf(
    const GcState & state, uint64_t interval_sec, uint64_t list_budget_keys, uint64_t now_ms)
{
    if (list_budget_keys == 0)
        return OrphanManifestSweepDecision::Disabled;
    if (!state.manifest_sweep_cursor.empty())
        return OrphanManifestSweepDecision::ContinueSweep;
    if (interval_sec == 0 || state.orphan_manifest_sweep_completed_at_ms == 0)
        return OrphanManifestSweepDecision::StartSweep;
    if (now_ms < state.orphan_manifest_sweep_completed_at_ms)
        return OrphanManifestSweepDecision::SkipUntilIntervalElapsed;
    return (now_ms - state.orphan_manifest_sweep_completed_at_ms) / 1000 >= interval_sec
        ? OrphanManifestSweepDecision::StartSweep
        : OrphanManifestSweepDecision::SkipUntilIntervalElapsed;
}

void updateOrphanManifestSweepSchedule(
    GcState & next, OrphanManifestSweepDecision decision, bool wrapped, uint64_t progress_at_ms)
{
    if ((decision != OrphanManifestSweepDecision::StartSweep && decision != OrphanManifestSweepDecision::ContinueSweep)
        || (wrapped != next.manifest_sweep_cursor.empty()))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Invalid CAS orphan manifest sweep progress: decision={}, wrapped={}, cursor_empty={}, progress_at_ms={}",
            magic_enum::enum_name(decision), wrapped, next.manifest_sweep_cursor.empty(), progress_at_ms);
    if (wrapped && progress_at_ms != 0)
        next.orphan_manifest_sweep_completed_at_ms = progress_at_ms;
}

}
