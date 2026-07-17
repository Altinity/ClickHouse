#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/ProfileEvents.h>
#include <Poco/Util/XMLConfiguration.h>
#include <gtest/gtest.h>
#include <latch>
#include <sstream>
#include <thread>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
}

namespace ProfileEvents
{
extern const Event CasRefRollbackBestEffortDropFailed;
extern const Event CasPartFolderValidateSkipped;
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
Cas::ManifestId publishPart(const Cas::PoolPtr & store, const Cas::RootNamespace & ns,
                            const String & ref, std::vector<Cas::ManifestEntry> entries)
{
    auto build = store->beginPartWrite(Cas::PartWriteInfo{.intended_ref = ns.string() + "/" + ref,
                                                  .intended_namespace = ns, .op = Cas::ProvenanceOp::Insert});
    const Cas::ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, ref, id);
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

Cas::CachedPartFolderAccess::CacheParams cacheOn()
{
    return {.cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 16ULL << 20,
            .explain_enabled = true, .validate = {}};
}

/// Mirrors gtest_cas_s3_staging.cpp's helper of the same shape: the shape a real CAS disk config
/// has under `storage_configuration.disks.<name>`, so `config_prefix = "disk"` reads exactly like
/// the disk factory's `config_prefix`. Used to unit-test `parsePartFolderValidate` standalone.
Poco::AutoPtr<Poco::Util::XMLConfiguration> configWithDiskSection(const std::string & inner_xml)
{
    std::istringstream xml_stream( // STYLE_CHECK_ALLOW_STD_STRING_STREAM
        "<clickhouse><disk>" + inner_xml + "</disk></clickhouse>");
    return new Poco::Util::XMLConfiguration(xml_stream);
}

/// Every mutating backend op throws once armed — models a correlated backend outage during the
/// transaction's compensating rollback (dropRef must append a removal, which mutates the backend).
class RollbackFaultBackend final : public Cas::InMemoryBackend
{
public:
    std::atomic<bool> armed{false};

    Cas::PutResult putIfAbsent(const String & k, const String & b, const Cas::ObjectMeta & m = {}) override
    {
        failIfArmed();
        return InMemoryBackend::putIfAbsent(k, b, m);
    }

    Cas::WriteSinkPtr putIfAbsentStream(const String & k, const Cas::ObjectMeta & m = {}) override
    {
        failIfArmed();
        return InMemoryBackend::putIfAbsentStream(k, m);
    }

    Cas::PutResult putOverwrite(const String & k, const String & b, const Cas::Token & e, const Cas::ObjectMeta & m = {}) override
    {
        failIfArmed();
        return InMemoryBackend::putOverwrite(k, b, e, m);
    }

    Cas::CasResult casPut(const String & k, const String & b, const std::optional<Cas::Token> & e, const Cas::ObjectMeta & m = {}) override
    {
        failIfArmed();
        return InMemoryBackend::casPut(k, b, e, m);
    }

    Cas::DeleteOutcome deleteExact(const String & k, const Cas::Token & t) override
    {
        failIfArmed();
        return InMemoryBackend::deleteExact(k, t);
    }

private:
    void failIfArmed()
    {
        if (armed.load())
            throw Exception(ErrorCodes::ABORTED, "injected backend outage");
    }
};

}

TEST(CasPartFolderAccess, RetainedHitSkipsManifestHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    for (int i = 0; i < 5; ++i)
        ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);

    /// The one-GET goal (spec acceptance 4): ONE body GET, ONE mandatory HEAD (the cold build);
    /// every subsequent CachedForLoad call is a validated hit — zero manifest ops.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), 1u);
    EXPECT_TRUE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::Hit);
}

TEST(CasPartFolderAccess, HitPathJournalEmptyAndCheapWhenExplainDisabled)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    /// Retention ON, explain journal OFF (the production default): the hit path must take neither the
    /// per-disk explain mutex nor write a journal entry (B2).
    Cas::CachedPartFolderAccess access(store,
        {.cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 16ULL << 20,
         .explain_enabled = false, .validate = {}});
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    for (int i = 0; i < 5; ++i)
        ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);

    /// Same request oracle as RetainedHitSkipsManifestHead — one cold build, then validated hits.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), 1u);
    /// The journal is never written when disabled.
    EXPECT_EQ(access.explainJournalSizeForTest(), 0u);
    /// explain() still reports live retention truthfully, but the decision defaults to Miss (unwritten).
    EXPECT_TRUE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::Miss);
}

