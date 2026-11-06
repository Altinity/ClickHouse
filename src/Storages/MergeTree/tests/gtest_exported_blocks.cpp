#include <gtest/gtest.h>

#include <Storages/MergeTree/ExportedBlocks.h>

using namespace DB;

namespace
{

MergeTreePartInfo part(const String & partition_id, Int64 min_block, Int64 max_block, UInt32 level = 0, Int64 mutation = 0)
{
    return MergeTreePartInfo(partition_id, min_block, max_block, level, mutation);
}

using Blocks = std::vector<std::pair<Int64, Int64>>;

Blocks blocks(const std::vector<MergeTreePartInfo> & ranges)
{
    Blocks result;
    for (const auto & range : ranges)
        result.emplace_back(range.min_block, range.max_block);
    return result;
}

}

TEST(ExportedBlocks, CompactMergesOnlyOverlappingOrAdjacent)
{
    const auto compacted = ExportBlockRanges::compact(
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

TEST(ExportedBlocks, IsCoveredByUnion)
{
    const std::vector<MergeTreePartInfo> ranges{part("p", 1, 2), part("p", 3, 5), part("p", 8, 9)};

    EXPECT_TRUE(ExportBlockRanges::isCoveredByUnion(part("p", 1, 5, 2), ranges));
    EXPECT_TRUE(ExportBlockRanges::isCoveredByUnion(part("p", 8, 9, 1), ranges));
    EXPECT_FALSE(ExportBlockRanges::isCoveredByUnion(part("p", 1, 9, 3), ranges));
    EXPECT_FALSE(ExportBlockRanges::isCoveredByUnion(part("p", 5, 6, 1), ranges));
    EXPECT_FALSE(ExportBlockRanges::isCoveredByUnion(part("q", 1, 2, 1), ranges));
}

TEST(ExportedBlocks, IntersectsAny)
{
    const std::vector<MergeTreePartInfo> ranges{part("p", 3, 5), part("p", 8, 8)};

    EXPECT_TRUE(ExportBlockRanges::intersectsAny(part("p", 1, 3, 1), ranges));
    EXPECT_TRUE(ExportBlockRanges::intersectsAny(part("p", 6, 9, 1), ranges));
    EXPECT_FALSE(ExportBlockRanges::intersectsAny(part("p", 6, 7, 1), ranges));
    EXPECT_FALSE(ExportBlockRanges::intersectsAny(part("q", 3, 5, 1), ranges));
    EXPECT_FALSE(ExportBlockRanges::intersectsAny(part("p", 3, 5), {}));
}

TEST(ExportedBlocks, Subtract)
{
    const std::vector<MergeTreePartInfo> ranges{part("p", 8, 9), part("p", 1, 2), part("p", 4, 5), part("q", 3, 3)};

    /// The ranges of a merged part that are not exported, gaps between exported ranges included.
    EXPECT_EQ(blocks(ExportBlockRanges::subtract(part("p", 1, 12, 3), ranges)), (Blocks{{3, 3}, {6, 7}, {10, 12}}));
    EXPECT_EQ(blocks(ExportBlockRanges::subtract(part("p", 0, 3, 1), ranges)), (Blocks{{0, 0}, {3, 3}}));
    EXPECT_TRUE(ExportBlockRanges::subtract(part("p", 4, 5, 1), ranges).empty());
    EXPECT_TRUE(ExportBlockRanges::subtract(part("p", 1, 2, 0, 42), ranges).empty());
    /// The ranges of another partition don't cover anything.
    EXPECT_EQ(blocks(ExportBlockRanges::subtract(part("q", 1, 4, 1), ranges)), (Blocks{{1, 2}, {4, 4}}));
    EXPECT_EQ(blocks(ExportBlockRanges::subtract(part("p", 6, 7, 1), ranges)), (Blocks{{6, 7}}));
    EXPECT_EQ(blocks(ExportBlockRanges::subtract(part("p", 6, 7, 1), {})), (Blocks{{6, 7}}));

    for (const auto & range : ExportBlockRanges::subtract(part("p", 1, 12, 3), ranges))
        EXPECT_EQ(range.getPartitionId(), "p");
}

TEST(ExportedBlocks, IsExportedOnlyIfEveryBlockIs)
{
    ExportedBlocks exported_blocks;
    EXPECT_TRUE(exported_blocks.empty());

    exported_blocks.ranges_by_partition["p"]["db.dst"] = {part("p", 1, 5), part("p", 7, 7)};
    EXPECT_FALSE(exported_blocks.empty());

    /// A mutated or merged part keeps its block range.
    EXPECT_TRUE(exported_blocks.isExported(part("p", 1, 5, 3, 42), "db.dst"));
    EXPECT_TRUE(exported_blocks.isExported(part("p", 2, 3, 1), "db.dst"));
    EXPECT_TRUE(exported_blocks.isExported(part("p", 7, 7), "db.dst"));
    /// A part merged across a block that is not exported holds rows that are not exported, or none of
    /// that block if it held no part.
    EXPECT_FALSE(exported_blocks.isExported(part("p", 1, 7, 2), "db.dst"));
    EXPECT_FALSE(exported_blocks.isExported(part("p", 6, 6), "db.dst"));
    EXPECT_FALSE(exported_blocks.isExported(part("p", 1, 1), "db.other"));
    EXPECT_FALSE(exported_blocks.isExported(part("q", 1, 1), "db.dst"));
}
