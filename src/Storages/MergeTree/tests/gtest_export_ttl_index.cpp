#include <gtest/gtest.h>

#include <Storages/MergeTree/ExportTTLIndex.h>
#include <Storages/MergeTree/MergeTreeDataPartTTLInfo.h>
#include <Storages/TTLDescription.h>
#include <Common/Exception.h>
#include <base/defines.h>

using namespace DB;

namespace
{

MergeTreePartInfo part(Int64 min_block, Int64 max_block, UInt32 level = 0, Int64 mutation = 0)
{
    return MergeTreePartInfo("p", min_block, max_block, level, mutation);
}

std::vector<std::pair<Int64, Int64>> blocks(const std::vector<MergeTreePartInfo> & ranges)
{
    std::vector<std::pair<Int64, Int64>> result;
    for (const auto & range : ranges)
        result.emplace_back(range.min_block, range.max_block);
    return result;
}

using Blocks = std::vector<std::pair<Int64, Int64>>;

}

TEST(ExportTTLIndex, ClaimThenCommit)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";

    entry.startClaim("t1", {part(1, 1), part(2, 2), part(4, 4)});
    ASSERT_TRUE(entry.claim);
    EXPECT_EQ(entry.claim->transaction_id, "t1");
    EXPECT_EQ(blocks(entry.claim->ranges), (Blocks{{1, 2}, {4, 4}}));
    EXPECT_EQ(blocks(entry.toFenceEntry("db.t").claimed), (Blocks{{1, 2}, {4, 4}}));
    EXPECT_EQ(entry.classify(part(1, 2, 1)), PartExportState::CLAIMED);
    EXPECT_EQ(entry.classify(part(3, 3)), PartExportState::NONE);

    entry.commitClaim("t1", {});
    EXPECT_FALSE(entry.claim);
    EXPECT_EQ(blocks(entry.exported), (Blocks{{1, 2}, {4, 4}}));
    /// A mutated exported part keeps its block range.
    EXPECT_EQ(entry.classify(part(4, 4, 0, 7)), PartExportState::EXPORTED);
    EXPECT_EQ(entry.classify(part(3, 3)), PartExportState::NONE);
    EXPECT_EQ(entry.maxBlock(), 4);
}

TEST(ExportTTLIndex, RetryReclaimsExactRanges)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.startClaim("t1", {part(1, 1), part(2, 2)});

    /// Part 2 was dropped before the retry, part 5 became eligible meanwhile.
    entry.releaseClaim();
    entry.startClaim("t2", {part(1, 1), part(5, 5)});

    ASSERT_TRUE(entry.claim);
    EXPECT_EQ(entry.claim->transaction_id, "t2");
    EXPECT_EQ(blocks(entry.claim->ranges), (Blocks{{1, 1}, {5, 5}}));
    EXPECT_EQ(entry.classify(part(2, 2)), PartExportState::NONE);
    EXPECT_EQ(entry.maxBlock(), 5);
}

/// A second claim is a `LOGICAL_ERROR`, which aborts debug and sanitizer builds instead of throwing.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(ExportTTLIndex, OneClaimAtATime)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.startClaim("t1", {part(1, 1)});

    EXPECT_THROW(entry.startClaim("t2", {part(2, 2)}), Exception);
    EXPECT_EQ(entry.claim->transaction_id, "t1");
    EXPECT_EQ(blocks(entry.claim->ranges), (Blocks{{1, 1}}));
}
#else
TEST(ExportTTLIndexDeathTest, OneClaimAtATime)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.startClaim("t1", {part(1, 1)});

    EXPECT_DEATH(entry.startClaim("t2", {part(2, 2)}), "cannot claim parts of partition p");
}
#endif

TEST(ExportTTLIndex, CommitAddsPartsWhoseClaimWasLost)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.commitClaim("t1", {part(3, 5)});
    EXPECT_EQ(blocks(entry.exported), (Blocks{{3, 5}}));

    /// The claim of another task is kept.
    entry.startClaim("t2", {part(7, 7)});
    entry.commitClaim("t1", {part(6, 6)});
    EXPECT_EQ(blocks(entry.exported), (Blocks{{3, 6}}));
    ASSERT_TRUE(entry.claim);
    EXPECT_EQ(entry.claim->transaction_id, "t2");
}

TEST(ExportTTLIndex, JsonRoundTrip)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.exported = {part(0, 10), part(20, 30)};

    auto parsed = ExportTTLIndexEntry::fromJSONString("p", entry.toJSONString());
    EXPECT_EQ(blocks(parsed.exported), (Blocks{{0, 10}, {20, 30}}));
    EXPECT_FALSE(parsed.claim);

    entry.startClaim("t1", {part(31, 31)});
    parsed = ExportTTLIndexEntry::fromJSONString("p", entry.toJSONString());
    EXPECT_EQ(parsed.partition_id, "p");
    EXPECT_EQ(blocks(parsed.exported), (Blocks{{0, 10}, {20, 30}}));
    ASSERT_TRUE(parsed.claim);
    EXPECT_EQ(parsed.claim->transaction_id, "t1");
    EXPECT_EQ(blocks(parsed.claim->ranges), (Blocks{{31, 31}}));

    EXPECT_TRUE(ExportTTLIndexEntry::fromJSONString("p", "").empty());
    EXPECT_ANY_THROW(ExportTTLIndexEntry::fromJSONString("p", R"({"exported":[[1]]})"));
}

TEST(ExportTTLIndex, DestinationKeysOfDottedNamesDiffer)
{
    EXPECT_NE(ExportTTLUtils::destinationKey("db.x", "y"), ExportTTLUtils::destinationKey("db", "x.y"));
}

TEST(ExportTTLIndex, RangesOfParts)
{
    const auto ranges = ExportTTLUtils::rangesOfParts({"p_3_3_0", "p_1_2_1", "p_5_5_0_9"}, MERGE_TREE_DATA_MIN_FORMAT_VERSION_WITH_CUSTOM_PARTITIONING);
    EXPECT_EQ(blocks(ranges), (Blocks{{1, 3}, {5, 5}}));
}

TEST(ExportTTLIndex, BlockRangesOfRetriedTasks)
{
    const std::vector<MergeTreePartInfo> ranges{part(1, 3), part(7, 7)};
    const auto block_ranges = ExportTTLUtils::toBlockRanges(ranges);
    EXPECT_EQ(block_ranges, (Blocks{{1, 3}, {7, 7}}));

    const auto restored = ExportTTLUtils::fromBlockRanges("p", {{4, 5}, {1, 3}});
    EXPECT_EQ(blocks(restored), (Blocks{{1, 5}}));
    EXPECT_EQ(restored.front().getPartitionId(), "p");
}

TEST(ExportTTLIndex, EligibleOnceTheMaximumIsDue)
{
    TTLDescription description;
    description.result_column = "plus(t, toIntervalDay(1))";
    const TTLDescriptions descriptions{description};

    TTLInfoMap infos;
    infos[description.result_column] = MergeTreeDataPartTTLInfo{.min = 100, .max = 200, .ttl_finished = {}};

    EXPECT_FALSE(selectTTLDescriptionForTTLInfos(descriptions, infos, 150, /* use_max */ true).has_value());
    EXPECT_TRUE(selectTTLDescriptionForTTLInfos(descriptions, infos, 200, /* use_max */ true).has_value());

    /// A part without the TTL info is never eligible.
    EXPECT_FALSE(selectTTLDescriptionForTTLInfos(descriptions, {}, 1000, /* use_max */ true).has_value());
}