TEST(CasPartFolderAccess, GetViewServesCommittedFolder)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    publishPart(store, ns, "part_1",
                {inlineEntry("checksums.txt", "cs"), inlineEntry("count.txt", "1"), inlineEntry("txn_version.txt", "v1")});

    Cas::CachedPartFolderAccess access(store);
    const Cas::PartRefKey key{ns, "part_1"};

    auto view = access.getView(key, Cas::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("checksums.txt"), nullptr);
    EXPECT_EQ(view->inlineBytes("txn_version.txt"), std::optional<String>("v1"));

    /// Absent ref => nullptr, never an exception, never retained (nothing to retain in Phase 2).
    EXPECT_EQ(access.getView({ns, "absent"}, Cas::Freshness::CachedForLoad), nullptr);
    EXPECT_TRUE(access.existsRef(key, Cas::Freshness::CachedForLoad));
    EXPECT_FALSE(access.existsRef({ns, "absent"}, Cas::Freshness::ForceFresh));
    ASSERT_TRUE(access.resolve(key, Cas::Freshness::ForceFresh).has_value());
}

TEST(CasPartFolderAccess, GetViewFailsClosedOnMissingBody)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    /// Physically delete the live manifest body (a protocol violation) — every getView mode must
    /// surface INV-NO-DANGLE as FILE_DOESNT_EXIST in Phase 2 (there is no retained view to hit).
    /// Retention is off (the single-arg ctor below), so this is the `always` (default) part_folder_validate
    /// mode under test regardless — the `never`/`age` skip is proven by the ValidateNever/ValidateAge
    /// tests further down, which turn retention ON.
    deleteManifestBody(*backend, layout, id);

    Cas::CachedPartFolderAccess access(store);
    const Cas::PartRefKey key{ns, "part_1"};
    for (auto freshness : {Cas::Freshness::CachedForLoad,
                           Cas::Freshness::ForceFresh,
                           Cas::Freshness::StrictValidate})
        expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST, [&] { access.getView(key, freshness); });
}

TEST(CasPartFolderAccess, WritePrimitivesRoundTrip)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store);
    const Cas::PartRefKey key{ns, "part_1"};

    /// promoteBuild: the transaction's terminal publish step, through the facade.
    auto build = store->beginPartWrite(Cas::PartWriteInfo{.intended_ref = ns.string() + "/part_1",
                                                  .intended_namespace = ns, .op = Cas::ProvenanceOp::Insert});
    const Cas::ManifestId id = build->stageManifest({inlineEntry("checksums.txt", "cs")});
    build->precommitAdd(ns, "part_1", id);
    access.promoteBuild(*build, key, build->buildId(), id);
    ASSERT_TRUE(access.existsRef(key, Cas::Freshness::ForceFresh));

    /// dropRefIfPresent: replay-safe (absent ref is success, not failure).
    access.dropRefIfPresent(key);
    EXPECT_FALSE(access.existsRef(key, Cas::Freshness::ForceFresh));
    access.dropRefIfPresent(key);                              /// second drop: no-op, no throw
    access.dropRefBestEffort(key);                             /// noexcept even when absent

    /// dropNamespace clears the whole namespace.
    publishPart(store, ns, "part_2", {inlineEntry("checksums.txt", "cs")});
    access.dropNamespace(ns);
    EXPECT_FALSE(access.existsRef({ns, "part_2"}, Cas::Freshness::ForceFresh));
}

TEST(CasPartFolderAccess, RepublishRefMovesCommittedRef)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store);
    publishPart(store, ns, "src_part", {inlineEntry("checksums.txt", "cs"), inlineEntry("txn_version.txt", "v1")});

    EXPECT_FALSE(access.republishRef({ns, "absent"}, {ns, "dst"}));   /// absent source: nothing written

    ASSERT_TRUE(access.republishRef({ns, "src_part"}, {ns, "dst_part"}));
    EXPECT_FALSE(access.existsRef({ns, "src_part"}, Cas::Freshness::ForceFresh));
    auto view = access.getView({ns, "dst_part"}, Cas::Freshness::ForceFresh);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("checksums.txt"), nullptr);
    EXPECT_EQ(view->inlineBytes("txn_version.txt"), std::optional<String>("v1"));   /// carried over
}

