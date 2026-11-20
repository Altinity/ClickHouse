#include <Storages/MergeTree/ExportedBlocks.h>

#include <algorithm>

namespace DB
{

namespace ExportBlockRanges
{

std::vector<MergeTreePartInfo> compact(std::vector<MergeTreePartInfo> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const MergeTreePartInfo & lhs, const MergeTreePartInfo & rhs)
    {
        if (lhs.getPartitionId() != rhs.getPartitionId())
            return lhs.getPartitionId() < rhs.getPartitionId();
        return lhs.min_block < rhs.min_block;
    });

    std::vector<MergeTreePartInfo> result;
    result.reserve(ranges.size());

    for (auto & range : ranges)
    {
        if (!result.empty()
            && result.back().getPartitionId() == range.getPartitionId()
            && range.min_block <= result.back().max_block + 1)
        {
            result.back().max_block = std::max(result.back().max_block, range.max_block);
            result.back().level = std::max(result.back().level, range.level);
            continue;
        }

        result.push_back(std::move(range));
    }

    return result;
}

bool isCoveredByUnion(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges)
{
    return subtract(part, ranges).empty();
}

bool intersectsAny(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges)
{
    return std::any_of(ranges.begin(), ranges.end(), [&](const MergeTreePartInfo & range) { return !part.isDisjoint(range); });
}

std::vector<MergeTreePartInfo> subtract(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges)
{
    std::vector<MergeTreePartInfo> result;
    auto next_uncovered = part.min_block;

    for (const auto & range : compact(ranges))
    {
        if (range.getPartitionId() != part.getPartitionId() || range.max_block < next_uncovered)
            continue;

        if (range.min_block > part.max_block)
            break;

        if (range.min_block > next_uncovered)
            result.emplace_back(part.getPartitionId(), next_uncovered, range.min_block - 1, /* level */ 0, /* mutation */ 0);

        next_uncovered = range.max_block + 1;
        if (next_uncovered > part.max_block)
            break;
    }

    if (next_uncovered <= part.max_block)
        result.emplace_back(part.getPartitionId(), next_uncovered, part.max_block, /* level */ 0, /* mutation */ 0);

    return result;
}

}

bool ExportedBlocks::isExported(const MergeTreePartInfo & part, const String & destination) const
{
    const auto partition_it = ranges_by_partition.find(part.getPartitionId());
    if (partition_it == ranges_by_partition.end())
        return false;

    const auto destination_it = partition_it->second.find(destination);
    if (destination_it == partition_it->second.end())
        return false;

    return ExportBlockRanges::isCoveredByUnion(part, destination_it->second);
}

}
