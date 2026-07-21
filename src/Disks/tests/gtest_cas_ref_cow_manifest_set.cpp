#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowManifestSet.h>

using namespace DB::Cas;

namespace
{

ManifestRef mref(uint64_t epoch, uint64_t seq, uint32_t ordinal)
{
    return ManifestRef{epoch, seq, ordinal};
}

}

/// ===================================================================================
/// Keyed ops: contains/insert/erase across base+overlay (spec:
/// docs/superpowers/reports/2026-07-21-reftablestate-experiments.md "E2 owned-manifest index").
/// ===================================================================================

TEST(CasRefCowManifestSet, EmptySetHasNoMembers)
{
    RefCowManifestSet s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_FALSE(s.contains(mref(1, 1, 1)));
}

TEST(CasRefCowManifestSet, InsertThenContains)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_EQ(s.size(), 1u);
    EXPECT_FALSE(s.contains(mref(2, 2, 2)));
}

TEST(CasRefCowManifestSet, InsertMultipleThenContainsEachIndependently)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.insert(mref(1, 1, 2));
    s.insert(mref(2, 1, 1));
    EXPECT_EQ(s.size(), 3u);
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_TRUE(s.contains(mref(1, 1, 2)));
    EXPECT_TRUE(s.contains(mref(2, 1, 1)));
    EXPECT_FALSE(s.contains(mref(3, 3, 3)));
}

TEST(CasRefCowManifestSet, EraseRemovesAnOverlayOnlyMember)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.erase(mref(1, 1, 1));
    EXPECT_FALSE(s.contains(mref(1, 1, 1)));
    EXPECT_EQ(s.size(), 0u);
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.overlayEntriesForTest(), 0u);   /// pure-overlay member: erase removes it outright
}

TEST(CasRefCowManifestSet, TombstoneThenReinsertWhilePurelyInOverlay)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.erase(mref(1, 1, 1));
    s.insert(mref(1, 1, 1));   /// re-insert -- must not be treated as "still present"
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_EQ(s.size(), 1u);
}

/// ===================================================================================
/// materialize()
/// ===================================================================================

TEST(CasRefCowManifestSet, MaterializeFoldsOverlayIntoBaseAndEmptiesOverlay)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.insert(mref(1, 1, 2));
    EXPECT_GT(s.overlayEntriesForTest(), 0u);

    s.materialize();
    EXPECT_EQ(s.overlayEntriesForTest(), 0u);
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_TRUE(s.contains(mref(1, 1, 2)));
    EXPECT_EQ(s.size(), 2u);
}

TEST(CasRefCowManifestSet, MaterializeOnAnEmptyOverlayIsANoOp)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.materialize();
    const int64_t use_count_before = s.baseUseCountForTest();
    s.materialize();   /// overlay is already empty
    EXPECT_EQ(s.baseUseCountForTest(), use_count_before);
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
}

TEST(CasRefCowManifestSet, EraseAfterMaterializeTombstonesABaseMember)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.insert(mref(1, 1, 2));
    s.materialize();   /// both now live in `base`

    s.erase(mref(1, 1, 1));
    EXPECT_FALSE(s.contains(mref(1, 1, 1)));
    EXPECT_TRUE(s.contains(mref(1, 1, 2)));
    EXPECT_EQ(s.size(), 1u);

    s.materialize();   /// tombstone folds away; base member actually removed
    EXPECT_FALSE(s.contains(mref(1, 1, 1)));
    EXPECT_EQ(s.size(), 1u);
}

TEST(CasRefCowManifestSet, TombstoneThenReinsertAcrossMaterializedBase)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.materialize();   /// mref(1,1,1) now lives in `base`

    s.erase(mref(1, 1, 1));            /// tombstone shadowing the base member
    s.insert(mref(1, 1, 1));           /// revive the tombstone -- must read as present again
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_EQ(s.size(), 1u);

    s.materialize();
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_EQ(s.size(), 1u);
}

/// ===================================================================================
/// Copy-on-write isolation + O(1)-copy assertion.
/// ===================================================================================

