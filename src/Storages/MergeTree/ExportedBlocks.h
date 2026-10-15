#pragma once

#include <Storages/MergeTree/MergeTreePartInfo.h>
#include <base/types.h>

#include <memory>
#include <unordered_map>
#include <vector>

namespace DB
{

/// Export state of a data part relative to the `TTL ... EXPORT` of its partition to one destination.
enum class PartExportState : UInt8
{
    /// The part holds rows of blocks that were never exported to the destination.
    NONE,
    /// The part holds rows of the blocks that the current TTL export task of its partition exports.
    CLAIMED,
    /// Every block of the part was committed to the destination.
    EXPORTED,
};

/// The block ranges committed to each destination, in every partition of a table.
///
/// With `enable_block_number_column`, merges keep the number of the block every row was inserted in,
/// and that number lies within the block range of the row's part, so a part whose range is covered by
/// exported ranges consists of exported rows only. Immutable once published, so a merge predicate can
/// read it without locks.
struct ExportedBlocks
{
    /// Partition id -> destination key -> exported ranges, compacted.
    std::unordered_map<String, std::unordered_map<String, std::vector<MergeTreePartInfo>>> ranges_by_partition;

    bool empty() const { return ranges_by_partition.empty(); }

    /// Whether every block of the part is exported to `destination`.
    bool isExported(const MergeTreePartInfo & part, const String & destination) const;
};

using ExportedBlocksPtr = std::shared_ptr<const ExportedBlocks>;

namespace ExportBlockRanges
{
    /// Sorts the ranges and merges the ones that overlap or are adjacent (`max_block + 1 == min_block`).
    /// A gap between ranges is preserved: a block number in the gap may still be committed later as
    /// a new part, which must not be classified as exported.
    std::vector<MergeTreePartInfo> compact(std::vector<MergeTreePartInfo> ranges);

    /// True if every block number of `part` is covered by the union of `ranges`.
    bool isCoveredByUnion(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges);

    /// True if `part` overlaps at least one of `ranges`.
    bool intersectsAny(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges);

    /// The block numbers of `part` that none of `ranges` covers, as compacted ranges.
    std::vector<MergeTreePartInfo> subtract(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges);
}

}
