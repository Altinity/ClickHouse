#include <Storages/MergeTree/ExportFence.h>

#include <fmt/format.h>
#include <magic_enum.hpp>

#include <algorithm>

namespace DB
{

namespace ExportFenceUtils
{

std::vector<MergeTreePartInfo> compactRanges(std::vector<MergeTreePartInfo> ranges)
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
    auto next_uncovered = part.min_block;

    for (const auto & range : compactRanges(ranges))
    {
        if (range.getPartitionId() != part.getPartitionId() || range.max_block < next_uncovered)
            continue;

        if (range.min_block > next_uncovered)
            return false;

        next_uncovered = range.max_block + 1;
        if (next_uncovered > part.max_block)
            return true;
    }

    return next_uncovered > part.max_block;
}

bool intersectsAny(const MergeTreePartInfo & part, const std::vector<MergeTreePartInfo> & ranges)
{
    return std::any_of(ranges.begin(), ranges.end(), [&](const MergeTreePartInfo & range) { return !part.isDisjoint(range); });
}

}

PartExportState ExportFenceEntry::classify(const MergeTreePartInfo & part) const
{
    if (ExportFenceUtils::intersectsAny(part, claimed))
        return PartExportState::CLAIMED;
    if (ExportFenceUtils::intersectsAny(part, exported))
        return PartExportState::EXPORTED;
    return PartExportState::NONE;
}

std::optional<String> ExportFence::checkCanMerge(const MergeTreePartInfo & left, const MergeTreePartInfo & right) const
{
    if (left.isPatch() || right.isPatch())
        return std::nullopt;

    const auto it = entries_by_partition.find(left.getPartitionId());
    if (it == entries_by_partition.end())
        return std::nullopt;

    for (const auto & entry : it->second)
    {
        const auto left_state = entry.classify(left);
        const auto right_state = entry.classify(right);
        if (left_state != right_state)
            return fmt::format(
                "Parts {} and {} have different export states for destination {}: {} and {}",
                left.getPartNameForLogs(), right.getPartNameForLogs(), entry.destination,
                magic_enum::enum_name(left_state), magic_enum::enum_name(right_state));

        /// A task exports its parts by name, and a claim must consist of the ranges of one task's
        /// parts, which is what makes committing a retried task exact.
        if (left_state == PartExportState::CLAIMED)
            return fmt::format(
                "Parts {} and {} are being exported to destination {}",
                left.getPartNameForLogs(), right.getPartNameForLogs(), entry.destination);

        /// The ranges of parts that no longer exist (e.g. an exported part removed by a delete TTL)
        /// stay in the index. The merged part would span them, so it would be classified by them too.
        if (right.min_block > left.max_block + 1)
        {
            const MergeTreePartInfo gap(left.getPartitionId(), left.max_block + 1, right.min_block - 1, 0, 0);
            const auto gap_state = entry.classify(gap);
            if (gap_state != PartExportState::NONE && gap_state != left_state)
                return fmt::format(
                    "Parts {} and {} have different export states than blocks between them for destination {}: {} and {}",
                    left.getPartNameForLogs(), right.getPartNameForLogs(), entry.destination,
                    magic_enum::enum_name(left_state), magic_enum::enum_name(gap_state));
        }
    }

    return std::nullopt;
}

PartExportState ExportFence::classify(const MergeTreePartInfo & part, const String & destination) const
{
    const auto it = entries_by_partition.find(part.getPartitionId());
    if (it == entries_by_partition.end())
        return PartExportState::NONE;

    for (const auto & entry : it->second)
        if (entry.destination == destination)
            return entry.classify(part);

    return PartExportState::NONE;
}

}