TEST(CasPartFolderAccess, RepublishRefIdempotentRedriveAndConflict)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store);

    /// Re-drive: dst already committed with the SAME content (a prior attempt's promote landed,
    /// only dropRef(src) was interrupted) -- idempotent-skip: drop src, dst's manifest is untouched
    /// (all-tree-part-files Task 9: there is no separate mutable payload left to drift/re-sync --
    /// identical `entries` is the whole idempotency contract now).
    publishPart(store, ns, "src", {inlineEntry("f", "same")});
    publishPart(store, ns, "dst", {inlineEntry("f", "same")});
    const auto dst_id_before = access.resolve({ns, "dst"}, Cas::Freshness::ForceFresh)->manifest_id;
    ASSERT_TRUE(access.republishRef({ns, "src"}, {ns, "dst"}));
    EXPECT_FALSE(access.existsRef({ns, "src"}, Cas::Freshness::ForceFresh));
    auto resolved = access.resolve({ns, "dst"}, Cas::Freshness::ForceFresh);
    EXPECT_EQ(resolved->manifest_id, dst_id_before) << "idempotent re-drive must not mint a fresh manifest";

    /// Conflict: dst committed with DIFFERENT content — fail closed, src untouched.
    publishPart(store, ns, "src2", {inlineEntry("f", "one")});
    publishPart(store, ns, "dst2", {inlineEntry("f", "two")});
    expectThrowsCode(ErrorCodes::ABORTED, [&] { access.republishRef({ns, "src2"}, {ns, "dst2"}); });
    EXPECT_TRUE(access.existsRef({ns, "src2"}, Cas::Freshness::ForceFresh));
}

TEST(CasPartFolderAccess, ExplainRecordsDecisions)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, {.explain_enabled = true, .validate = {}});
    publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const Cas::PartRefKey key{ns, "part_1"};

    access.getView(key, Cas::Freshness::CachedForLoad);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::Miss);       /// cold build
    EXPECT_FALSE(access.explain(key).retained);                                    /// Phase 3: never

    access.getView(key, Cas::Freshness::ForceFresh);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::ForceFreshRead);

    access.getView(key, Cas::Freshness::StrictValidate);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::StrictBypass);

    access.dropRef(key);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::Invalidated);
    EXPECT_GT(access.explain(key).estimated_bytes, 0u);
}

TEST(CasPartFolderAccess, BaselineRequestCountsWithoutRetention)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store);
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int n = 5;
    for (int i = 0; i < n; ++i)
        ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);

    /// The Phase-3 baseline (retention off): one manifest-body GET (the decode cache absorbs the
    /// rest) but a mandatory manifest HEAD per call. Phase 4's validated hits remove the HEADs;
    /// this test pins the numbers Phase 4 improves.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), static_cast<uint64_t>(n));
}

/// ==== Phase 4 (retention) semantics battery: spec §Testing acceptance criteria ====

/// REMOVED (all-tree-part-files Task 9, spec 2026-07-14-cas-all-tree-part-files-design.md §3):
/// `MutableRefreshWithoutManifestRead` and `WriteThroughEraseThenRebuild` proved the cache facade's
/// `LastDecision::MutableRefresh` fast path -- a cheap re-check that could serve a retained view whose
/// manifest was unchanged but whose separate mutable payload had drifted, without a manifest re-read.
/// That whole two-tier freshness model is gone: every per-part file is an ordinary manifest entry now,
/// so ANY content change is a manifest change (`repointRef`) and the existing manifest-id staleness
/// check (`getView`'s `cached->manifestId() == resolved->manifest_id` compare) is the only freshness
/// check left -- there is no cheaper "payload-only" path to test separately. Coverage that remains
/// valid: `MismatchRebuildAfterRepublish` below proves the cache correctly rebuilds when the manifest
/// id changes under a retained view (the one case the deleted tests' "erase => cold rebuild" half also
/// exercised); `gtest_cas_repoint.cpp` (Task 3) proves `repointRef` erases the affected view on success.
TEST(CasPartFolderAccess, MismatchRebuildAfterRepublish)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, cacheOn());
    publishPart(store, ns, "part_1", {inlineEntry("f", "orig")});
    const Cas::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);   /// retained

    /// Drop + republish the SAME ref name with DIFFERENT content through the raw Core protocol (no
    /// facade => no write-through erase): the retained entry survives with a manifest_id that no
    /// longer resolves — the next CachedForLoad hits the manifest-changed compare (step 2c).
    store->dropRef(ns, "part_1");
    const auto id2 = publishPart(store, ns, "part_1", {inlineEntry("f", "DIFFERENT")});
    const String manifest_key2 = layout.manifestKey(id2);
    backend->resetCounts();

    auto view = access.getView(key, Cas::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("f"), nullptr);
    EXPECT_EQ(view->findFile("f")->inline_bytes, "DIFFERENT");    /// never the stale view
    EXPECT_EQ(backend->getCount(manifest_key2), 1u);              /// one new manifest GET
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::Miss);   /// rebuilt, now retained
    EXPECT_TRUE(access.explain(key).retained);
}

