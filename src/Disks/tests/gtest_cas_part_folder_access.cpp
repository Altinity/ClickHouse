#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/tests/cas_test_helpers.h>
#include <gtest/gtest.h>
#include <latch>
#include <thread>

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
    e.ref = DB::Cas::BlobRef{DB::Cas::BlobHashAlgo::CityHash128, DB::Cas::BlobDigest::fromU128(u128Of(bytes))};

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

ContentAddressed::CachedPartFolderAccess::CacheParams cacheOn()
{
    return {.cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 16ULL << 20};
}

}

TEST(CasPartFolderAccess, RetainedHitSkipsManifestHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    for (int i = 0; i < 5; ++i)
        ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// The one-GET goal (spec acceptance 4): ONE body GET, ONE mandatory HEAD (the cold build);
    /// every subsequent CachedForLoad call is a validated hit — zero manifest ops.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), 1u);
    EXPECT_TRUE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Hit);
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
    access.updateMutableFiles(key, [](Cas::RefMutableFilesUpdate & payload) { payload.mutable_files["txn_version.txt"] = "v2"; });
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

TEST(CasPartFolderAccess, RepublishRefMovesCommittedRef)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    publishPart(store, ns, "src_part", {inlineEntry("checksums.txt", "cs")}, {{"txn_version.txt", "v1"}});

    EXPECT_FALSE(access.republishRef({ns, "absent"}, {ns, "dst"}));   /// absent source: nothing written

    ASSERT_TRUE(access.republishRef({ns, "src_part"}, {ns, "dst_part"}));
    EXPECT_FALSE(access.existsRef({ns, "src_part"}, ContentAddressed::Freshness::ForceFresh));
    auto view = access.getView({ns, "dst_part"}, ContentAddressed::Freshness::ForceFresh);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("checksums.txt"), nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v1"));   /// carried over
}

TEST(CasPartFolderAccess, RepublishRefIdempotentRedriveAndConflict)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);

    /// Re-drive: dst already committed with the SAME content (a prior attempt's promote landed,
    /// only dropRef(src) was interrupted), and src's mutable payload drifted afterwards.
    publishPart(store, ns, "src", {inlineEntry("f", "same")}, {{"txn_version.txt", "v2"}});
    publishPart(store, ns, "dst", {inlineEntry("f", "same")}, {{"txn_version.txt", "v1"}});
    ASSERT_TRUE(access.republishRef({ns, "src"}, {ns, "dst"}));
    EXPECT_FALSE(access.existsRef({ns, "src"}, ContentAddressed::Freshness::ForceFresh));
    auto resolved = access.resolve({ns, "dst"}, ContentAddressed::Freshness::ForceFresh);
    EXPECT_EQ(resolved->mutable_files.at("txn_version.txt"), "v2");   /// re-synced from src

    /// Conflict: dst committed with DIFFERENT content — fail closed, src untouched.
    publishPart(store, ns, "src2", {inlineEntry("f", "one")});
    publishPart(store, ns, "dst2", {inlineEntry("f", "two")});
    expectThrowsCode(ErrorCodes::ABORTED, [&] { access.republishRef({ns, "src2"}, {ns, "dst2"}); });
    EXPECT_TRUE(access.existsRef({ns, "src2"}, ContentAddressed::Freshness::ForceFresh));
}

TEST(CasPartFolderAccess, ExplainRecordsDecisions)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Miss);       /// cold build
    EXPECT_FALSE(access.explain(key).retained);                                    /// Phase 3: never

    access.getView(key, ContentAddressed::Freshness::ForceFresh);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::ForceFreshRead);

    access.getView(key, ContentAddressed::Freshness::StrictValidate);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::StrictBypass);

    access.dropRef(key);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Invalidated);
    EXPECT_GT(access.explain(key).estimated_bytes, 0u);
}

TEST(CasPartFolderAccess, BaselineRequestCountsWithoutRetention)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int n = 5;
    for (int i = 0; i < n; ++i)
        ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// The Phase-3 baseline (retention off): one manifest-body GET (the decode cache absorbs the
    /// rest) but a mandatory manifest HEAD per call. Phase 4's validated hits remove the HEADs;
    /// this test pins the numbers Phase 4 improves.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), static_cast<uint64_t>(n));
}