TEST(CasRefCowManifestSet, CopyIsIsolatedFromOriginal)
{
    RefCowManifestSet original;
    original.insert(mref(1, 1, 1));
    original.materialize();

    RefCowManifestSet copy = original;
    copy.insert(mref(9, 9, 9));
    copy.erase(mref(1, 1, 1));

    EXPECT_TRUE(original.contains(mref(1, 1, 1)));
    EXPECT_FALSE(original.contains(mref(9, 9, 9)));

    EXPECT_FALSE(copy.contains(mref(1, 1, 1)));
    EXPECT_TRUE(copy.contains(mref(9, 9, 9)));
}

TEST(CasRefCowManifestSet, CopySharesBaseUntilEitherSideMaterializesANewOne)
{
    RefCowManifestSet original;
    original.insert(mref(1, 1, 1));
    original.materialize();

    RefCowManifestSet copy = original;
    /// A copy shares the SAME base object (refcount bump, no per-element allocation) until a write
    /// forces a new base into existence via `materialize()`.
    EXPECT_EQ(original.baseUseCountForTest(), 2);
    EXPECT_EQ(copy.baseUseCountForTest(), 2);

    copy.insert(mref(2, 2, 2));   /// writes go to `copy`'s overlay; `base` is untouched
    EXPECT_EQ(original.baseUseCountForTest(), 2);
    EXPECT_EQ(copy.baseUseCountForTest(), 2);
    EXPECT_FALSE(original.contains(mref(2, 2, 2)));

    copy.materialize();   /// NOW `copy` points at a fresh base of its own
    EXPECT_EQ(original.baseUseCountForTest(), 1);
    EXPECT_EQ(copy.baseUseCountForTest(), 1);
}

/// ===================================================================================
/// size()/net_delta correctness across a longer op sequence, mixing base and overlay changes.
/// ===================================================================================

TEST(CasRefCowManifestSet, SizeTracksNetDeltaAcrossMixedOps)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.insert(mref(1, 1, 2));
    s.insert(mref(1, 1, 3));
    EXPECT_EQ(s.size(), 3u);
    s.materialize();
    EXPECT_EQ(s.size(), 3u);

    s.erase(mref(1, 1, 2));         /// base member removed via overlay tombstone
    EXPECT_EQ(s.size(), 2u);
    s.insert(mref(1, 1, 4));        /// pure-overlay addition
    EXPECT_EQ(s.size(), 3u);
    s.erase(mref(1, 1, 4));         /// pure-overlay addition removed outright
    EXPECT_EQ(s.size(), 2u);
    s.insert(mref(1, 1, 2));        /// revive the earlier tombstone
    EXPECT_EQ(s.size(), 3u);

    s.materialize();
    EXPECT_EQ(s.size(), 3u);
    EXPECT_TRUE(s.contains(mref(1, 1, 1)));
    EXPECT_TRUE(s.contains(mref(1, 1, 2)));
    EXPECT_TRUE(s.contains(mref(1, 1, 3)));
    EXPECT_FALSE(s.contains(mref(1, 1, 4)));
}

/// ===================================================================================
/// Chasserted misuse (debug/sanitizer builds only): `insert` requires absence, `erase` requires
/// presence -- the uniqueness invariant the ref table enforces before ever calling either, so a
/// violation here means the index has drifted, not that a legitimate caller can trigger it.
/// ===================================================================================

#if defined(DEBUG_OR_SANITIZER_BUILD)

TEST(CasRefCowManifestSetDeathTest, InsertAbortsWhenAlreadyPresentInOverlay)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    EXPECT_DEATH({ s.insert(mref(1, 1, 1)); }, "");
}

TEST(CasRefCowManifestSetDeathTest, InsertAbortsWhenAlreadyPresentInBase)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.materialize();
    EXPECT_DEATH({ s.insert(mref(1, 1, 1)); }, "");
}

TEST(CasRefCowManifestSetDeathTest, EraseAbortsWhenAbsent)
{
    RefCowManifestSet s;
    EXPECT_DEATH({ s.erase(mref(1, 1, 1)); }, "");
}

TEST(CasRefCowManifestSetDeathTest, EraseAbortsWhenAlreadyTombstoned)
{
    RefCowManifestSet s;
    s.insert(mref(1, 1, 1));
    s.materialize();
    s.erase(mref(1, 1, 1));
    EXPECT_DEATH({ s.erase(mref(1, 1, 1)); }, "");
}

#endif
