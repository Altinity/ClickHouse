#pragma once

#include <Interpreters/Context_fwd.h>
#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <Storages/MergeTree/ExportBlockNumberRanges.h>
#include <Storages/MergeTree/ExportedBlocks.h>
#include <base/types.h>

#include <map>
#include <memory>
#include <vector>

namespace DB
{

/// What a table's `TTL ... EXPORT TO TABLE` has exported to one destination from one partition.
///
/// Rows are identified by the number of the block they were inserted in, which merges and mutations
/// keep (`enable_block_number_column`): a part may hold rows of exported blocks and of other blocks,
/// and an export task reads only the rows of the blocks it exports. Ranges are qualified by the
/// partition because `ReplicatedMergeTree` allocates block numbers per partition.
///
/// The entry is only updated together with a transition of a TTL export task, in the same Keeper
/// transaction (or under the same lock for a plain `MergeTree`), so it always agrees with what was
/// committed to the destination:
///  - creating a task makes it the current task of the partition;
///  - committing a task adds its block ranges to `exported`;
///  - retrying a failed task makes the retry, which exports the same block ranges under the same
///    commit id, the current task;
///  - resolving a failed task that does not need a retry leaves the partition without a current task.
struct ExportTTLIndexEntry
{
    String partition_id;

    /// Ranges committed to the destination, compacted.
    std::vector<MergeTreePartInfo> exported;

    /// Transaction id of the TTL export task that exports blocks of the partition now: in flight, or
    /// failed and waiting to be retried. Empty if there is none. The task holds its block ranges and
    /// its commit id. A partition has at most one: a group starts only while it has none.
    String current_task;

    bool empty() const { return exported.empty() && current_task.empty(); }

    /// Highest block number of any exported range, 0 if there is none.
    Int64 maxBlock() const;

    /// State of `part`, given the block ranges of the current task.
    PartExportState classify(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & current_task_ranges) const;

    /// The blocks of `part` that are not exported.
    std::vector<MergeTreePartInfo> notExported(const MergeTreePartInfo & part) const;

    /// Makes `transaction_id` the current task. Throws if the partition has one already.
    void startClaim(const String & transaction_id);

    /// Makes `transaction_id`, which retries the current task, the current task. Throws if there is none.
    void retryClaim(const String & transaction_id);

    /// Adds `ranges`, the block ranges of the committed task `transaction_id`, to the exported ranges,
    /// and leaves the partition without a current task if it was that one. A task may commit after a
    /// retry of it became the current task.
    void commitClaim(const String & transaction_id, const std::vector<MergeTreePartInfo> & ranges);

    void releaseClaim() { current_task.clear(); }

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

/// The whole export index of a table as of one version, with the exported blocks the delete gate reads.
struct ExportTTLIndexSnapshot
{
    /// Version of the `export_ttl/version` node of a `ReplicatedMergeTree` it was read at: every change of
    /// the index bumps it, so the snapshot stays valid while it does not change. -1 for a plain `MergeTree`.
    int32_t version = -1;

    /// By destination key, then by partition id.
    std::map<String, std::map<String, ExportTTLVersionedEntry>> entries;

    ExportedBlocksPtr exported_blocks;

    static std::unique_ptr<const ExportTTLIndexSnapshot> build(
        int32_t version, std::map<String, std::map<String, ExportTTLVersionedEntry>> entries);
};

using ExportTTLIndexSnapshotPtr = std::shared_ptr<const ExportTTLIndexSnapshot>;

namespace ExportTTLUtils
{
    /// The `EXPORT` TTL always allows lossy casts: its tasks run in the background, so a session
    /// that opted in at `CREATE` or `ALTER` would not reach them, and Iceberg has no unsigned types.
    void allowLossyCasts(Context & context);

    /// Identifies a destination in the export index, by name: a table dropped and created again under
    /// the same name is the same destination.
    String destinationKey(const String & database, const String & table);

    /// Conversions between the block ranges of the export index and those of task descriptors.
    std::vector<MergeTreePartInfo> toPartInfos(const String & partition_id, const ExportBlockNumberRanges & ranges);
    ExportBlockNumberRanges toBlockNumberRanges(const std::vector<MergeTreePartInfo> & ranges);
}

}
