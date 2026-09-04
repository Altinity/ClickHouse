#include <algorithm>
#include <vector>

#include <Interpreters/Aggregator.h>

#include <gtest/gtest.h>

using namespace DB;

TEST(AggregatorBucketMergeOrder, EmptyInput)
{
    auto order = Aggregator::sortBucketsByRowCountDescending({});
    EXPECT_TRUE(order.empty());
}

TEST(AggregatorBucketMergeOrder, SingleBucket)
{
    auto order = Aggregator::sortBucketsByRowCountDescending({{5, 100}});
    ASSERT_EQ(order.size(), 1u);
    EXPECT_EQ(order[0], 5);
}

TEST(AggregatorBucketMergeOrder, SortsByRowCountDescendingRegardlessOfBucketId)
{
    /// Bucket ids intentionally don't correlate with size, matching the real symptom: a
    /// skewed hash distribution can put the biggest bucket anywhere in the id range.
    auto order = Aggregator::sortBucketsByRowCountDescending({{0, 10}, {1, 1000}, {2, 500}, {3, 1}});
    EXPECT_EQ(order, (std::vector<Int32>{1, 2, 0, 3}));
}

TEST(AggregatorBucketMergeOrder, AllBucketsPreservedExactlyOnceOnTies)
{
    auto order = Aggregator::sortBucketsByRowCountDescending({{0, 50}, {1, 50}, {2, 200}});
    ASSERT_EQ(order.size(), 3u);
    /// The strictly largest bucket must be dispatched first.
    EXPECT_EQ(order[0], 2);
    /// The tied pair can come back in either order, but both must be present.
    std::vector<Int32> tied{order[1], order[2]};
    std::sort(tied.begin(), tied.end());
    EXPECT_EQ(tied, (std::vector<Int32>{0, 1}));
}
