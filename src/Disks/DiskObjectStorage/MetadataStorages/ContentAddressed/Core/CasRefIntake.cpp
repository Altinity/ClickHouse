#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

std::vector<RefManifestEdge> manifestEdgesOfTxn(const RefLogTxn & txn)
{
    std::vector<RefManifestEdge> edges;
    const RootNamespace ns{txn.ns};

    for (uint32_t op_ordinal = 0; op_ordinal < txn.ops.size(); ++op_ordinal)
    {
        const RefOp & op = txn.ops[op_ordinal];
        if (op.kind != RefOpKind::OwnerTransition)
            continue;

        const bool has_old = op.old_binding.has_value();
        const bool has_new = op.new_binding.has_value();

        /// Promote / same-manifest owner move: the owner changes kind but the manifest keeps an owner
        /// the whole time, so there is no net edge (spec §Promote).
        if (has_old && has_new && op.old_binding->manifest_ref == op.new_binding->manifest_ref)
            continue;

        if (has_old)
            edges.push_back(RefManifestEdge{
                ManifestId{ns, op.old_binding->manifest_ref}, -1, op.old_binding->kind, op_ordinal, 0});
        if (has_new)
            edges.push_back(RefManifestEdge{
                ManifestId{ns, op.new_binding->manifest_ref}, +1, op.new_binding->kind, op_ordinal, 1});
    }

    return edges;
}

std::optional<RefTxnId> removalTxnId(const RefLogTxn & txn)
{
    for (const RefOp & op : txn.ops)
        if (op.kind == RefOpKind::RemoveNamespace)
            return txn.txn_id;
    return std::nullopt;
}

std::map<String, RefTableListing> groupRefKeys(const Layout & layout, const std::vector<String> & listed_keys)
{
    const String base = layout.casRefsPrefix();
    std::map<String, RefTableListing> out;

    for (const String & key : listed_keys)
    {
        if (!key.starts_with(base))
            continue;

        const auto parsed = layout.parseRefObjectKey(key);
        if (!parsed)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "groupRefKeys: key '{}' under the ref prefix is not a valid ref object -- aborting ref folding", key);

        /// `parseRefObjectKey` reconstructs the namespace from the key WITHOUT checking
        /// its shape. This is the first production consumer of that namespace, so re-validate it; a
        /// malformed namespace throws (BAD_ARGUMENTS), which the round treats as a malformed key.
        layout.validateNamespace(parsed->ns);

        RefTableListing & table = out[parsed->ns.string()];
        switch (parsed->kind)
        {
            case RefObjectKind::Log:
                table.logs.push_back(parsed->txn_id);
                break;
            case RefObjectKind::Snap:
                table.snapshots.push_back(parsed->txn_id);
                break;
            case RefObjectKind::Cleanup:
                table.cleanup_markers.push_back(parsed->txn_id);
                break;
        }
    }

    for (auto & [ns, table] : out)
    {
        std::sort(table.logs.begin(), table.logs.end());
        std::sort(table.snapshots.begin(), table.snapshots.end());
        std::sort(table.cleanup_markers.begin(), table.cleanup_markers.end());
    }

    return out;
}

RefCleanupPlan planRefCleanup(const RefTableListing & listing, const RefTxnId & durable_cursor,
                              const std::set<RefTxnId> & removal_logs_blocked,
                              std::optional<RefTxnId> completed_removal_snapshot)
{
    RefCleanupPlan plan;

    /// The coverage boundary `X` is the newest snapshot known to be durable: the newest observed in this
    /// round's scan, and -- for a namespace-cleanup item that reached `Completed` this round -- the
    /// `Removed` snapshot the caller just made durable (spec §Namespace Removal republication path). With
    /// neither there is no boundary, so no log is coverage-deletable and no older snapshot exists to delete
    /// (spec §Step 6, condition 2).
    std::optional<RefTxnId> newest_snapshot;
    if (!listing.snapshots.empty())
        newest_snapshot = listing.snapshots.back();
    if (completed_removal_snapshot && (!newest_snapshot || *newest_snapshot < *completed_removal_snapshot))
        newest_snapshot = completed_removal_snapshot;
    if (!newest_snapshot)
        return plan;

    for (const RefTxnId & log_id : listing.logs)
    {
        if (*newest_snapshot < log_id)     /// L > X: not covered by any durable snapshot
            continue;
        if (durable_cursor < log_id)       /// L > cursor: its edge delta is not yet durable
            continue;
        if (removal_logs_blocked.contains(log_id))   /// remove_namespace log whose item is not Completed
            continue;
        plan.deletable_logs.push_back(log_id);
    }

    /// Only snapshots the scan actually returned are deletion candidates; a `completed_removal_snapshot`
    /// first published this round is not in `listing.snapshots`, so it is never scheduled for deletion,
    /// and once it later appears in the scan it is the newest and is retained here.
    for (const RefTxnId & snapshot_id : listing.snapshots)
        if (snapshot_id < *newest_snapshot)
            plan.deletable_snapshots.push_back(snapshot_id);

    return plan;
}

RefTableState recoverRefTable(Backend & backend, const Layout & layout, const RootNamespace & ns,
                              const std::function<void()> & on_page_fetched, unsigned max_restarts)
{
    for (unsigned attempt = 0;; ++attempt)
    {
        /// One LIST of the table prefix; classify keys into logs and snapshots.
        std::vector<RefTxnId> logs;
        std::vector<RefTxnId> snapshots;
        String cursor;
        for (;;)
        {
            const ListPage page = backend.list(layout.refsNamespacePrefix(ns), cursor, 1000);
            if (on_page_fetched)
                on_page_fetched();
            for (const ListedKey & lk : page.keys)
            {
                const auto parsed = layout.parseRefObjectKey(lk.key);
                if (parsed && parsed->ns == ns)
                {
                    if (parsed->kind == RefObjectKind::Log)
                        logs.push_back(parsed->txn_id);
                    else if (parsed->kind == RefObjectKind::Snap)
                        snapshots.push_back(parsed->txn_id);
                }
            }
            if (page.next_cursor.empty())
                break;
            cursor = page.next_cursor;
        }
        std::sort(logs.begin(), logs.end());
        std::sort(snapshots.begin(), snapshots.end());

        bool vanished = false;

        /// Newest snapshot, if any.
        std::optional<RefTableSnapshot> snapshot;
        std::optional<RefTxnId> snapshot_id;
        if (!snapshots.empty())
        {
            snapshot_id = snapshots.back();
            const auto got = backend.get(layout.refSnapshotKey(ns, *snapshot_id));
            if (!got)
                vanished = true;
            else
                snapshot = decodeRefTableSnapshot(got->bytes, ns.string(), *snapshot_id);
        }

        /// Replay every log with id greater than the selected snapshot, in id order.
        std::vector<RefLogTxn> tail;
        if (!vanished)
            for (const RefTxnId & id : logs)
            {
                if (snapshot_id && !(*snapshot_id < id))
                    continue;   /// id <= snapshot: already included in the snapshot
                const auto got = backend.get(layout.refLogKey(ns, id));
                if (!got)
                {
                    vanished = true;
                    break;
                }
                tail.push_back(decodeRefLogTxn(got->bytes, ns.string(), id));
            }

        if (vanished)
        {
            if (attempt < max_restarts)
                continue;   /// a concurrent cleanup deleted a selected object; restart with a fresh LIST
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "recoverRefTable: table {} kept losing a selected snapshot/tail object across {} restarts",
                ns.string(), max_restarts);
        }

        return replay(snapshot, tail);
    }
}

}
