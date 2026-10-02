#include <gtest/gtest.h>

#include <Storages/MergeTree/ExportFence.h>

using namespace DB;

namespace
{

MergeTreePartInfo part(const String & partition_id, Int64 min_block, Int64 max_block, UInt32 level = 0, Int64 mutation = 0)
{
    return MergeTreePartInfo(partition_id, min_block, max_block, level, mutation);
}

}

TEST(ExportFence, CompactRangesMergesOnlyOverlappingOrAdjacent)
{
    const auto compacted = ExportFenceUtils::compactRanges(
        {part("p", 7, 8), part("p", 1, 2), part("p", 3, 5), part("q", 1, 1), part("p", 10, 12), part("p", 11, 11)});

    ASSERT_EQ(compacted.size(), 4);
    EXPECT_EQ(compacted[0].getPartitionId(), "p");
    EXPECT_EQ(compacted[0].min_block, 1);
    EXPECT_EQ(compacted[0].max_block, 5);
    /// Block 6 is a gap: it may still be committed later as a new part.
    EXPECT_EQ(compacted[1].min_block, 7);
    EXPECT_EQ(compacted[1].max_block, 8);
    EXPECT_EQ(compacted[2].min_block, 10);
    EXPECT_EQ(compacted[2].max_block, 12);
    EXPECT_EQ(compacted[3].getPartitionId(), "q");
}

TEST(ExportFence, IsCoveredByUnion)
{
    const std::vector<MergeTreePartInfo> ranges{part("p", 1, 2), part("p", 3, 5), part("p", 8, 9)};

    EXPECT_TRUE(ExportFenceUtils::isCoveredByUnion(part("p", 1, 5, 2), ranges));
    EXPECT_TRUE(ExportFenceUtils::isCoveredByUnion(part("p", 8, 9, 1), ranges));
    EXPECT_FALSE(ExportFenceUtils::isCoveredByUnion(part("p", 1, 9, 3), ranges));
    EXPECT_FALSE(ExportFenceUtils::isCoveredByUnion(part("p", 5, 6, 1), ranges));
    EXPECT_FALSE(ExportFenceUtils::isCoveredByUnion(part("q", 1, 2, 1), ranges));
}

TEST(ExportFence, ClassifyByOverlap)
{
    ExportFenceEntry entry;
    entry.destination = "db.dst";
    entry.exported = {part("p", 1, 5)};
    entry.claimed = {part("p", 7, 7)};

    /// A mutated or merged exported part keeps its block range, so it is still exported.
    EXPECT_EQ(entry.classify(part("p", 1, 5, 3, 42)), PartExportState::EXPORTED);
    EXPECT_EQ(entry.classify(part("p", 2, 3, 1)), PartExportState::EXPORTED);
    EXPECT_EQ(entry.classify(part("p", 7, 7)), PartExportState::CLAIMED);
    EXPECT_EQ(entry.classify(part("p", 6, 6)), PartExportState::NONE);
    EXPECT_EQ(entry.classify(part("p", 8, 8)), PartExportState::NONE);
    EXPECT_EQ(entry.classify(part("q", 1, 5)), PartExportState::NONE);
}

TEST(ExportFence, CheckCanMerge)
{
    ExportFence fence;
    ExportFenceEntry entry;
    entry.destination = "db.dst";
    entry.exported = {part("p", 1, 5)};
    entry.claimed = {part("p", 7, 8)};
    fence.entries_by_partition["p"].push_back(entry);

    EXPECT_FALSE(fence.checkCanMerge(part("p", 1, 2), part("p", 3, 5)).has_value());
    EXPECT_FALSE(fence.checkCanMerge(part("p", 9, 9), part("p", 10, 10)).has_value());
    /// Claimed parts are being exported by name, so they are not merged even with each other.
    EXPECT_TRUE(fence.checkCanMerge(part("p", 7, 7), part("p", 8, 8)).has_value());
    EXPECT_TRUE(fence.checkCanMerge(part("p", 1, 5), part("p", 6, 6)).has_value());
    EXPECT_TRUE(fence.checkCanMerge(part("p", 6, 6), part("p", 7, 7)).has_value());
    EXPECT_TRUE(fence.checkCanMerge(part("p", 3, 5), part("p", 7, 8)).has_value());

    /// Two parts that are not exported must not merge across the range of an exported part that
    /// no longer exists: the merged part would overlap it and look exported.
    EXPECT_TRUE(fence.checkCanMerge(part("p", 0, 0), part("p", 6, 6)).has_value());
    EXPECT_TRUE(fence.checkCanMerge(part("p", 6, 6), part("p", 9, 9)).has_value());
    /// Exported parts may merge across an exported gap, and parts of any state across blocks without rows.
    EXPECT_FALSE(fence.checkCanMerge(part("p", 1, 1), part("p", 5, 5)).has_value());
    EXPECT_FALSE(fence.checkCanMerge(part("p", 10, 10), part("p", 12, 12)).has_value());

    /// Partitions nothing was exported from are not fenced.
    EXPECT_FALSE(fence.checkCanMerge(part("q", 1, 1), part("q", 2, 2)).has_value());

    EXPECT_EQ(fence.classify(part("p", 1, 1), "db.dst"), PartExportState::EXPORTED);
    EXPECT_EQ(fence.classify(part("p", 1, 1), "db.other"), PartExportState::NONE);
}