TEST(CasPartFolderAccess, ForceFreshFailsClosedWhileRetainedViewExists)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const Cas::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);   /// retained
    deleteManifestBody(*backend, layout, id);   /// protocol violation: live body vanishes

    /// Write-evidence and strict paths surface INV-NO-DANGLE immediately (mandatory HEAD)...
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, Cas::Freshness::ForceFresh); });
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, Cas::Freshness::StrictValidate); });

    /// ...while a validated CachedForLoad hit still serves the immutable decode — the documented
    /// residual delta (spec §Staleness Equivalence): detection deferred, never for write evidence.
    EXPECT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);
}

/// ==== §3 (part_folder_validate): the ForceFresh body re-proof HEAD is configurable ====

TEST(CasPartFolderAccess, ValidateNeverServesRetainedViewWithoutBodyHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    auto params = cacheOn();
    params.validate = {Cas::PartFolderValidate::Mode::Never, 0};
    Cas::CachedPartFolderAccess access(store, params);
    const Cas::PartRefKey key{ns, "part_1"};

    /// Prime the retained view (pays the HEAD once).
    ASSERT_NE(access.getView(key, Cas::Freshness::ForceFresh), nullptr);
    /// Body vanishes (a protocol violation the net would normally catch)...
    deleteManifestBody(*backend, layout, id);
    const auto skips_before = ProfileEvents::global_counters[ProfileEvents::CasPartFolderValidateSkipped].load();
    /// ...but `never` serves the retained view, no HEAD, no throw.
    EXPECT_NO_THROW(access.getView(key, Cas::Freshness::ForceFresh));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasPartFolderValidateSkipped].load() - skips_before, 1);
}

TEST(CasPartFolderAccess, ValidateAlwaysStillHeadsEveryForceFresh)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    Cas::CachedPartFolderAccess access(store, cacheOn());   /// default = Always
    const Cas::PartRefKey key{ns, "part_1"};
    ASSERT_NE(access.getView(key, Cas::Freshness::ForceFresh), nullptr);
    deleteManifestBody(*backend, layout, id);
    /// `always` re-proves the body every ForceFresh — the deleted body surfaces as FILE_DOESNT_EXIST.
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, Cas::Freshness::ForceFresh); });
}

TEST(CasPartFolderAccess, ValidateAgeSkipsWithinWindowThenHeadsAfter)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    auto params = cacheOn();
    params.validate = {Cas::PartFolderValidate::Mode::Age, /*age_seconds=*/5};
    /// An injected clock (spec §3 TDD requirement): the SAME function stamps the retained view's
    /// validated_at_ms (buildView) and drives the age-window comparison (getView), so the test controls
    /// both sides of the comparison deterministically -- no real sleep.
    std::atomic<uint64_t> fake_now_ms{1'000'000};
    Cas::CachedPartFolderAccess access(store, params, [&] { return fake_now_ms.load(); });
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    /// Prime the retained view (pays the HEAD once) at fake_now_ms.
    ASSERT_NE(access.getView(key, Cas::Freshness::ForceFresh), nullptr);
    const uint64_t heads_after_prime = backend->headCount(manifest_key);

    /// +2s: still inside the 5s window — served from the retained view, no new HEAD.
    fake_now_ms += 2000;
    ASSERT_NE(access.getView(key, Cas::Freshness::ForceFresh), nullptr);
    EXPECT_EQ(backend->headCount(manifest_key), heads_after_prime);

    /// +6s from the ORIGINAL stamp (past the 5s window): re-proves the body via a fresh HEAD.
    fake_now_ms += 4000;
    ASSERT_NE(access.getView(key, Cas::Freshness::ForceFresh), nullptr);
    EXPECT_GT(backend->headCount(manifest_key), heads_after_prime);
}

/// ==== §3: `parsePartFolderValidate` config parsing, standalone (mirrors CasS3Staging's
/// parseStagingBackend coverage) -- review finding: std::stoull silently accepted a leading '-'
/// (unsigned wraparound), so a malformed `age -5` never hit the parser's own fail-closed throw.
/// These pin the fixed `std::from_chars`-based parsing directly, with no disk/store needed. ====

