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

    entry.startClaim("t1");
    EXPECT_EQ(entry.current_task, "t1");
    EXPECT_FALSE(entry.empty());

    /// The ranges of the current task live in the task.
    const std::vector<MergeTreePartInfo> task_ranges{part(1, 2), part(4, 4)};
    EXPECT_EQ(entry.classify(part(1, 2, 1), task_ranges), PartExportState::CLAIMED);
    /// A part merged with a claimed part is in flight too: the task exports its claimed rows.
    EXPECT_EQ(entry.classify(part(1, 3, 1), task_ranges), PartExportState::CLAIMED);
    EXPECT_EQ(entry.classify(part(3, 3), task_ranges), PartExportState::NONE);

    /// The commit adds only the task's ranges, not those of the parts that hold them.
    entry.commitClaim("t1", task_ranges);
    EXPECT_TRUE(entry.current_task.empty());
    EXPECT_EQ(blocks(entry.exported), (Blocks{{1, 2}, {4, 4}}));
    /// A mutated exported part keeps its block range.
    EXPECT_EQ(entry.classify(part(4, 4, 0, 7), {}), PartExportState::EXPORTED);
    EXPECT_EQ(entry.classify(part(3, 3), {}), PartExportState::NONE);
    /// A part merged from exported and other blocks is not exported.
    EXPECT_EQ(entry.classify(part(1, 4, 2), {}), PartExportState::NONE);
    EXPECT_EQ(blocks(entry.notExported(part(1, 6, 2))), (Blocks{{3, 3}, {5, 6}}));
    EXPECT_EQ(entry.maxBlock(), 4);
}

TEST(ExportTTLIndex, RetryMovesThePointer)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.startClaim("t1");

    entry.retryClaim("t2");
    EXPECT_EQ(entry.current_task, "t2");

    /// The failed task commits after all, e.g. its commit to the destination was only discovered
    /// later: its ranges are exported, and the retry stays the current task until it settles.
    entry.commitClaim("t1", {part(1, 2)});
    EXPECT_EQ(entry.current_task, "t2");
    EXPECT_EQ(blocks(entry.exported), (Blocks{{1, 2}}));

    entry.commitClaim("t2", {part(1, 2)});
    EXPECT_TRUE(entry.current_task.empty());
    EXPECT_EQ(blocks(entry.exported), (Blocks{{1, 2}}));
}

TEST(ExportTTLIndex, ReleaseExportsNothing)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.exported = {part(1, 2)};
    entry.startClaim("t1");

    entry.releaseClaim();
    EXPECT_TRUE(entry.current_task.empty());
    EXPECT_EQ(blocks(entry.exported), (Blocks{{1, 2}}));

    /// A new task can start once the partition has none.
    entry.startClaim("t2");
    EXPECT_EQ(entry.current_task, "t2");
}

/// Starting a second task or retrying without one is a `LOGICAL_ERROR`, which aborts debug and
/// sanitizer builds instead of throwing.
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(ExportTTLIndex, OneTaskAtATime)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    EXPECT_THROW(entry.retryClaim("t1"), Exception);

    entry.startClaim("t1");
    EXPECT_THROW(entry.startClaim("t2"), Exception);
    EXPECT_EQ(entry.current_task, "t1");
}
#else
TEST(ExportTTLIndexDeathTest, OneTaskAtATime)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    EXPECT_DEATH(entry.retryClaim("t1"), "cannot retry the task of partition p");

    entry.startClaim("t1");
    EXPECT_DEATH(entry.startClaim("t2"), "cannot start for partition p");
}
#endif

TEST(ExportTTLIndex, CommitOfAnotherTaskKeepsThePointer)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.commitClaim("t1", {part(3, 5)});
    EXPECT_EQ(blocks(entry.exported), (Blocks{{3, 5}}));
    EXPECT_TRUE(entry.current_task.empty());

    entry.startClaim("t2");
    entry.commitClaim("t1", {part(6, 6)});
    EXPECT_EQ(blocks(entry.exported), (Blocks{{3, 6}}));
    EXPECT_EQ(entry.current_task, "t2");
}

TEST(ExportTTLIndex, JsonRoundTrip)
{
    ExportTTLIndexEntry entry;
    entry.partition_id = "p";
    entry.exported = {part(0, 10), part(20, 30)};

    auto parsed = ExportTTLIndexEntry::fromJSONString("p", entry.toJSONString());
    EXPECT_EQ(blocks(parsed.exported), (Blocks{{0, 10}, {20, 30}}));
    EXPECT_TRUE(parsed.current_task.empty());

    entry.startClaim("t1");
    parsed = ExportTTLIndexEntry::fromJSONString("p", entry.toJSONString());
    EXPECT_EQ(parsed.partition_id, "p");
    EXPECT_EQ(blocks(parsed.exported), (Blocks{{0, 10}, {20, 30}}));
    EXPECT_EQ(parsed.current_task, "t1");

    EXPECT_TRUE(ExportTTLIndexEntry::fromJSONString("p", "").empty());
    EXPECT_ANY_THROW(ExportTTLIndexEntry::fromJSONString("p", R"({"exported":[[1]]})"));
}

TEST(ExportTTLIndex, DestinationKeysOfDottedNamesDiffer)
{
    EXPECT_NE(ExportTTLUtils::destinationKey("db.x", "y"), ExportTTLUtils::destinationKey("db", "x.y"));
}

TEST(ExportTTLIndex, BlockRangesOfTasks)
{
    const auto ranges = ExportTTLUtils::toPartInfos("p", {{5, 5}, {1, 2}, {3, 3}});
    EXPECT_EQ(blocks(ranges), (Blocks{{1, 3}, {5, 5}}));
    EXPECT_EQ(ranges.front().getPartitionId(), "p");
    EXPECT_EQ(ExportTTLUtils::toBlockNumberRanges(ranges), (ExportBlockNumberRanges{{1, 3}, {5, 5}}));

    EXPECT_EQ(exportBlockNumberRangesFromJSON(exportBlockNumberRangesToJSON({{1, 3}, {5, 5}})), (ExportBlockNumberRanges{{1, 3}, {5, 5}}));
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
