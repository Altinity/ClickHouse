#pragma once

#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <base/types.h>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace DB
{

/// Export state of a data part relative to the `TTL ... EXPORT` of its partition to one destination.
enum class PartExportState : UInt8
{
    /// The part holds rows that were never exported to the destination.
    NONE,
    /// The part holds rows reserved by a TTL export task that did not commit yet.
    CLAIMED,
    /// The part holds rows committed to the destination.
    EXPORTED,
};

/// Block ranges exported to one destination, and claimed for it, in one partition.
///
/// A part is classified by overlap, not by name: mutations, TTL rewrites and merges among parts of
/// the same state keep block ranges, so a part that overlaps an exported range consists of exported
/// rows only. That holds only because parts in different states are never merged together, which
/// is what `ExportFence::checkCanMerge` enforces.
struct ExportFenceEntry
{
    /// `db.table` of the destination, for messages only.
    String destination;
    /// Ranges committed to the destination, compacted.
    std::vector<MergeTreePartInfo> exported;
    /// Ranges claimed by TTL export tasks that did not commit, compacted.
    std::vector<MergeTreePartInfo> claimed;

    PartExportState classify(const MergeTreePartInfo & part) const;
};

/// Snapshot of the TTL export state of every partition of a table, for every destination. Immutable
/// once published, so a merge predicate can read it without locks.
struct ExportFence
{
    std::unordered_map<String, std::vector<ExportFenceEntry>> entries_by_partition;

    bool empty() const { return entries_by_partition.empty(); }

    /// Returns the reason why the two parts must not be merged, or nothing if they may be: they must
    /// have the same state, as must any exported or claimed blocks between them, and claimed parts
    /// are not merged at all.
    std::optional<String> checkCanMerge(const MergeTreePartInfo & left, const MergeTreePartInfo & right) const;

    /// State of the part relative to `destination`, or NONE if nothing of its partition was exported there.
    PartExportState classify(const MergeTreePartInfo & part, const String & destination) const;
};

using ExportFencePtr = std::shared_ptr<const ExportFence>;

namespace ExportFenceUtils
{
    /// Sorts the ranges and merges the ones that overlap or are adjacent (`max_block + 1 == min_block`).
    /// A gap between ranges is preserved: a block number in the gap may still be committed later as
    /// a new part, which must not be classified as exported.
    std::vector<MergeTreePartInfo> compactRanges(std::vector<MergeTreePartInfo> ranges);

    /// True if every block number of `part` is covered by the union of `ranges`.
    bool isCoveredByUnion(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges);

    /// True if `part` overlaps at least one of `ranges`.
    bool intersectsAny(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges);
}

}
