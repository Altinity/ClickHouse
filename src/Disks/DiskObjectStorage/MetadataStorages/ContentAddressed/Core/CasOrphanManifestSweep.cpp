#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackendListing.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcCursorKey.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/logger_useful.h>
#include <algorithm>
#include <map>
#include <set>
#include <limits>

namespace ProfileEvents
{
    extern const Event CasGcEnumerationPages;
}

namespace DB::Cas
{

namespace
{

/// The build-watermark floor now rides the per-server mount lease (ack-floor merge, spec 2026-07-02).
/// A namespace is rooted by `server_root_id`, but that id is a clean relative path and can contain
/// slashes. Try namespace prefixes from longest to shortest and accept the first durable mount body.
/// No mount => no authority => fail open / not eligible. On the writable path the mount's
/// `writer_epoch` is the same durable value the old watermark's `epoch` carried (CasStore.cpp "THE
/// BRIDGE"), so `{writer_epoch, min_active}` are consumed exactly where `ServerWatermark::{epoch,
/// min_active}` were.
std::optional<MountLease> floorForNamespace(Store & store, const RootNamespace & ns)
{
    const String & value = ns.string();
    size_t pos = value.size();
    while (true)
    {
        pos = value.rfind('/', pos == 0 ? 0 : pos - 1);
        if (pos == String::npos)
            break;

        const String server_root_id = value.substr(0, pos);
        if (!server_root_id.empty())
        {
            if (const auto got = store.backend().get(store.layout().mountKey(server_root_id)))
                return decodeMountLease(got->bytes);
        }
        if (pos == 0)
            break;
    }
    return std::nullopt;
}

struct ListedManifestObject
{
    RootNamespace ns;
    BuildPrefix prefix;
    String key;
};

/// Delegates to the one shared `Layout::parseManifestKey` (spec §Manifest Identifier canonical hex
/// form) instead of hand-rolling a second parser -- see also `CasFsck.cpp`'s `parseBuildPrefix`, which
/// now routes through the same function.
std::optional<ListedManifestObject> parseListedManifestObject(const Layout & layout, const String & key)
{
    const auto parsed = layout.parseManifestKey(key);
    if (!parsed)
        return std::nullopt;

    return ListedManifestObject{
        .ns = parsed->root_namespace,
        .prefix = BuildPrefix{.writer_epoch = parsed->ref.writer_epoch, .build_sequence = parsed->ref.build_sequence},
        .key = key};
}

/// The table's durable `last_folded_ref_id` (spec §Orphan Manifest Protection), read from the adopted
/// fold seal at `(snap_generation, snap_attempt)`; {0,0} when no seal covers it yet (fresh pool). A
/// manifest removed by a log ABOVE this cursor has NOT had its `-1` decrement folded, so its body is still
/// load-bearing (delete-after-sealed-decrements).
RefTxnId sealedRefCursor(Store & store, const RootNamespace & ns)
{
    const Layout & layout = store.layout();
    const auto state_got = store.backend().get(layout.gcStateKey());
    if (!state_got)
        return RefTxnId{};
    const GcState state = decodeGcState(state_got->bytes);
    if (const auto got = store.backend().get(layout.foldSealKey(state.snap_generation, state.snap_attempt)))
    {
        const CasFoldSeal seal = decodeFoldSeal(got->bytes);
        const auto it = seal.per_ns_shard.find(cursorKey(ns, /*shard*/0));
        if (it != seal.per_ns_shard.end())
            return it->second.last_folded_ref_id;
    }
    return RefTxnId{};
}

/// The active manifest-object-KEY set for one namespace (spec §Orphan Manifest Protection), built as the
/// same complete view writer recovery uses:
///   owners in the newest snapshot + owner changes in every later log (== `recoverRefTable`'s committed
///   rows and live precommits) + manifests removed anywhere in the tail above the durable
///   `last_folded_ref_id` (their `-1` is not yet folded, so the GC fold still needs the body).
/// Keys (not ManifestIds) so a listed object key can be tested directly. Throws on a corrupt snapshot /
/// invalid transaction (via `recoverRefTable` / `decodeRefLogTxn`); the caller SKIPS the namespace's
/// deletions on such a throw rather than substituting an empty owner set.
std::set<String> activeManifestKeys(Store & store, const RootNamespace & ns)
{
    std::set<String> active;
    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    /// Current owners = snapshot + replayed tail (committed rows + live precommits).
    const RefTableState state = recoverRefTable(backend, layout, ns);
    for (const auto & [ref_name, row] : state.committed)
        active.insert(layout.manifestKey(ManifestId{ns, row.manifest_ref}));
    for (const auto & [ref_name, manifest_ref] : state.precommits)
        active.insert(layout.manifestKey(ManifestId{ns, manifest_ref}));

    /// Tail-removal protection: every manifest removed by a log ABOVE the durable fold cursor stays active
    /// until its `-1` folds. LIST the table's logs, decode those above the cursor, and protect each
    /// removed manifest (a `-1` edge). A namespace-removal transaction names every removed owner explicitly,
    /// so this also protects a whole removed namespace's bodies until the fold catches up.
    const RefTxnId cursor = sealedRefCursor(store, ns);
    std::vector<RefTxnId> logs;
    forEachListedKey(backend, layout.refsNamespacePrefix(ns), [&](const ListedKey & lk)
    {
        if (const auto parsed = layout.parseRefObjectKey(lk.key);
            parsed && parsed->ns == ns && parsed->kind == RefObjectKind::Log)
            logs.push_back(parsed->txn_id);
    });
    std::sort(logs.begin(), logs.end());
    for (const RefTxnId & id : logs)
    {
        if (!(cursor < id))
            continue;   /// id <= cursor: its edges are already folded
        const auto got = backend.get(layout.refLogKey(ns, id));
        if (!got)
            continue;   /// vanished (a concurrent cleanup published a covering snapshot) -- its -1 was folded
        const RefLogTxn txn = decodeRefLogTxn(got->bytes, ns.string(), id);
        for (const RefManifestEdge & edge : manifestEdgesOfTxn(txn))
            if (edge.change < 0)
                active.insert(layout.manifestKey(edge.manifest_id));
    }
    return active;
}

}

bool prefixEligible(Store & store, const RootNamespace & ns, const BuildPrefix & prefix)
{
    /// OQ6: durable watermark fact only. A missing watermark => NOT eligible (control #9: never a
    /// frozen-seq / judged-dead guess). Compare writer_epoch first, then build_sequence, so old-epoch
    /// debris drains after a process restart even when its build_sequence is above the current min_active.
    const auto floor = floorForNamespace(store, ns);
    if (!floor)
        return false;

    const MountLease & w = *floor;
    if (prefix.writer_epoch < w.writer_epoch)
        return true;
    if (prefix.writer_epoch > w.writer_epoch)
        return false;
    if (w.min_active == std::numeric_limits<uint64_t>::max())
        return true;   /// farewell/retired sentinel: every seq is retired
    return w.min_active > prefix.build_sequence;
}

void sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix)
{
    if (!prefixEligible(store, ns, prefix))
        return;   /// not eligible by the durable watermark fact — delete nothing (controls #8/#9)

    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    /// Build the protection view. A corrupt snapshot / invalid transaction throws (spec §Orphan Manifest
    /// Protection: "a missing snapshot body, invalid transaction, or incomplete ordered view causes the
    /// sweep to skip deletion and surface the error. It never substitutes an empty owner set."). Skip this
    /// namespace's deletions on such a throw rather than deleting against a wrong (empty) view.
    std::set<String> active;
    try
    {
        active = activeManifestKeys(store, ns);
    }
    catch (const Exception & e)
    {
        LOG_WARNING(getLogger("CasOrphanManifestSweep"),
                    "CAS orphan sweep: namespace {} protection view unavailable ({}); skipping its deletions",
                    ns.string(), e.message());
        return;
    }

    /// Enumerate the ONE build prefix: cas/manifests/<ns>/<epoch-hex>-<seq-hex>/ (spec §Manifest
    /// Identifier canonical hex form -- same rendering `Layout::manifestKey` uses).
    const String prefix_key = layout.manifestNamespacePrefix(ns)
        + renderRefTxnId(RefTxnId{prefix.writer_epoch, prefix.build_sequence}) + "/";

    forEachListedKey(backend, prefix_key, [&](const ListedKey & listed)
    {
        if (active.count(listed.key))
            return;   /// owned by a committed/precommit owner — never sweep (control #8)

        /// Exact-token delete: HEAD for the current token, then deleteExact. A 404 between HEAD and
        /// delete (or a TokenMismatch — a fresh owner reclaimed it) is tolerated (record-and-continue).
        const HeadResult head = backend.head(listed.key);
        if (!head.exists)
            return;
        backend.deleteExact(listed.key, head.token);   /// NotFound/TokenMismatch spared
    });
}

