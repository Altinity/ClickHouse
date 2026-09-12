#include <Interpreters/Cache/QueryConditionCache.h>
#include <base/UUID.h>
#include <base/unit.h>
#include <gtest/gtest.h>

using namespace DB;

TEST(QueryConditionCache, KeepsEntriesWithDifferentMarkLayoutsSeparate)
{
    QueryConditionCache cache("LRU", 1_MiB, 0.5);
    const UUID table_id(1);
    const String part_name = "part";
    constexpr UInt64 condition_hash = 42;

    MarkRanges one_mark;
    one_mark.emplace_back(0, 1);
    cache.write(table_id, part_name, condition_hash, "", one_mark, 1, false);

    MarkRanges two_marks;
    two_marks.emplace_back(1, 2);
    cache.write(table_id, part_name, condition_hash, "", two_marks, 2, false);

    auto first_layout = cache.read(table_id, part_name, condition_hash, 1, false);
    ASSERT_TRUE(first_layout);
    ASSERT_EQ(first_layout->size(), 1);
    EXPECT_FALSE((*first_layout)[0]);

    auto second_layout = cache.read(table_id, part_name, condition_hash, 2, false);
    ASSERT_TRUE(second_layout);
    ASSERT_EQ(second_layout->size(), 2);
    EXPECT_TRUE((*second_layout)[0]);
    EXPECT_FALSE((*second_layout)[1]);
}

TEST(QueryConditionCache, RejectsRangesOutsideEntryMarkLayout)
{
    QueryConditionCache cache("LRU", 1_MiB, 0.5);
    MarkRanges invalid_range;
    invalid_range.emplace_back(0, 2);

    /// An invalid mark range (out of bounds for the given marks_count) is a caller bug, but write()
    /// logs and skips rather than throwing from inside the query pipeline (see Altinity/ClickHouse#2342).
    EXPECT_NO_THROW(cache.write(UUID(1), "part", 42, "", invalid_range, 1, false));

    /// Nothing should have been cached for this key.
    auto result = cache.read(UUID(1), "part", 42, 1, false);
    EXPECT_FALSE(result);
}
