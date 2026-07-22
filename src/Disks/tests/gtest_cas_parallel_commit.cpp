#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/tests/cas_test_helpers.h>
#include <fmt/format.h>
#include <gtest/gtest.h>

/// Task 2 of the CAS parallel-write-path plan (docs/superpowers/sdd): `promoteBuild`/`repointRef`
/// return an exact, in-lane-derived `Cas::CommitOutcome` instead of `void`/`bool`, and
/// `dropRefIfMatches` gives a future rollback a conditional drop keyed on that exact outcome instead
/// of the unsafe-under-concurrency `dropRef` (which removes whatever manifest currently occupies the
/// ref name). This suite grows across the later parallel-commit tasks; here it only proves the
/// outcome is exact and that the conditional drop is a true guard -- still single-threaded commit, no
/// concurrency yet.

using namespace DB;
using namespace DB::Cas::tests;

namespace
{

/// Fixture mirroring `gtest_cas_part_folder_access.cpp`'s `publishPart`/`cacheOn` helpers: a fresh
/// in-memory pool + a `CachedPartFolderAccess` facade over it, plus the minimal staging helpers this
/// suite's tests need (stage a simple one-file part without promoting it; stage-and-promote it in one
/// call; repoint an already-committed ref onto a fresh manifest, modeling a later writer).
struct CaWiringFixture
{
    std::shared_ptr<Cas::InMemoryBackend> backend = std::make_shared<Cas::InMemoryBackend>();
    Cas::PoolPtr store = openPoolForTest(backend);
    Cas::CachedPartFolderAccess access{store};
    Cas::RootNamespace namespace_{"srv/t1"};
    int content_counter = 0;

    const Cas::RootNamespace & ns() const { return namespace_; }
    Cas::CachedPartFolderAccess & partAccess() { return access; }

    static Cas::ManifestEntry inlineEntry(const String & path, const String & bytes)
    {
        Cas::ManifestEntry e;
        e.path = path;
        e.placement = Cas::EntryPlacement::Inline;
        e.ref = Cas::BlobRef{Cas::BlobHashAlgo::CityHash128, Cas::BlobDigest::fromU128(u128Of(bytes))};
        e.blob_size = bytes.size();
        e.inline_bytes = bytes;
        return e;
    }

    struct Staged
    {
        Cas::PartWriteTxnPtr build;
        Cas::ManifestId id;
    };

    /// Stages a fresh build (manifest + precommit) for `key` over `blobs` inline entries, WITHOUT
    /// promoting it -- the caller drives `promoteBuild` itself so it can observe the exact
    /// `CommitOutcome` the promote primitive derives.
    Staged stageSimplePart(const Cas::PartRefKey & key, int blobs)
    {
        std::vector<Cas::ManifestEntry> entries;
        for (int i = 0; i < blobs; ++i)
            entries.push_back(inlineEntry(fmt::format("f{}", i), fmt::format("payload-{}-{}", key.ref, i)));
        auto build = store->beginPartWrite(Cas::PartWriteInfo{
            .intended_ref = key.ns.string() + "/" + key.ref, .intended_namespace = key.ns, .op = Cas::ProvenanceOp::Insert});
        const Cas::ManifestId id = build->stageManifest(entries);
        build->precommitAdd(key.ns, key.ref, id);
        return {std::move(build), id};
    }

    /// Stages and promotes one simple part end-to-end, returning the exact `CommitOutcome`.
    Cas::CommitOutcome commitSimplePart(const Cas::PartRefKey & key, int blobs)
    {
        auto staged = stageSimplePart(key, blobs);
        return access.promoteBuild(*staged.build, key, staged.build->buildId(), staged.id);
    }

    /// Repoints an already-committed `key` onto a fresh manifest (different content), through the
    /// public `repointRef` primitive -- models "another writer" rebinding the ref after this
    /// fixture's own `commitSimplePart`.
    Cas::CommitOutcome repointToFreshManifest(const Cas::PartRefKey & key)
    {
        return access.repointRef(key, {inlineEntry("f0", fmt::format("repoint-{}", ++content_counter))},
            Cas::ProvenanceOp::Other);
    }
};

}

TEST(CasCommitOutcome, PromoteReportsCreatedAndManifest)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_1_1_0"};
    auto staged = fx.stageSimplePart(key, /*blobs=*/1);

    const Cas::CommitOutcome oc = fx.partAccess().promoteBuild(*staged.build, key, staged.build->buildId(), staged.id);

    EXPECT_TRUE(oc.created);
    EXPECT_EQ(oc.ns.string(), key.ns.string());
    EXPECT_EQ(oc.ref, key.ref);
    EXPECT_EQ(oc.manifest_ref, staged.id.ref);
}

TEST(CasCommitOutcome, DropRefIfMatchesRemovesOnlyExact)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_2_2_0"};
    const Cas::CommitOutcome oc1 = fx.commitSimplePart(key, /*blobs=*/1);
    EXPECT_TRUE(oc1.created);

    /// Rebind key -> M2 (a legitimate repoint by "another writer").
    const Cas::CommitOutcome oc2 = fx.repointToFreshManifest(key);
    EXPECT_FALSE(oc2.created);
    ASSERT_NE(oc1.manifest_ref, oc2.manifest_ref);

    /// Conditional drop keyed on the STALE M1 must NOT remove the current M2 binding.
    EXPECT_FALSE(fx.partAccess().dropRefIfMatches(key, oc1.manifest_ref));
    EXPECT_TRUE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));

    /// Conditional drop keyed on the CURRENT M2 removes it.
    EXPECT_TRUE(fx.partAccess().dropRefIfMatches(key, oc2.manifest_ref));
    EXPECT_FALSE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
}

TEST(CasCommitOutcome, DropRefIfMatchesOnAbsentRefIsANoOp)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_3_3_0"};
    Cas::ManifestRef bogus;
    EXPECT_FALSE(fx.partAccess().dropRefIfMatches(key, bogus)) << "no committed ref at all: nothing to match";
    EXPECT_FALSE(fx.partAccess().existsRef(key, Cas::Freshness::ForceFresh));
}

/// `repointRef`'s byte-equal candidate is a documented ZERO-pool-mutation no-op (it must not mint a
/// fresh manifest just to compare it). The returned `CommitOutcome` must still describe reality: the
/// CURRENTLY committed manifest, unchanged, `created=false`.
TEST(CasCommitOutcome, RepointRefByteEqualNoOpReportsCurrentManifestNotCreated)
{
    CaWiringFixture fx;
    const Cas::PartRefKey key{fx.ns(), "20260101_4_4_0"};
    const Cas::CommitOutcome oc1 = fx.commitSimplePart(key, /*blobs=*/1);

    const Cas::CommitOutcome oc_noop = fx.partAccess().repointRef(
        key, {CaWiringFixture::inlineEntry("f0", fmt::format("payload-{}-0", key.ref))}, Cas::ProvenanceOp::Other);
    EXPECT_FALSE(oc_noop.created);
    EXPECT_EQ(oc_noop.manifest_ref, oc1.manifest_ref);
}
