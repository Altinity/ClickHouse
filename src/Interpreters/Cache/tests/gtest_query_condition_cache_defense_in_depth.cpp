/// These tests exercise the defense-in-depth paths in QueryConditionCache.cpp (the log-and-skip
/// checks kept alongside the Key fix). They cannot currently trigger a *genuine* Key-hash collision
/// (SipHash64 over table_id + part_name + condition_hash + marks_count + has_final_mark is not
/// practically collidable in a unit test), so "mismatch" here is necessarily synthetic. These tests
/// document and lock in the *behavior* (log + skip, no throw, no OOB access) rather than proving the
/// mismatch is unreachable in production -- that claim rests on the Key change itself, not on these
/// tests.

#include <Interpreters/Cache/QueryConditionCache.h>
#include <base/UUID.h>
#include <base/unit.h>
#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace DB;

/// A part_name reused with a completely different mark count (simulating the part-lifecycle
/// scenario this issue hypothesizes -- e.g. a part name recycled after data changed underneath it)
/// must not throw and must not corrupt either entry. With the Key fix this is definitionally two
/// separate keys, so this mostly re-confirms the same property as the test above from a different
/// angle (larger layout difference, has_final_mark also varied).
TEST(QueryConditionCache, ReusedPartNameWithFinalMarkDoesNotThrowOrCorrupt)
{
    QueryConditionCache cache("LRU", 1_MiB, 0.5);
    const UUID table_id(1);
    const String part_name = "reused_part";
    constexpr UInt64 condition_hash = 7;

    MarkRanges small_ranges;
    small_ranges.emplace_back(0, 3);
    EXPECT_NO_THROW(cache.write(table_id, part_name, condition_hash, "", small_ranges, 3, false));

    /// Simulates the same part_name later associated with a much larger, differently-shaped part
    /// (e.g. after a merge or mutation), including a final mark this time.
    MarkRanges large_ranges;
    large_ranges.emplace_back(0, 50);
    large_ranges.emplace_back(60, 99);
    EXPECT_NO_THROW(cache.write(table_id, part_name, condition_hash, "", large_ranges, 100, true));

    auto small_layout = cache.read(table_id, part_name, condition_hash, 3, false);
    ASSERT_TRUE(small_layout);
    ASSERT_EQ(small_layout->size(), 3);
    /// small_ranges = [0, 3) covers the whole entry, so every mark must have been forced to false
    /// ("must scan"); a corrupted entry (e.g. bleeding over from the differently-shaped large write)
    /// could instead leave marks at their initial true default or some other value.
    EXPECT_FALSE((*small_layout)[0]);
    EXPECT_FALSE((*small_layout)[1]);
    EXPECT_FALSE((*small_layout)[2]);

    auto large_layout = cache.read(table_id, part_name, condition_hash, 100, true);
    ASSERT_TRUE(large_layout);
    ASSERT_EQ(large_layout->size(), 100);
    /// has_final_mark=true means marks_count-1 (index 99) must be forced to false (i.e. "must scan").
    EXPECT_FALSE((*large_layout)[99]);
}

/// Concurrent-scan case referenced in the Entry class comment (*): multiple threads writing
/// overlapping/adjacent ranges for the same key must not race or corrupt the entry. This does not
/// specifically target #2342, but is adjacent: the ticket raises prefer_localhost_replica's effect
/// on crash rate as unexplained by the layout-mismatch fix alone, and increased local-read
/// concurrency against this cache is one candidate mechanism worth having coverage for.
TEST(QueryConditionCache, ConcurrentWritesToSameKeyDoNotCorruptEntry)
{
    QueryConditionCache cache("LRU", 1_MiB, 0.5);
    const UUID table_id(2);
    const String part_name = "concurrent_part";
    constexpr UInt64 condition_hash = 99;
    constexpr size_t marks_count = 1000;

    std::vector<std::thread> threads;
    for (size_t t = 0; t < 8; ++t)
    {
        threads.emplace_back([&, t]()
        {
            MarkRanges ranges;
            ranges.emplace_back(t * 100, (t + 1) * 100);
            cache.write(table_id, part_name, condition_hash, "", ranges, marks_count, false);
        });
    }
    for (auto & th : threads)
        th.join();

    auto result = cache.read(table_id, part_name, condition_hash, marks_count, false);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), marks_count);
    for (size_t i = 0; i < 800; ++i)
        EXPECT_FALSE((*result)[i]) << "mark " << i << " should have been marked non-matching";
}