/// ==== Phase 4 (retention) semantics battery: spec §Testing acceptance criteria ====

TEST(CasPartFolderAccess, MutableRefreshWithoutManifestRead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")}, {{"txn_version.txt", "v1"}});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    /// RAW-store mutation: the write bypasses the facade (validate-on-hit must cope — this is the
    /// mutation-site-not-routed / foreign-writer shape the compare exists for). The retained entry
    /// survives, so the next read exercises the manifest-match + mutable-drift CLONE path.
    store->updateRefPayload(ns, "part_1", [](Cas::RefMutableFilesUpdate & p) { p.mutable_files["txn_version.txt"] = "v2"; });
    backend->resetCounts();

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v2"));   /// read-your-writes
    EXPECT_EQ(backend->headCount(manifest_key), 0u);                                 /// clone: no HEAD
    EXPECT_EQ(backend->getCount(manifest_key), 0u);                                  /// and no GET
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::MutableRefresh);
}

TEST(CasPartFolderAccess, WriteThroughEraseThenRebuild)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")}, {{"txn_version.txt", "v1"}});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);
    access.updateMutableFiles(key, [](Cas::RefMutableFilesUpdate & p) { p.mutable_files["txn_version.txt"] = "v2"; });
    backend->resetCounts();

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v2"));
    EXPECT_EQ(backend->headCount(manifest_key), 1u);   /// erase => cold rebuild re-HEADs...
    EXPECT_EQ(backend->getCount(manifest_key), 0u);    /// ...but the decode cache absorbs the GET
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Miss);
}

TEST(CasPartFolderAccess, MismatchRebuildAfterRepublish)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    publishPart(store, ns, "part_1", {inlineEntry("f", "orig")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained

    /// Drop + republish the SAME ref name with DIFFERENT content through the raw Core protocol (no
    /// facade => no write-through erase): the retained entry survives with a manifest_id that no
    /// longer resolves — the next CachedForLoad hits the manifest-changed compare (step 2c).
    store->dropRef(ns, "part_1");
    const auto id2 = publishPart(store, ns, "part_1", {inlineEntry("f", "DIFFERENT")});
    const String manifest_key2 = layout.manifestKey(id2);
    backend->resetCounts();

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("f"), nullptr);
    EXPECT_EQ(view->findFile("f")->inline_bytes, "DIFFERENT");    /// never the stale view
    EXPECT_EQ(backend->getCount(manifest_key2), 1u);              /// one new manifest GET
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Miss);   /// rebuilt, now retained
    EXPECT_TRUE(access.explain(key).retained);
}

TEST(CasPartFolderAccess, ForceFreshFailsClosedWhileRetainedViewExists)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    deleteManifestBody(*backend, layout, id);   /// protocol violation: live body vanishes

    /// Write-evidence and strict paths surface INV-NO-DANGLE immediately (mandatory HEAD)...
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, ContentAddressed::Freshness::ForceFresh); });
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, ContentAddressed::Freshness::StrictValidate); });

    /// ...while a validated CachedForLoad hit still serves the immutable decode — the documented
    /// residual delta (spec §Staleness Equivalence): detection deferred, never for write evidence.
    EXPECT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);
}

TEST(CasPartFolderAccess, AbsenceIsNeverRetained)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    access.dropRef(key);
    EXPECT_EQ(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// absent: nullptr, never retained

    /// Re-publish under the SAME ref name: immediately visible, no stale absence remembered.
    publishPart(store, ns, "part_1", {inlineEntry("f", "y")});
    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->inlineBytes("f"), std::optional<String>("y"));
}

TEST(CasPartFolderAccess, OversizedViewServedNotRetained)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    /// max_entry_bytes = 1: every real view (>= the 256-byte fixed overhead alone) is oversized.
    ContentAddressed::CachedPartFolderAccess access(store,
        ContentAddressed::CachedPartFolderAccess::CacheParams{
            .cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 1});
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    auto view1 = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view1, nullptr);
    EXPECT_FALSE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::OversizedBypass);

    const uint64_t head_before = backend->headCount(manifest_key);
    auto view2 = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view2, nullptr);
    EXPECT_GT(backend->headCount(manifest_key), head_before);   /// not retained: re-HEADs every call
    EXPECT_FALSE(access.explain(key).retained);
}

