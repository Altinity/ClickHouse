#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include "cas_test_helpers.h"
#include <base/types.h>
#include <functional>
#include <mutex>
#include <utility>

namespace DB::Cas::tests
{

class SweepReadHookBackend final : public CountingBackend
{
public:
    void arm(const String & key, uint64_t read_number, std::function<void()> callback)
    {
        std::lock_guard lock(hook_mutex);
        hook_key = key;
        remaining_reads = read_number;
        hook = std::move(callback);
        fired = false;
    }

    bool didFire() const
    {
        std::lock_guard lock(hook_mutex);
        return fired;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        std::function<void()> callback;
        {
            std::lock_guard lock(hook_mutex);
            if (hook && key == hook_key && --remaining_reads == 0)
            {
                callback = std::move(hook);
                hook = {};
                fired = true;
            }
        }
        if (callback)
            callback();
        return CountingBackend::read(key, access);
    }

private:
    mutable std::mutex hook_mutex;
    String hook_key;
    uint64_t remaining_reads = 0;
    std::function<void()> hook;
    bool fired = false;
};

/// TEST-ONLY variant of the cursor page: plans a page via the production `planManifestCursorPage` and
/// then exact-token-deletes every nomination immediately, with no source-edge retirement and no
/// `gc/state` adoption of the retirement. Production deletion always goes through `Gc::fold`'s
/// orphan_sweep phase instead, which adopts the retirements in the same round CAS before deleting —
/// this shortcut recreates the accounting hole that path exists to close, so it must never be reached
/// from a production translation unit.
inline ManifestSweepResult sweepManifestCursorPageForTest(
    Pool & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget,
    GcRoundWorkBudget * work_budget = nullptr)
{
    ManifestSweepResult result = planManifestCursorPage(
        store, cursor, list_budget, delete_budget, /*catalog_recovery_authoritative=*/true, work_budget);
    CasOperation op = store.openRequests().admit();
    for (const ManifestSweepResult::Nomination & nomination : result.nominations)
    {
        /// A nomination records the incarnation it was planned against, so the delete re-observes the
        /// key and refuses unless what is there now is still that one: a key a fresh owner has since
        /// replaced must survive.
        const std::optional<Meta> seen = op.head(nomination.key, Retry::standard());
        if (seen && nomination.token.matches(seen->etag)
            && op.remove(nomination.key, seen->etag, Retry::standard()) == Removal::Removed)
            ++result.deleted;
        else
            ++result.skipped;
    }
    return result;
}

}
