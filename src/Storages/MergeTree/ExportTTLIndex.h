#pragma once

#include <Interpreters/Context_fwd.h>
#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <Storages/MergeTree/ExportFence.h>
#include <base/types.h>

#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace DB
{

/// What a table's `TTL ... EXPORT TO TABLE` has exported to one destination from one partition.
///
/// Rows are identified by block numbers: every insert gets its own block number, a part covers
/// exactly the rows of its block range, and merges and mutations keep the range. Ranges are
/// qualified by the partition because `ReplicatedMergeTree` allocates block numbers per partition.
///
/// The entry is only updated together with a transition of a TTL export task, in the same Keeper
/// transaction (or under the same lock for a plain `MergeTree`), so it always agrees with what was
/// committed to the destination:
///  - creating a task claims the ranges of its parts;
///  - committing a task moves its claim to `exported`;
///  - retrying failed tasks moves their claims to the new task;
///  - resolving a failed task that does not need a retry releases its claim.
struct ExportTTLIndexEntry
{
    String partition_id;

    /// Ranges committed to the destination, compacted.
    std::vector<MergeTreePartInfo> exported;

    /// Ranges owned by TTL export tasks that did not commit, by transaction id, compacted. A task
    /// may be in flight, or failed and waiting to be retried.
    std::map<String, std::vector<MergeTreePartInfo>> claimed;

    bool empty() const { return exported.empty() && claimed.empty(); }

    /// Highest block number of any exported or claimed range, 0 if there is none.
    Int64 maxBlock() const;

    std::vector<MergeTreePartInfo> allClaimed() const;

    PartExportState classify(const MergeTreePartInfo & part) const;

    ExportFenceEntry toFenceEntry(const String & destination) const;

    /// Adds the ranges of `parts` to the claim of `transaction_id`.
    void claim(const String & transaction_id, const std::vector<MergeTreePartInfo> & parts);

    /// Moves the claims of `from` to `to`.
    void moveClaims(const std::vector<String> & from, const String & to);

    /// Moves the claim of `transaction_id` to the exported ranges, adding `parts` as well: a task
    /// may commit parts whose claim was lost.
    void commitClaim(const String & transaction_id, const std::vector<MergeTreePartInfo> & parts);

    void releaseClaim(const String & transaction_id);

    String toJSONString() const;
    static ExportTTLIndexEntry fromJSONString(const String & partition_id, const String & json_string);
};

/// An index entry as read, with the version to update it with a check.
struct ExportTTLVersionedEntry
{
    ExportTTLIndexEntry entry;
    /// Version of the stored entry, or -1 if it is not stored yet.
    int32_t version = -1;
};

/// The whole export index of a table as of one version, with the merge fence built from it.
struct ExportTTLIndexSnapshot
{
    /// Version of the `export_ttl/version` node of a `ReplicatedMergeTree` it was read at: every change of
    /// the index bumps it, so the snapshot stays valid while it does not change. -1 for a plain `MergeTree`.
    int32_t version = -1;

    /// By destination key, then by partition id.
    std::map<String, std::map<String, ExportTTLVersionedEntry>> entries;

    ExportFencePtr fence;

    static std::unique_ptr<const ExportTTLIndexSnapshot> build(
        int32_t version, std::map<String, std::map<String, ExportTTLVersionedEntry>> entries);
};

using ExportTTLIndexSnapshotPtr = std::shared_ptr<const ExportTTLIndexSnapshot>;

namespace ExportTTLUtils
{
    /// The `EXPORT` TTL always allows lossy casts: its tasks run in the background, so a session
    /// that opted in at `CREATE` or `ALTER` would not reach them, and Iceberg has no unsigned types.
    void allowLossyCasts(Context & context);

    /// Identifies a destination in the export index. The UUID, if not empty, tells apart a table that
    /// was dropped and created again under the same name, which holds none of the exported rows.
    String destinationKey(const String & database, const String & table, const String & uuid);

    /// Ranges of the parts named `part_names`, compacted.
    std::vector<MergeTreePartInfo> rangesOfParts(const std::vector<String> & part_names, MergeTreeDataFormatVersion format_version);

    /// `[min_block, max_block]` of `ranges`, as recorded for a retried task (see `ExportRetriedTask`).
    std::vector<std::pair<Int64, Int64>> toBlockRanges(const std::vector<MergeTreePartInfo> & ranges);
    std::vector<MergeTreePartInfo> fromBlockRanges(const String & partition_id, const std::vector<std::pair<Int64, Int64>> & block_ranges);
}

}