TEST(CasPartFolderValidateParse, DefaultConfigParsesToAlways)
{
    /// No `part_folder_validate` key at all -- the byte-for-byte-pre-§3-behavior default.
    auto config = configWithDiskSection("<scratch_path>/tmp/whatever</scratch_path>");
    const auto v = ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk");
    EXPECT_EQ(v.mode, Cas::PartFolderValidate::Mode::Always);
}

TEST(CasPartFolderValidateParse, ParsesAlways)
{
    auto config = configWithDiskSection("<part_folder_validate>always</part_folder_validate>");
    const auto v = ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk");
    EXPECT_EQ(v.mode, Cas::PartFolderValidate::Mode::Always);
}

TEST(CasPartFolderValidateParse, ParsesNever)
{
    auto config = configWithDiskSection("<part_folder_validate>never</part_folder_validate>");
    const auto v = ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk");
    EXPECT_EQ(v.mode, Cas::PartFolderValidate::Mode::Never);
}

TEST(CasPartFolderValidateParse, ParsesPositiveAge)
{
    auto config = configWithDiskSection("<part_folder_validate>age 5</part_folder_validate>");
    const auto v = ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk");
    EXPECT_EQ(v.mode, Cas::PartFolderValidate::Mode::Age);
    EXPECT_EQ(v.age_seconds, 5u);
}

TEST(CasPartFolderValidateParse, AcceptsAgeZeroAsADegenerateButValidWindow)
{
    /// `age 0` is accepted, not rejected: it is a well-formed (if degenerate -- effectively an
    /// almost-always-expired window) configuration, not malformed input. Only genuinely malformed
    /// suffixes (negative, non-digit, empty, trailing garbage) fail closed below.
    auto config = configWithDiskSection("<part_folder_validate>age 0</part_folder_validate>");
    const auto v = ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk");
    EXPECT_EQ(v.mode, Cas::PartFolderValidate::Mode::Age);
    EXPECT_EQ(v.age_seconds, 0u);
}

TEST(CasPartFolderValidateParse, NegativeAgeThrows)
{
    /// The bug this regression-guards: std::stoull("-5") used to return 18446744073709551611
    /// (unsigned wraparound) instead of rejecting the leading '-'.
    auto config = configWithDiskSection("<part_folder_validate>age -5</part_folder_validate>");
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS,
        [&] { ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk"); });
}

TEST(CasPartFolderValidateParse, NonDigitAgeThrows)
{
    auto config = configWithDiskSection("<part_folder_validate>age abc</part_folder_validate>");
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS,
        [&] { ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk"); });
}

TEST(CasPartFolderValidateParse, TrailingGarbageAfterAgeThrows)
{
    auto config = configWithDiskSection("<part_folder_validate>age 5abc</part_folder_validate>");
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS,
        [&] { ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk"); });
}

TEST(CasPartFolderValidateParse, EmptyAgeSuffixThrows)
{
    auto config = configWithDiskSection("<part_folder_validate>age </part_folder_validate>");
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS,
        [&] { ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk"); });
}

TEST(CasPartFolderValidateParse, UnknownValueThrows)
{
    /// Fail-closed: an unrecognized value must NEVER silently become `never`/`always`.
    auto config = configWithDiskSection("<part_folder_validate>sometimes</part_folder_validate>");
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS,
        [&] { ContentAddressedMetadataStorage::parsePartFolderValidate(*config, "disk"); });
}

TEST(CasPartFolderAccess, AbsenceIsNeverRetained)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, cacheOn());
    publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const Cas::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);   /// retained
    access.dropRef(key);
    EXPECT_EQ(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);   /// absent: nullptr, never retained

    /// Re-publish under the SAME ref name: immediately visible, no stale absence remembered.
    publishPart(store, ns, "part_1", {inlineEntry("f", "y")});
    auto view = access.getView(key, Cas::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->inlineBytes("f"), std::optional<String>("y"));
}

