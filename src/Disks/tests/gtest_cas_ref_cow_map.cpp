#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>

#include <map>
#include <random>
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
    EXPECT_FALSE(m.contains("a"));
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

/// ===================================================================================
/// materialize() (spec §Materialization)
/// ===================================================================================

TEST(CasRefCowMap, MaterializeFoldsOverlayIntoFreshBaseAndKeepsValuesUnchanged)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    m.emplace("b", row(1, 2, 1));
    m.erase("a");
    EXPECT_GT(m.overlayEntriesForTest(), 0u);

    m.materialize();
    EXPECT_EQ(m.overlayEntriesForTest(), 0u);
    EXPECT_FALSE(m.contains("a"));
    ASSERT_TRUE(m.contains("b"));
    EXPECT_EQ(m.at("b").manifest_ref, (ManifestRef{1, 2, 1}));
    EXPECT_EQ(m.size(), 1u);
}

TEST(CasRefCowMap, MaterializeOnAnEmptyOverlayIsANoOp)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    m.materialize();
    const int64_t use_count_before = m.baseUseCountForTest();
    m.materialize();   /// overlay is already empty
    EXPECT_EQ(m.baseUseCountForTest(), use_count_before);
    EXPECT_TRUE(m.contains("a"));
}

TEST(CasRefCowMap, MaterializeDoesNotAffectACopyTakenBeforeIt)
{
    RefCowMap m;
    m.emplace("a", row(1, 1, 1));
    RefCowMap snapshot_before = m;   /// copy shares m's pre-materialize base, owns its own overlay
    m.insert_or_assign("a", row(2, 2, 2));
    m.materialize();

    EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{2, 2, 2}));
    EXPECT_EQ(snapshot_before.at("a").manifest_ref, (ManifestRef{1, 1, 1}));
}

TEST(CasRefCowMap, EqualityComparesEffectiveContentsNotInternalLayout)
{
    RefCowMap a;
    a.emplace("x", row(1, 1, 1));
    a.materialize();   /// "x" lives in `base`

    RefCowMap b;
    b.emplace("x", row(1, 1, 1));   /// same logical content, but lives entirely in `overlay`

    EXPECT_EQ(a.overlayEntriesForTest(), 0u);
    EXPECT_GT(b.overlayEntriesForTest(), 0u);
    EXPECT_TRUE(a == b);
}

/// ===================================================================================
/// Copy-on-write isolation + O(1)-copy assertion (spec §Correctness & testing)
/// ===================================================================================

TEST(CasRefCowMap, CopyIsIsolatedFromOriginal)
{
    RefCowMap original;
    original.emplace("a", row(1, 1, 1));
    original.materialize();

    RefCowMap copy = original;
    copy.insert_or_assign("a", row(9, 9, 9));
    copy.emplace("b", row(9, 9, 9));

    EXPECT_EQ(original.at("a").manifest_ref, (ManifestRef{1, 1, 1}));
    EXPECT_FALSE(original.contains("b"));

    EXPECT_EQ(copy.at("a").manifest_ref, (ManifestRef{9, 9, 9}));
    EXPECT_TRUE(copy.contains("b"));
}

TEST(CasRefCowMap, CopySharesBaseUntilEitherSideMaterializesANewOne)
{
    RefCowMap original;
    original.emplace("a", row(1, 1, 1));
    original.materialize();

    RefCowMap copy = original;
    /// A copy shares the SAME base object (refcount bump, no per-row allocation) until a write
    /// forces a new base into existence via `materialize()` (spec §Mechanism: "Copy = O(1)").
    EXPECT_EQ(original.baseUseCountForTest(), 2);
    EXPECT_EQ(copy.baseUseCountForTest(), 2);

    copy.insert_or_assign("a", row(2, 2, 2));   /// writes go to `copy`'s overlay; `base` is untouched
    EXPECT_EQ(original.baseUseCountForTest(), 2);
    EXPECT_EQ(copy.baseUseCountForTest(), 2);

    copy.materialize();   /// NOW `copy` points at a fresh base of its own
    EXPECT_EQ(original.baseUseCountForTest(), 1);
    EXPECT_EQ(copy.baseUseCountForTest(), 1);
}

/// ===================================================================================
/// Randomized exactness property test: RefCowMap must behave IDENTICALLY to
/// std::map<String, RefCommittedRow> across randomized op sequences (spec §Correctness &
/// testing: "random op sequences ... including copy-then-mutate isolation ... and
/// tombstone/override correctness on the merged iterator").
/// ===================================================================================

TEST(CasRefCowMap, PropertyMatchesStdMapOverRandomOps)
{
    std::mt19937 rng(20260717); // NOLINT(cert-msc): deterministic seed is required for reproducible property coverage.

    for (int trial = 0; trial < 50; ++trial)
    {
        RefCowMap actual;
        std::map<String, RefCommittedRow> oracle;

        for (int step = 0; step < 200; ++step)
        {
            const String key = "ref" + std::to_string(rng() % 12);
            const uint32_t action = rng() % 6;
            switch (action)
            {
                case 0:   /// emplace
                {
                    RefCommittedRow r = row(1, static_cast<uint64_t>(step) + 1, 1);
                    const bool oracle_inserted = oracle.emplace(key, r).second;
                    const bool actual_inserted = actual.emplace(key, r).second;
                    EXPECT_EQ(oracle_inserted, actual_inserted) << "trial " << trial << " step " << step;
                    break;
                }
                case 1:   /// insert_or_assign
                {
                    RefCommittedRow r = row(2, static_cast<uint64_t>(step) + 1, 2);
                    oracle[key] = r;
                    actual.insert_or_assign(key, r);
                    break;
                }
                case 2:   /// erase by key
                {
                    const size_t oracle_erased = oracle.erase(key);
                    const size_t actual_erased = actual.erase(key);
                    EXPECT_EQ(oracle_erased, actual_erased) << "trial " << trial << " step " << step;
                    break;
                }
                case 3:   /// find/contains/at (read-only)
                {
                    EXPECT_EQ(oracle.contains(key), actual.contains(key)) << "trial " << trial << " step " << step;
                    if (oracle.contains(key))
                        EXPECT_EQ(oracle.at(key), actual.at(key)) << "trial " << trial << " step " << step;
                    break;
                }
                case 4:   /// erase via a found iterator
                {
                    if (auto it = actual.find(key); it != actual.end())
                    {
                        oracle.erase(key);
                        actual.erase(it);
                    }
                    break;
                }
                case 5:   /// materialize -- must not change observable content
                {
                    actual.materialize();
                    break;
                }
                default:
                    UNREACHABLE();
            }

            ASSERT_EQ(oracle.size(), actual.size()) << "trial " << trial << " step " << step;

            auto oit = oracle.begin();
            auto ait = actual.begin();
            for (; oit != oracle.end() && ait != actual.end(); ++oit, ++ait)
            {
                ASSERT_EQ(oit->first, ait->first) << "trial " << trial << " step " << step;
                ASSERT_EQ(oit->second, ait->second) << "trial " << trial << " step " << step;
            }
            ASSERT_TRUE(oit == oracle.end()) << "trial " << trial << " step " << step;
            ASSERT_TRUE(ait == actual.end()) << "trial " << trial << " step " << step;
        }
    }
}
