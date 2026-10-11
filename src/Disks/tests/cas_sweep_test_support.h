#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <base/types.h>

#include <map>
#include <set>

namespace DB::Cas::tests
{

/// The two LISTs a GC round has already taken when it plans its sweep page -- phase 3's
/// `gc/server-roots/` and phase 4's `cas/ns/stream/` -- taken here for a test that calls the page
/// directly, in the shapes `planManifestCursorPage` takes them.
struct RoundListingsForTest
{
    std::set<String> server_roots;
    std::map<NamespaceLifePhysicalId, std::set<RefTxnId>> ref_logs;
};

inline RoundListingsForTest listRoundForTest(Pool & store)
{
    const Layout & layout = store.layout();
    RoundListingsForTest listed{.server_roots = listServerRoots(store)};
    CasOperation op = store.openRequests().admit();
    op.forEachListedKey(layout.casRefsPrefix(), [&](const ListedKey & k)
    {
        if (const auto parsed = layout.parseRefObjectKey(k.key); parsed && parsed->kind == RefObjectKind::Log)
            listed.ref_logs[parsed->life_id].insert(parsed->txn_id);
        return true;
    }, Retry::standard());
    return listed;
}

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
    const RoundListingsForTest listed = listRoundForTest(store);
    ManifestSweepResult result = planManifestCursorPage(
        store, cursor, list_budget, delete_budget, /*catalog_recovery_authoritative=*/true,
        listed.server_roots, listed.ref_logs, work_budget);
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
