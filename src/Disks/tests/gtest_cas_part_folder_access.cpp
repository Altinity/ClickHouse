#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/tests/cas_test_helpers.h>
#include <gtest/gtest.h>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ABORTED;
}

using namespace DB;
using namespace DB::Cas::tests;

namespace
{

Cas::ManifestEntry inlineEntry(const String & path, const String & bytes)
{
    Cas::ManifestEntry e;
    e.path = path;
    e.placement = Cas::EntryPlacement::Inline;
    e.blob_hash = u128Of(bytes);
    e.blob_size = bytes.size();
    e.inline_bytes = bytes;
    return e;
}

/// Publish `entries` as committed ref `ns/ref` through the real writer protocol.
Cas::ManifestId publishPart(const Cas::StorePtr & store, const Cas::RootNamespace & ns,
                            const String & ref, std::vector<Cas::ManifestEntry> entries,
                            std::map<String, String> mutable_files = {})
{
    auto build = store->startBuild(Cas::BuildInfo{.intended_ref = ns.string() + "/" + ref,
                                                  .intended_namespace = ns, .op = Cas::ProvenanceOp::Insert});
    const Cas::ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, ref, id);
    build->setPendingMutableFiles(std::move(mutable_files));
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

}

TEST(CasPartFolderAccess, GetViewServesCommittedFolder)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs"), inlineEntry("count.txt", "1")},
                {{"txn_version.txt", "v1"}});

    ContentAddressed::CachedPartFolderAccess access(store);
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("checksums.txt"), nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v1"));

    /// Absent ref => nullptr, never an exception, never retained (nothing to retain in Phase 2).
    EXPECT_EQ(access.getView({ns, "absent"}, ContentAddressed::Freshness::CachedForLoad), nullptr);
    EXPECT_TRUE(access.existsRef(key, ContentAddressed::Freshness::CachedForLoad));
    EXPECT_FALSE(access.existsRef({ns, "absent"}, ContentAddressed::Freshness::ForceFresh));
    ASSERT_TRUE(access.resolve(key, ContentAddressed::Freshness::ForceFresh).has_value());
}

TEST(CasPartFolderAccess, GetViewFailsClosedOnMissingBody)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    /// Physically delete the live manifest body (a protocol violation) — every getView mode must
    /// surface INV-NO-DANGLE as FILE_DOESNT_EXIST in Phase 2 (there is no retained view to hit).
    deleteManifestBody(*backend, layout, id);

    ContentAddressed::CachedPartFolderAccess access(store);
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    for (auto freshness : {ContentAddressed::Freshness::CachedForLoad,
                           ContentAddressed::Freshness::ForceFresh,
                           ContentAddressed::Freshness::StrictValidate})
        expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST, [&] { access.getView(key, freshness); });
}

TEST(CasPartFolderAccess, WritePrimitivesRoundTrip)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    /// promoteBuild: the transaction's terminal publish step, through the facade.
    auto build = store->startBuild(Cas::BuildInfo{.intended_ref = ns.string() + "/part_1",
                                                  .intended_namespace = ns, .op = Cas::ProvenanceOp::Insert});
    const Cas::ManifestId id = build->stageManifest({inlineEntry("checksums.txt", "cs")});
    build->precommitAdd(ns, "part_1", id);
    access.promoteBuild(*build, key, build->buildId(), id, {{"txn_version.txt", "v1"}});
    ASSERT_TRUE(access.existsRef(key, ContentAddressed::Freshness::ForceFresh));

    /// updateMutableFiles is visible to a force-fresh resolve immediately.
    access.updateMutableFiles(key, [](Cas::RootRef & payload) { payload.mutable_files["txn_version.txt"] = "v2"; });
    auto resolved = access.resolve(key, ContentAddressed::Freshness::ForceFresh);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->mutable_files.at("txn_version.txt"), "v2");

    /// dropRefIfPresent: replay-safe (absent ref is success, not failure).
    access.dropRefIfPresent(key);
    EXPECT_FALSE(access.existsRef(key, ContentAddressed::Freshness::ForceFresh));
    access.dropRefIfPresent(key);                              /// second drop: no-op, no throw
    access.dropRefBestEffort(key);                             /// noexcept even when absent

    /// dropNamespace clears the whole namespace.
    publishPart(store, ns, "part_2", {inlineEntry("checksums.txt", "cs")});
    access.dropNamespace(ns);
    EXPECT_FALSE(access.existsRef({ns, "part_2"}, ContentAddressed::Freshness::ForceFresh));
}