TEST(CasPartFolderAccess, OversizedViewServedNotRetained)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    /// max_entry_bytes = 1: every real view (>= the 256-byte fixed overhead alone) is oversized.
    Cas::CachedPartFolderAccess access(store,
        Cas::CachedPartFolderAccess::CacheParams{
            .cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 1,
            .explain_enabled = true, .validate = {}});
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    auto view1 = access.getView(key, Cas::Freshness::CachedForLoad);
    ASSERT_NE(view1, nullptr);
    EXPECT_FALSE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              Cas::CachedPartFolderAccess::LastDecision::OversizedBypass);

    const uint64_t head_before = backend->headCount(manifest_key);
    auto view2 = access.getView(key, Cas::Freshness::CachedForLoad);
    ASSERT_NE(view2, nullptr);
    EXPECT_GT(backend->headCount(manifest_key), head_before);   /// not retained: re-HEADs every call
    EXPECT_FALSE(access.explain(key).retained);
}

TEST(CasPartFolderAccess, DisabledModeKeepsBaseline)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    /// CacheParams{} (cache_bytes == 0): the explicit disable switch, same as the single-arg ctor.
    Cas::CachedPartFolderAccess access(store, Cas::CachedPartFolderAccess::CacheParams{});
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int n = 5;
    for (int i = 0; i < n; ++i)
        ASSERT_NE(access.getView(key, Cas::Freshness::CachedForLoad), nullptr);

    /// Exactly the Phase-3 baseline: bytes=0 restores the no-retention call graph byte-for-byte.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), static_cast<uint64_t>(n));
    EXPECT_FALSE(access.explain(key).retained);
}

TEST(CasPartFolderAccess, SingleFlightColdBuild)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const Cas::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int k = 8;
    std::latch start_gate(k);
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<const Cas::PartFolderView>> results(k);
    for (int i = 0; i < k; ++i)
        threads.emplace_back([&, i]
        {
            start_gate.arrive_and_wait();
            results[i] = access.getView(key, Cas::Freshness::CachedForLoad);
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
    auto store = openPoolForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    Cas::CachedPartFolderAccess access(store, cacheOn());
    publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    publishPart(store, ns, "part_2", {inlineEntry("f", "y")});
    const Cas::PartRefKey key1{ns, "part_1"};
    const Cas::PartRefKey key2{ns, "part_2"};

    ASSERT_NE(access.getView(key1, Cas::Freshness::CachedForLoad), nullptr);   /// retained
    ASSERT_NE(access.getView(key2, Cas::Freshness::CachedForLoad), nullptr);   /// retained
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
    EXPECT_EQ(access.getView(key1, Cas::Freshness::CachedForLoad), nullptr);
    EXPECT_EQ(access.getView(key2, Cas::Freshness::CachedForLoad), nullptr);

    /// Recreation end-to-end (Task 12, snapshot+log §Namespace Birth): recreating the namespace requires
    /// GC's `_cleanup/<remove_txn_id>` completion marker; a warm writer re-observes it via one exact-key
    /// re-check (`Pool::observedNamespaceCleanupMarker`). This file has no GC harness, so we publish the
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
    const auto recreated_view = access.getView(key1, Cas::Freshness::CachedForLoad);
    ASSERT_NE(recreated_view, nullptr) << "the recreated namespace must serve a fresh view after the marker is durable";
    EXPECT_TRUE(access.explain(key1).retained);
}

TEST(CasPartFolderAccess, BestEffortRollbackDropCountsAndSurvivesABackendOutage)
{
    auto backend = std::make_shared<RollbackFaultBackend>();
    auto store = openPoolForTest(backend);
    Cas::CachedPartFolderAccess access(store, cacheOn());

    const Cas::RootNamespace ns_a{"srv/ta"};
    const Cas::RootNamespace ns_b{"srv/tb"};
    publishPart(store, ns_a, "part_a", {inlineEntry("checksums.txt", "cs")});
    publishPart(store, ns_b, "part_b", {inlineEntry("checksums.txt", "cs")});

    backend->armed = true;
    /// Sanity: with the backend armed, a real dropRef propagates (so the fault reaches the catch).
    EXPECT_ANY_THROW(store->dropRef(ns_a, "part_a"));

    using ProfileEvents::global_counters;
    const auto before = global_counters[ProfileEvents::CasRefRollbackBestEffortDropFailed].load();
    /// The compensating-rollback path must NOT throw (noexcept) and MUST record the swallowed failure.
    access.dropRefBestEffort(Cas::PartRefKey{ns_b, "part_b"});
    const auto after = global_counters[ProfileEvents::CasRefRollbackBestEffortDropFailed].load();
    EXPECT_EQ(after, before + 1);

    backend->armed = false;   /// let store teardown release its lease cleanly
}