TEST(CasPartFolderAccess, DisabledModeKeepsBaseline)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    /// CacheParams{} (cache_bytes == 0): the explicit disable switch, same as the single-arg ctor.
    ContentAddressed::CachedPartFolderAccess access(store, ContentAddressed::CachedPartFolderAccess::CacheParams{});
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int n = 5;
    for (int i = 0; i < n; ++i)
        ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// Exactly the Phase-3 baseline: bytes=0 restores the no-retention call graph byte-for-byte.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), static_cast<uint64_t>(n));
    EXPECT_FALSE(access.explain(key).retained);
}

TEST(CasPartFolderAccess, SingleFlightColdBuild)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int k = 8;
    std::latch start_gate(k);
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<const ContentAddressed::PartFolderView>> results(k);
    for (int i = 0; i < k; ++i)
        threads.emplace_back([&, i]
        {
            start_gate.arrive_and_wait();
            results[i] = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
        });
    for (auto & t : threads)
        t.join();

    for (const auto & r : results)
        EXPECT_NE(r, nullptr);
    EXPECT_EQ(backend->getCount(manifest_key), 1u);   /// single-flight: ONE body GET for the burst
}

TEST(CasPartFolderAccess, DropNamespaceErasesAllViews)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    publishPart(store, ns, "part_2", {inlineEntry("f", "y")});
    const ContentAddressed::PartRefKey key1{ns, "part_1"};
    const ContentAddressed::PartRefKey key2{ns, "part_2"};

    ASSERT_NE(access.getView(key1, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    ASSERT_NE(access.getView(key2, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    EXPECT_TRUE(access.explain(key1).retained);
    EXPECT_TRUE(access.explain(key2).retained);

    access.dropNamespace(ns);

    /// dropNamespace removes the namespace via the ref-log `remove_namespace` transaction AND erases every
    /// cached view: the dropped entries must not masquerade as "retained", and no stale key1/key2 view may
    /// be served.
    EXPECT_FALSE(access.explain(key1).retained);
    EXPECT_FALSE(access.explain(key2).retained);   /// dropped too, even though never re-touched

    /// A fresh getView on the removed namespace is a COLD MISS (nullptr) -- never a stale hit on the
    /// dropped manifest. A residual retained entry would instead be served here without ever going through
    /// validate-on-hit, exactly the masquerade this guards against.
    EXPECT_EQ(access.getView(key1, ContentAddressed::Freshness::CachedForLoad), nullptr);
    EXPECT_EQ(access.getView(key2, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// Recreation end-to-end (Task 12, snapshot+log §Namespace Birth): recreating the namespace requires
    /// GC's `_cleanup/<remove_txn_id>` completion marker; a warm writer re-observes it via one exact-key
    /// re-check (`Store::observedNamespaceCleanupMarker`). This file has no GC harness, so we publish the
    /// marker directly -- the removal already published a `Removed` snapshot at `remove_txn_id`, so read
    /// that id and write the marker, exactly as GC's namespace-cleanup item would. Republishing part_1
    /// under the SAME name must then be admitted and serve a fresh RECREATED view via validate-on-hit,
    /// never a stale hit on the dropped manifest.
    const Cas::Layout & layout = store->layout();
    const Cas::ListPage removed_snaps = backend->list(layout.refsNamespacePrefix(ns) + "_snap/", "", 100);
    ASSERT_FALSE(removed_snaps.keys.empty()) << "dropNamespace must publish a Removed snapshot at remove_txn_id";
    const auto parsed = layout.parseRefObjectKey(removed_snaps.keys.front().key);
    ASSERT_TRUE(parsed.has_value());
    backend->putIfAbsent(layout.refCleanupMarkerKey(ns, parsed->txn_id), "");

    publishPart(store, ns, "part_1", {inlineEntry("f", "recreated")});
    const auto recreated_view = access.getView(key1, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(recreated_view, nullptr) << "the recreated namespace must serve a fresh view after the marker is durable";
    EXPECT_TRUE(access.explain(key1).retained);
}
