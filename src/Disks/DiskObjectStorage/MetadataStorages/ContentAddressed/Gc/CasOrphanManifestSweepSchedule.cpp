#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweepSchedule.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace DB::Cas
{

OrphanManifestSweepScheduleDecision orphanManifestSweepDecisionOf(
    const GcState & state, uint64_t interval_sec, uint64_t list_budget_keys, uint64_t now_ms)
{
    if (list_budget_keys == 0)
        return OrphanManifestSweepScheduleDecision::Disabled;
    if (state.orphan_manifest_sweep_active)
        return OrphanManifestSweepScheduleDecision::Continue;
    if (interval_sec == 0 || state.orphan_manifest_sweep_completed_at_ms == 0)
        return OrphanManifestSweepScheduleDecision::Start;
    if (now_ms < state.orphan_manifest_sweep_completed_at_ms)
        return OrphanManifestSweepScheduleDecision::Waiting;
    return (now_ms - state.orphan_manifest_sweep_completed_at_ms) / 1000 >= interval_sec
        ? OrphanManifestSweepScheduleDecision::Start
        : OrphanManifestSweepScheduleDecision::Waiting;
}

void updateOrphanManifestSweepSchedule(
    GcState & next, OrphanManifestSweepScheduleDecision decision, bool wrapped, uint64_t progress_at_ms)
{
    if ((decision != OrphanManifestSweepScheduleDecision::Start && decision != OrphanManifestSweepScheduleDecision::Continue)
        || progress_at_ms == 0 || (wrapped && !next.manifest_sweep_cursor.empty()))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS orphan manifest sweep: invalid progress transition");
    if (decision == OrphanManifestSweepScheduleDecision::Start)
        next.orphan_manifest_sweep_started_at_ms = progress_at_ms;
    next.orphan_manifest_sweep_active = !wrapped;
    if (wrapped)
        next.orphan_manifest_sweep_completed_at_ms = progress_at_ms;
}

}