ManifestSweepResult sweepManifestCursorPage(
    Store & store,
    const String & cursor,
    uint64_t list_budget,
    uint64_t delete_budget)
{
    ManifestSweepResult result;
    result.next_cursor = cursor;
    if (list_budget == 0)
        return result;

    Backend & backend = store.backend();
    const Layout & layout = store.layout();
    const ListPage page = backend.list(layout.casManifestsPrefix(), cursor, list_budget);
    /// §0 introspection: this pass fetches exactly one page per round (the cursor advances ACROSS
    /// rounds, not within this call) — one increment per call, not per listed key.
    ProfileEvents::increment(ProfileEvents::CasGcEnumerationPages);

    std::map<String, bool> eligible_by_prefix;
    std::map<String, std::set<String>> active_by_ns;
    std::set<String> errored_namespaces;   /// protection view unavailable => skip, never delete
    for (const ListedKey & listed : page.keys)
    {
        ++result.listed;
        const auto parsed = parseListedManifestObject(layout, listed.key);
        if (!parsed)
        {
            ++result.skipped;
            continue;
        }

        if (result.deleted >= delete_budget)
        {
            ++result.skipped;
            continue;
        }

        const String eligibility_key = parsed->ns.string() + "\n"
            + std::to_string(parsed->prefix.writer_epoch) + "\n"
            + std::to_string(parsed->prefix.build_sequence);
        auto [eligible_it, eligible_inserted] = eligible_by_prefix.emplace(eligibility_key, false);
        if (eligible_inserted)
            eligible_it->second = prefixEligible(store, parsed->ns, parsed->prefix);
        if (!eligible_it->second)
        {
            ++result.skipped;
            continue;
        }

        auto [active_it, inserted] = active_by_ns.emplace(parsed->ns.string(), std::set<String>{});
        if (inserted)
        {
            /// A corrupt snapshot / invalid transaction throws (spec §Orphan Manifest Protection): skip the
            /// namespace's deletions and surface the error, never substitute an empty owner set.
            try
            {
                active_it->second = activeManifestKeys(store, parsed->ns);
            }
            catch (const Exception & e)
            {
                LOG_WARNING(getLogger("CasOrphanManifestSweep"),
                            "CAS orphan sweep: namespace {} protection view unavailable ({}); skipping",
                            parsed->ns.string(), e.message());
                errored_namespaces.insert(parsed->ns.string());
            }
        }
        if (errored_namespaces.count(parsed->ns.string()))
        {
            ++result.skipped;
            continue;
        }
        if (active_it->second.count(parsed->key))
        {
            ++result.skipped;
            continue;
        }

        Token token;
        if (listed.token)
            token = *listed.token;
        else
        {
            const HeadResult head = backend.head(parsed->key);
            if (!head.exists)
            {
                ++result.skipped;
                continue;
            }
            token = head.token;
        }

        const DeleteOutcome outcome = backend.deleteExact(parsed->key, token);
        const DeleteClass outcome_class = classifyDeleteOutcome(outcome);
        if (outcome_class == DeleteClass::Deleted)
            ++result.deleted;
        else
            ++result.skipped;

        /// INTROSPECTION-3 (2026-07-10): EVERY manifest-body deletion must leave an audit row. The sweep
        /// silently deleting bodies was the blocker in diagnosing the GC-WEDGE (a live committed body
        /// vanished with no trace). object_hash is the full raw key (namespace-qualified — no cross-ns
        /// manifest-ref-string collision, the other diagnosis pitfall).
        EventEmitter{store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::ManifestDelete;
            e.namespace_ = parsed->ns.string();
            e.object_kind = CasEventObjectKind::Manifest;
            e.object_hash = parsed->key;
            e.outcome = String{deleteClassName(outcome_class)};
            e.reason = "orphan-manifest sweep: exact-token delete of an eligible+unowned build-prefix body";
            e.detail = {{"writer_epoch", std::to_string(parsed->prefix.writer_epoch)},
                        {"build_sequence", std::to_string(parsed->prefix.build_sequence)}};
        });
    }

    result.next_cursor = page.next_cursor;
    result.wrapped = page.next_cursor.empty();
    return result;
}

}
