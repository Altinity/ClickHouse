#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace DB::Cas;

namespace
{

RefCommittedRow row(uint64_t epoch, uint64_t seq, uint32_t ordinal, String payload = "")
{
    RefCommittedRow r;
    r.manifest_ref = ManifestRef{epoch, seq, ordinal};
    r.payload = payload;
    return r;
}

}

/// ===================================================================================
/// Keyed ops (spec 2026-07-17-cas-reftable-cow-map-design.md §Mechanism)
/// ===================================================================================

TEST(CasRefCowMap, EmptyMapHasNoEntries)
{
    RefCowMap m;
    EXPECT_TRUE(m.empty());
    EXPECT_EQ(m.size(), 0u);
    EXPECT_FALSE(m.contains("a"));
    EXPECT_TRUE(m.find("a") == m.end());
}

TEST(CasRefCowMap, EmplaceThenFind)
{
    RefCowMap m;
    const auto [it, inserted] = m.emplace("a", row(1, 1, 1));
    EXPECT_TRUE(inserted);
    EXPECT_EQ(m.size(), 1u);
    ASSERT_TRUE(m.contains("a"));
    EXPECT_EQ(it->second.manifest_ref, (ManifestRef{1, 1, 1}));
    EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{1, 1, 1}));
}

TEST(CasRefCowMap, EmplaceDoesNotOverwriteExisting)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    const auto [it, inserted] = m.emplace("a", row(2, 2, 2));
    EXPECT_FALSE(inserted);
    EXPECT_EQ(it->second.manifest_ref, (ManifestRef{1, 1, 1}));
    EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{1, 1, 1}));   /// unchanged
}

TEST(CasRefCowMap, InsertOrAssignOverwritesExisting)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    const auto [it, inserted] = m.insert_or_assign("a", row(2, 2, 2));
    EXPECT_FALSE(inserted);
    EXPECT_EQ(it->second.manifest_ref, (ManifestRef{2, 2, 2}));
    EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{2, 2, 2}));
}

TEST(CasRefCowMap, InsertOrAssignInsertsWhenAbsent)
{
    RefCowMap m;
    const auto [it, inserted] = m.insert_or_assign("a", row(1, 1, 1));
    EXPECT_TRUE(inserted);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(it->second.manifest_ref, (ManifestRef{1, 1, 1}));
}

TEST(CasRefCowMap, EraseByKey)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    EXPECT_EQ(m.erase("a"), 1u);
    EXPECT_FALSE(m.contains("a"));
    EXPECT_EQ(m.size(), 0u);
    EXPECT_EQ(m.erase("a"), 0u);              /// already gone: no-op
    EXPECT_EQ(m.erase("nonexistent"), 0u);
}

TEST(CasRefCowMap, AtThrowsOnMissingKey)
{
    RefCowMap m;
    EXPECT_THROW(m.at("missing"), std::out_of_range);
}

TEST(CasRefCowMap, CountMatchesContains)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    EXPECT_EQ(m.count("a"), 1u);
    EXPECT_EQ(m.count("b"), 0u);
}

/// ===================================================================================
/// Ordered iteration -- overlay overrides/tombstones a materialized base (spec: "Ordered
/// iteration: merge-iterate base and overlay ... a standard two-sorted-range merge").
/// ===================================================================================

TEST(CasRefCowMap, OrderedIterationOverAllBaseRowsIsSorted)
{
    RefCowMap m;
    m.emplace("c", row(1, 3, 1));
    m.emplace("a", row(1, 1, 1));
    m.emplace("b", row(1, 2, 1));

    std::vector<String> names;
    for (const auto [name, r] : m)
        names.push_back(name);
    EXPECT_EQ(names, (std::vector<String>{"a", "b", "c"}));
}

TEST(CasRefCowMap, MergedIterationAppliesTombstonesAndOverrides)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    m.emplace("b", row(1, 2, 1));
    m.emplace("c", row(1, 3, 1));
    m.materialize();   /// a, b, c now live in `base`

    m.insert_or_assign("b", row(9, 9, 9));   /// override b via the overlay
    m.erase("c");                             /// tombstone c via the overlay
    m.emplace("d", row(9, 9, 2));             /// pure-overlay addition (not in base)

    std::vector<std::pair<String, ManifestRef>> seen;
    for (const auto [name, r] : m)
        seen.emplace_back(name, r.manifest_ref);

    const std::vector<std::pair<String, ManifestRef>> expected = {
        {"a", ManifestRef{1, 1, 1}},
        {"b", ManifestRef{9, 9, 9}},
        {"d", ManifestRef{9, 9, 2}},
    };
    EXPECT_EQ(seen, expected);
    EXPECT_EQ(m.size(), 3u);
}

TEST(CasRefCowMap, EraseByIteratorReturnsNextAndRemovesTheRow)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    m.emplace("b", row(1, 2, 1));
    m.emplace("c", row(1, 3, 1));

    auto it = m.find("b");
    ASSERT_TRUE(it != m.end());
    auto next = m.erase(it);
    ASSERT_TRUE(next != m.end());
    EXPECT_EQ(next->first, "c");
    EXPECT_FALSE(m.contains("b"));
    EXPECT_EQ(m.size(), 2u);
}

TEST(CasRefCowMap, EraseByIteratorOfLastElementReturnsEnd)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    auto it = m.find("a");
    auto next = m.erase(it);
    EXPECT_TRUE(next == m.end());
    EXPECT_TRUE(m.empty());
}
