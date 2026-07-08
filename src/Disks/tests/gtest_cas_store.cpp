#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/Exception.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <optional>
#include <thread>
#include <vector>

namespace DB::ErrorCodes
{
extern const int ABORTED;
extern const int BAD_ARGUMENTS;
extern const int CORRUPTED_DATA;
extern const int NOT_IMPLEMENTED;
extern const int UNKNOWN_FORMAT_VERSION;
extern const int FILE_DOESNT_EXIST;
extern const int LOGICAL_ERROR;
}

using namespace DB::Cas;
using DB::Cas::tests::blobEntryFor;
using DB::Cas::tests::expectThrowsCode;
using DB::Cas::tests::idOf;
using DB::Cas::tests::publishRaw;
using DB::Cas::tests::shardOfForTest;
using DB::Cas::tests::u128Of;

namespace
{
/// Counts mutating backend calls so a test can assert an open path is write-free.
class WriteCountingBackend final : public DB::Cas::Backend
{
public:
    explicit WriteCountingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    size_t writes = 0;

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override { ++writes; return inner->casPut(k, b, e, meta); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { ++writes; return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }
private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

/// Publish one part `ref` through the REAL Build write path: stage a manifest holding a single content
/// blob whose payload is `payload`, precommit-add into the owning shard, then promote precommit ->
/// committed. Returns the published ManifestId. This is the canonical write-side fixture for the
/// read-path tests (the same shape as `publishPart` in gtest_cas_gc_log.cpp). The manifest entry path
/// is `data.bin` unless `entry_path` overrides it.
ManifestId publishPart(
    const StorePtr & s, const String & ns, const String & ref, const String & payload,
    const String & entry_path = "data.bin")
{
    const RootNamespace nsr{ns};
    BuildInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->startBuild(info);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));

    ManifestEntry e;
    e.path = entry_path;
    e.placement = EntryPlacement::Blob;
    e.blob_hash = u128Of(payload);
    e.blob_size = payload.size();

    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(nsr, ref, id);
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}

/// A ManifestRef carrying a unique instance id derived from `tag` (all fields explicit so the
/// missing-designated-field-initializer warning never fires). The writer/build fields are stable test
/// constants — the read path keys identity by the full ref, so any consistent choice works here.
ManifestRef manifestRefFor(const String & tag)
{
    uint32_t ordinal = 1;
    for (char c : tag)
        ordinal = ordinal * 131 + static_cast<unsigned char>(c);
    ordinal = ordinal % 999999 + 1;
    return ManifestRef{
        .writer_epoch = 1,
        .build_sequence = 1,
        .manifest_ordinal = ordinal};
}

/// Publish a part holding the given manifest entries verbatim through the real Build. Used by read-path
/// lookup/list tests that want a precise multi-entry manifest. Each Blob entry's body MUST be present at
/// promote: the promote gate revalidates EVERY blob leaf with a HEAD and fails closed on an absent body.
/// So write a blob body for each Blob entry (addressed by its hash) and record it as W-EVIDENCE before
/// staging. Inline entries need no body. Returns the published ManifestId.
ManifestId publishPartWithEntries(
    const StorePtr & s, const String & ns, const String & ref, std::vector<ManifestEntry> entries)
{
    const RootNamespace nsr{ns};
    BuildInfo info;
    info.intended_ref = ns + "/" + ref;
    auto build = s->startBuild(info);
    for (const auto & e : entries)
        if (e.placement == EntryPlacement::Blob)
        {
            /// Materialize the blob body so the promote-time HEAD revalidation succeeds, then record the
            /// tokenless W-EVIDENCE dep (the gate re-observes the current token at promote).
            DB::Cas::tests::writeBlobBody(s->backend(), s->layout(), e.blob_hash);
            build->adoptEvidence(e);
        }
    const ManifestId id = build->stageManifest(std::move(entries));
    build->precommitAdd(nsr, ref, id);
    build->promote(nsr, ref, build->buildId(), id);
    return id;
}
}

TEST(CasStore, ReadOnlyOpenSkipsProbe)
{
    auto shared = std::make_shared<DB::Cas::InMemoryBackend>();

    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    /// Writable open: creates _pool_meta and runs the probe (which writes+cleans up).
    DB::Cas::Store::open(std::make_shared<WriteCountingBackend>(shared), cfg);

    /// Read-only re-open over the SAME data must perform ZERO writes (no probe, meta already present).
    auto counter = std::make_shared<WriteCountingBackend>(shared);
    DB::Cas::PoolConfig ro = cfg;
    ro.read_only = true;
    auto store = DB::Cas::Store::open(counter, ro);
    EXPECT_EQ(counter->writes, 0u);
    ASSERT_NE(store, nullptr);
}

TEST(CasStore, MinActiveTracksInFlightBuilds)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    cfg.background_watermark = false;
    auto store = DB::Cas::Store::open(backend, cfg);

    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   /// no builds: floor == next seq
    auto b1 = store->startBuild({});                            /// seq 1
    auto b2 = store->startBuild({});                            /// seq 2
    ASSERT_EQ(store->minActive(), 1u);
    b1->abandon();                                              /// finishes seq 1
    ASSERT_EQ(store->minActive(), 2u);                          /// floor advances
    b2->abandon();
    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   /// empty again
}

TEST(CasStore, BuildSeqIsStrictlyMonotone)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    cfg.server_root_id = "test";
    cfg.background_watermark = false;
    auto store = DB::Cas::Store::open(backend, cfg);
    auto a = store->startBuild({});
    auto sa = a->buildSeq();
    a->abandon();
    auto b = store->startBuild({});
    ASSERT_GT(b->buildSeq(), sa);                               /// never reused, never lower
}

TEST(CasPoolMeta, CreateThenReopen)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    PoolMeta created = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 8, /*blob_header_len*/ 256);
    EXPECT_NE(created.pool_id, UInt128{});
    PoolMeta reopened = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(reopened.pool_id, created.pool_id);     /// pool is authoritative — config ignored on reopen
    EXPECT_EQ(reopened.root_shards, 8u);
    EXPECT_EQ(reopened.blob_header_len, 256u);
}

TEST(CasPoolMeta, FailClosed)
{
    Layout layout("p");
    /// Garbage bytes that are not a valid protobuf pool-meta => CORRUPTED_DATA (createOrValidate path).
    /// The converged-header future-compat fail-closed (compatibility_version > G_BUILD =>
    /// UNKNOWN_FORMAT_VERSION) is covered at the codec level by CasHeaderGolden.PoolMetaFailClosedOnGarbage;
    /// a future object is now a VALID protobuf with a higher header.compatibility_version, not a magic
    /// prefix, so it cannot be hand-crafted here without the proto headers.
    auto b2 = std::make_shared<InMemoryBackend>();
    b2->putIfAbsent(layout.poolMetaKey(), "garbage");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*b2, layout, 8, 256); });
}

TEST(CasPoolMeta, RoundTripAndReadability)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("0123456789abcdeffedcba9876543210");
    pm.root_shards = 8;
    pm.blob_header_len = 256;

    const String encoded = encodePoolMeta(pm);
    /// Pure protobuf (not JSON); the CAPM magic is the CasHeader.magic fixed32 field (little-endian,
    /// so the 4 bytes read "CAPM"), carried INSIDE the message, not at byte 0.
    ASSERT_GE(encoded.size(), 8u);
    EXPECT_NE(encoded.find(String("CAPM")), String::npos);
    EXPECT_NE(encoded.front(), '{');

    PoolMeta decoded = decodePoolMeta(encoded);
    EXPECT_EQ(decoded.pool_id, pm.pool_id);
    EXPECT_EQ(decoded.root_shards, pm.root_shards);
    EXPECT_EQ(decoded.blob_header_len, pm.blob_header_len);
}

TEST(CasPoolMeta, RejectsBadConstantsAtCreation)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// not 8-aligned
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 100); });
    /// below the 96-byte floor
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 64); });
    /// above the 16 KiB ceiling
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 17 * 1024); });
    /// zero shards
    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS,
        [&] { PoolMeta::createOrValidate(*b, layout, 0, 256); });

    /// A creation that fails config validation must not have written anything.
    EXPECT_FALSE(b->get(layout.poolMetaKey()).has_value());
}

TEST(CasPoolMeta, RejectsBadConstantsOnDecode)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");
    /// Encode a PoolMeta with blob_header_len=100 (not 8-aligned); decode must reject it as CORRUPTED_DATA.
    PoolMeta bad_pm;
    bad_pm.pool_id = hexToU128("00000000000000000000000000000001");
    bad_pm.root_shards = 8;
    bad_pm.blob_header_len = 100;   /// violates 8-alignment invariant
    b->putIfAbsent(layout.poolMetaKey(), encodePoolMeta(bad_pm));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { PoolMeta::createOrValidate(*b, layout, 8, 256); });
}

TEST(CasPoolMeta, DecodeGarbageFails)
{
    /// Any non-CAPM framing byte sequence => CORRUPTED_DATA.
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("garbage")); });
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodePoolMeta(String("")); });
}

TEST(CasPoolMeta, ConcurrentCreateRace)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout layout("p");

    /// A racing creator already wrote a valid foreign pool_id. createOrValidate must NOT overwrite it:
    /// it re-reads (after losing the create-if-absent CAS, or seeing it present) and returns the
    /// foreign pool_id, validated like a reopen.
    const UInt128 foreign = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta foreign_pm;
    foreign_pm.pool_id = foreign;
    foreign_pm.root_shards = 8;
    foreign_pm.blob_header_len = 256;
    b->putIfAbsent(layout.poolMetaKey(), encodePoolMeta(foreign_pm));

    PoolMeta result = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, foreign);
    EXPECT_EQ(result.root_shards, 8u);     /// the foreign pool's constants win
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CasPoolMeta, CasConflictReReadsWinner)
{
    /// The subtlest branch: the initial GET sees ABSENT, so createOrValidate proceeds to the
    /// create-if-absent casPut — and loses, because a racing creator committed in between. The loser
    /// must then re-read and return the WINNER's pool identity, not LOGICAL_ERROR. A single-threaded
    /// `failNextCasPut` alone cannot exercise this: it returns Conflict without leaving the object
    /// readable, so the re-read would fire the LOGICAL_ERROR guard. We model the real interleaving
    /// with a backend whose casPut commits the winner's object (via the public putIfAbsent) and THEN
    /// reports Conflict — exactly what the loser observes.
    class RacingBackend : public InMemoryBackend
    {
    public:
        String winner_bytes;
        CasResult casPut(const String & key, const String & bytes,
            const std::optional<Token> & expected, const ObjectMeta & meta = {}) override
        {
            if (!winner_committed)
            {
                winner_committed = true;
                /// The winner lands first; our create-if-absent now necessarily conflicts.
                putIfAbsent(key, winner_bytes);
                return {CasOutcome::Conflict, {}};
            }
            return InMemoryBackend::casPut(key, bytes, expected, meta);
        }
    private:
        bool winner_committed = false;
    };

    const UInt128 winner = hexToU128("0123456789abcdeffedcba9876543210");
    PoolMeta winner_pm;
    winner_pm.pool_id = winner;
    winner_pm.root_shards = 8;
    winner_pm.blob_header_len = 256;

    auto b = std::make_shared<RacingBackend>();
    b->winner_bytes = encodePoolMeta(winner_pm);
    Layout layout("p");

    /// Our config (4 / 512) is what we WOULD have minted, but we lose the race and inherit the winner.
    PoolMeta result = PoolMeta::createOrValidate(*b, layout, /*root_shards*/ 4, /*blob_header_len*/ 512);
    EXPECT_EQ(result.pool_id, winner);
    EXPECT_EQ(result.root_shards, 8u);
    EXPECT_EQ(result.blob_header_len, 256u);
}

TEST(CasStore, OpenFailsClosedOnNonEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);
    expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
        [&] { Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"}); });   /// the probe error contract
}

TEST(CasStore, OpenCreatesPoolMetaAndReopens)
{
    auto b = std::make_shared<InMemoryBackend>();
    /// Two CONCURRENT opens over the same POOL: a shared pool is the multi-server model, so each
    /// mounts a DISTINCT server_root_id (and a distinct server_id) — same-root same-uuid co-mounting
    /// is correctly fail-closed by the mount-safety protocol. This test only asserts that pool-meta is
    /// pool-authoritative and shared across opens.
    auto s1 = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(1), .server_root_id = "srv-1"});
    auto s2 = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_id = UInt128(2), .server_root_id = "srv-2", .root_shards = 4});
    EXPECT_EQ(s2->poolMeta().root_shards, 32u);   /// the PoolConfig default (2026-07-03 weighing)                      /// pool authoritative
    EXPECT_EQ(s1->poolMeta().pool_id, s2->poolMeta().pool_id);
}

TEST(CasStore, OpenWithExplicitConstantsCreatesThem)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 4, .blob_header_len = 512});
    EXPECT_EQ(s->poolMeta().root_shards, 4u);                       /// config applies at creation
    EXPECT_EQ(s->poolMeta().blob_header_len, 512u);
}

TEST(CasStore, VerbatimFilesLifecycle)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};
    s->putNamespaceFile(ns, "format_version.txt", "1\n");
    s->putNamespaceFile(ns, "uuid.txt", "abc");
    EXPECT_EQ(s->getNamespaceFile(ns, "format_version.txt"), String("1\n"));
    EXPECT_FALSE(s->getNamespaceFile(ns, "absent").has_value());
    auto names = s->listNamespaceFiles(ns);
    EXPECT_EQ(names, (std::vector<String>{"format_version.txt", "uuid.txt"}));
    s->putNamespaceFile(ns, "uuid.txt", "def");                     /// overwrite allowed (head + putOverwrite)
    EXPECT_EQ(s->getNamespaceFile(ns, "uuid.txt"), String("def"));
}

TEST(CasStore, ListNamespaceFilesEmpty)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};
    EXPECT_TRUE(s->listNamespaceFiles(ns).empty());
}

/// ---------- read side (spec §6): resolveRef / readManifest / lookupPath / listDirectory / listRefs ----------

/// Phase 1c read path: a published ref resolves to a ManifestId; readManifest returns the immutable
/// body; locate yields a ranged blob read; an Inline entry has no location. Replaces the old
/// resolveRef().tree_id / readTree round trip (the tree model is gone — a part is a single ManifestId).
TEST(CasStore, ResolveReturnsManifestId)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};

    /// blob "hello world" + an inline file, published through the real Build write path.
    const String payload = "hello world";
    BuildInfo info;
    info.intended_ref = ns.string() + "/part_1";
    auto build = s->startBuild(info);
    build->putBlob(idOf(payload), BlobSource::fromString(payload));

    ManifestEntry blob_entry;
    blob_entry.path = "data.bin";
    blob_entry.placement = EntryPlacement::Blob;
    blob_entry.blob_hash = u128Of(payload);
    blob_entry.blob_size = payload.size();
    ManifestEntry inline_entry;
    inline_entry.path = "small.txt";
    inline_entry.placement = EntryPlacement::Inline;
    inline_entry.inline_bytes = "tiny\n";

    const ManifestId id = build->stageManifest({blob_entry, inline_entry});
    build->precommitAdd(ns, "part_1", id);
    build->promote(ns, "part_1", build->buildId(), id);

    auto r = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id, id);                  /// resolve yields the published ManifestId

    auto manifest = s->readManifest(r->manifest_id);
    ASSERT_EQ(manifest.entries.size(), 2u);

    /// "data.bin" sorts before "small.txt" (canonical path order).
    auto data = s->lookupPath(manifest, "data.bin");
    ASSERT_TRUE(data.has_value());
    auto loc = s->locate(*data);
    EXPECT_EQ(loc.offset, s->poolMeta().blob_header_len);
    EXPECT_EQ(loc.length, payload.size());

    auto bytes = b->get(loc.key, Range{loc.offset, loc.length});
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(bytes->bytes, payload);               /// ranged read, no header touch

    auto small = s->lookupPath(manifest, "small.txt");
    ASSERT_TRUE(small.has_value());
    EXPECT_THROW(s->locate(*small), DB::Exception);  /// Inline has no location
}

/// readManifest fail-closes on a body whose self-described `ref`/`root_namespace_id` does NOT match the
/// resolved ManifestId — the ref is addressing the wrong object / a cross-namespace dangle. We stage a
/// body raw (writeManifestRaw, the on-storage write fixture) at a ManifestId, then resolve through a
/// committed binding that names a DIFFERENT ManifestRef pointing at the SAME object key — so the head
/// succeeds, the body decodes, but refMatchesBody fails => CORRUPTED_DATA.
TEST(CasStore, ReadManifestValidatesBodyAndFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};
    Layout layout("p");

    /// (1) ref/namespace mismatch: the BODY self-describes namespace `srv1/other`, but it is addressed
    /// as a manifest of `srv1/tbl` => manifestNamespaceMatches fails => CORRUPTED_DATA. We craft an id
    /// whose key lives under `srv1/tbl` but whose body carries the foreign namespace.
    {
        const ManifestRef ref = manifestRefFor("mismatch-ns");
        const ManifestId addressed{.root_namespace = ns, .ref = ref};
        /// Encode a body that claims a DIFFERENT namespace than `addressed.root_namespace`.
        PartManifest body;
        body.ref = ref;                                     /// ref matches
        body.root_namespace_id = RootNamespace{"srv1/other"};  /// namespace does NOT
        body.entries = {blobEntryFor("f", u128Of("x"), 1)};
        body.payload_digest = computePayloadDigest(body);
        b->putIfAbsent(layout.manifestKey(addressed), encodePartManifest(body));

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(addressed); });
    }

    /// (2) ref mismatch: the body self-describes a DIFFERENT ManifestRef than the id addressing it =>
    /// refMatchesBody fails => CORRUPTED_DATA.
    {
        const ManifestRef addressed_ref = manifestRefFor("addressed-ref");
        const ManifestRef body_ref = manifestRefFor("body-ref-other");
        const ManifestId addressed{.root_namespace = ns, .ref = addressed_ref};
        PartManifest body;
        body.ref = body_ref;                                /// ref does NOT match `addressed`
        body.root_namespace_id = ns;                        /// namespace matches
        body.entries = {blobEntryFor("f", u128Of("y"), 1)};
        body.payload_digest = computePayloadDigest(body);
        b->putIfAbsent(layout.manifestKey(addressed), encodePartManifest(body));

        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(addressed); });
    }

    /// (3) a committed ref naming a manifest with NO body present => readManifest throws
    /// FILE_DOESNT_EXIST (INV-NO-DANGLE surfaced on the read path). resolveRef itself SUCCEEDS — refs
    /// are pure manifest state.
    {
        const ManifestRef missing_ref = manifestRefFor("never-staged");
        const uint64_t shard = shardOfForTest("part_dangle", s->poolMeta().root_shards);
        RootShard root;
        root.shard_version = 1;
        RootRef rr;
        rr.ref_name = "part_dangle";
        rr.manifest_ref = missing_ref;
        root.refs["part_dangle"] = rr;
        publishRaw(*b, layout, ns, shard, root);

        auto r = s->resolveRef(ns, "part_dangle");
        ASSERT_TRUE(r.has_value());
        expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readManifest(r->manifest_id); });
    }
}

/// lookupPath and listDirectory over a decoded part manifest's canonical-path-ordered entries.
TEST(CasStore, LookupAndListOverManifestEntries)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};

    /// A multi-file/multi-directory part: top-level + a projection subdir.
    std::vector<ManifestEntry> entries;
    entries.push_back(blobEntryFor("columns.txt", u128Of("cols"), 4));
    entries.push_back(blobEntryFor("data.bin", u128Of("data"), 8));
    entries.push_back(blobEntryFor("p.proj/data.bin", u128Of("proj-data"), 6));
    entries.push_back(blobEntryFor("p.proj/columns.txt", u128Of("proj-cols"), 5));
    const ManifestId id = publishPartWithEntries(s, ns.string(), "all_1_1_0", entries);

    auto r = s->resolveRef(ns, "all_1_1_0");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->manifest_id, id);
    auto manifest = s->readManifest(r->manifest_id);
    ASSERT_EQ(manifest.entries.size(), 4u);

    /// lookupPath: exact-path hit + miss.
    auto hit = s->lookupPath(manifest, "data.bin");
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->blob_hash, u128Of("data"));
    EXPECT_FALSE(s->lookupPath(manifest, "no_such_file").has_value());

    /// listDirectory under "p.proj/" yields exactly the two projection files, in canonical order.
    auto proj = s->listDirectory(manifest, "p.proj/");
    ASSERT_EQ(proj.size(), 2u);
    EXPECT_EQ(proj[0].path, "p.proj/columns.txt");
    EXPECT_EQ(proj[1].path, "p.proj/data.bin");

    /// The empty prefix lists everything (all four), still in canonical order.
    auto all = s->listDirectory(manifest, "");
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0].path, "columns.txt");
    EXPECT_EQ(all[3].path, "p.proj/data.bin");
}

/// The Phase 1c manifest decode cache is keyed by (ManifestId, Token). Resolve+read the same ref twice:
/// the second readManifest must be served from the cache (no second GET of the body). A fresh publish
/// under a DIFFERENT ref name mints a NEW ManifestId (and a new shard token), so the cache misses and
/// the body is fetched again. A CountingBackend asserts the body GET count.
TEST(CasStore, ManifestCacheIsKeyedByIdAndToken)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const RootNamespace ns{"srv1/tbl"};
    Layout layout("p");

    const ManifestId id1 = publishPart(s, ns.string(), "part_1", "payload-1");
    const String key1 = layout.manifestKey(id1);

    /// First read: a body GET populates the (id1, token) cache entry.
    {
        auto r = s->resolveRef(ns, "part_1");
        ASSERT_TRUE(r.has_value());
        auto m = s->readManifest(r->manifest_id);
        ASSERT_EQ(m.entries.size(), 1u);
    }
    const uint64_t gets_after_first = b->getCount(key1);
    ASSERT_GE(gets_after_first, 1u);               /// the first read DID fetch the body

    /// Second read of the SAME id: the (id, token) cache must serve it — NO additional body GET.
    {
        auto r = s->resolveRef(ns, "part_1");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->manifest_id, id1);
        auto m = s->readManifest(r->manifest_id);
        ASSERT_EQ(m.entries.size(), 1u);
    }
    EXPECT_EQ(b->getCount(key1), gets_after_first)
        << "second readManifest re-GET the body for the same (ManifestId, Token) — cache miss";

    /// A fresh publish under a DIFFERENT ref name mints a NEW ManifestId: the cache (keyed by id) misses.
    /// (Promoting a different manifest over the SAME committed ref is a distinct promote-over-committed
    /// leak that `Build::promote` now forbids — see the CasPromoteRepublish tests.)
    const ManifestId id2 = publishPart(s, ns.string(), "part_2", "payload-2");
    EXPECT_FALSE(id2 == id1);                       /// a new publish never reuses a ManifestId
    const String key2 = layout.manifestKey(id2);

    auto r2 = s->resolveRef(ns, "part_2");
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(r2->manifest_id, id2);               /// resolve now sees the new manifest
    auto m2 = s->readManifest(r2->manifest_id);
    ASSERT_EQ(m2.entries.size(), 1u);
    EXPECT_GE(b->getCount(key2), 1u)               /// the new id's body WAS fetched (cache miss)
        << "fresh publish (new ManifestId) should miss the id-keyed manifest cache";
}

TEST(CasStore, ResolveDecodeCacheInvalidatesOnWrite)
{
    /// B113: resolveRef uses a token-validated shard-manifest decode cache. A write to the shard
    /// mints a new token, so a subsequent resolve must observe the change (cache must NOT serve a
    /// stale decoded manifest). Without token invalidation this would still see the dropped ref.
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    publishPart(s, ns.string(), "part_1", "payload-1");

    /// First resolve decodes + caches; second is a cache hit — both must see part_1.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());

    /// Write through the Store (mutateShard => new shard token), removing part_1.
    s->dropRef(ns, "part_1");

    /// The cache must invalidate on the token change: resolve now reflects the drop.
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CasStore, ResolveAbsentRefAndAbsentNamespace)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    /// A freshly-opened pool has no shard manifests: an absent shard is an empty manifest, so resolve
    /// yields nullopt and listRefs is empty (NOT an error).
    EXPECT_FALSE(s->resolveRef(ns, "anything").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CasStore, ListRefsMergesAllShards)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;

    /// Publish refs "a".."h", each routed to its shard's manifest; refs colliding into one shard share
    /// it. A raw RootShard with a committed RootRef per ref (manifest bodies need not exist — listRefs
    /// reads only shard manifest state, like resolveRef).
    std::map<uint64_t, RootShard> by_shard;
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const String ref(1, c);
        const ManifestRef mref = manifestRefFor("manifest-" + ref);
        RootRef rr;
        rr.ref_name = ref;
        rr.manifest_ref = mref;
        by_shard[shardOfForTest(ref, shards)].refs[ref] = rr;
    }
    for (auto & [shard, root] : by_shard)
    {
        root.shard_version = 1;
        publishRaw(*b, layout, ns, shard, root);
    }

    auto refs = s->listRefs(ns);
    ASSERT_EQ(refs.size(), 8u);
    for (char c = 'a'; c <= 'h'; ++c)
    {
        const String ref(1, c);
        ASSERT_TRUE(refs.count(ref));
        EXPECT_EQ(refs.at(ref).manifest_id.ref, manifestRefFor("manifest-" + ref));
        EXPECT_EQ(refs.at(ref).manifest_id.root_namespace.string(), ns.string());
    }
}

/// Task A (2026-07-03 CREATE/load HEAD storm): an empty namespace must cost exactly one LIST of the
/// namespace's ref-shard prefix and ZERO HEADs — not one HEAD per root shard (32 by default). Measure
/// deltas around the listRefs call: Store::open itself may LIST (probe/pool-meta), so the pre-call
/// counts are the baseline.
TEST(CasStore, ListRefsEmptyNamespaceCostsOneListZeroHeads)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    RootNamespace ns{"srv1/tbl"};

    const uint64_t heads_before = b->headTotal();
    const uint64_t lists_before = b->listTotal();

    auto refs = s->listRefs(ns);

    EXPECT_TRUE(refs.empty());
    EXPECT_EQ(b->headTotal() - heads_before, 0u)
        << "empty-namespace listRefs must not HEAD any shard (the CREATE/load storm)";
    EXPECT_EQ(b->listTotal() - lists_before, 1u)
        << "empty-namespace listRefs must cost exactly one LIST of the namespace's ref-shard prefix";
}

/// listRefs, after switching to LIST-first shard discovery, must still return exactly the same content
/// as the old HEAD-every-shard loop, and must HEAD no more than the number of PRESENT shards (not
/// root_shards).
TEST(CasStore, ListRefsReturnsSameContentAsBefore)
{
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;

    /// Publish refs "a", "m", "z" — chosen so they are highly likely to land in different shards
    /// (shardOfForTest hashes the ref name); collect the actual set of PRESENT shards below.
    std::map<uint64_t, RootShard> by_shard;
    for (const String & ref : {String("a"), String("m"), String("z")})
    {
        const ManifestRef mref = manifestRefFor("manifest-" + ref);
        RootRef rr;
        rr.ref_name = ref;
        rr.manifest_ref = mref;
        by_shard[shardOfForTest(ref, shards)].refs[ref] = rr;
    }
    for (auto & [shard, root] : by_shard)
    {
        root.shard_version = 1;
        publishRaw(*b, layout, ns, shard, root);
    }
    const uint64_t present_shards = by_shard.size();

    const uint64_t heads_before = b->headTotal();

    auto refs = s->listRefs(ns);

    ASSERT_EQ(refs.size(), 3u);
    for (const String & ref : {String("a"), String("m"), String("z")})
    {
        ASSERT_TRUE(refs.count(ref));
        EXPECT_EQ(refs.at(ref).manifest_id.ref, manifestRefFor("manifest-" + ref));
        EXPECT_EQ(refs.at(ref).manifest_id.root_namespace.string(), ns.string());
    }
    EXPECT_LE(b->headTotal() - heads_before, present_shards)
        << "listRefs must HEAD only the PRESENT shards, never every root shard";
}

/// A stray non-numeric key under the namespace's ref-shard prefix (a foreign/corrupt object) must not
/// break listRefs — it is skipped defensively, listRefs still returns the legit refs and never throws.
TEST(CasStore, ListRefsSkipsForeignKeys)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;

    const String ref = "legit";
    const ManifestRef mref = manifestRefFor("manifest-" + ref);
    RootRef rr;
    rr.ref_name = ref;
    rr.manifest_ref = mref;
    RootShard root;
    root.shard_version = 1;
    root.refs[ref] = rr;
    const uint64_t shard = shardOfForTest(ref, shards);
    publishRaw(*b, layout, ns, shard, root);

    /// A stray non-numeric key directly under the namespace's ref-shard prefix.
    b->putIfAbsent(layout.refsNamespacePrefix(ns) + "garbage", "not-a-shard");

    std::map<String, Resolved> refs;
    EXPECT_NO_THROW(refs = s->listRefs(ns));
    ASSERT_EQ(refs.size(), 1u);
    ASSERT_TRUE(refs.count(ref));
    EXPECT_EQ(refs.at(ref).manifest_id.ref, mref);
}

/// readManifest fails CLOSED on a corrupt or kind-mismatched manifest body addressed by a live id.
TEST(CasStore, ReadManifestFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    const RootNamespace ns{"srv1/tbl"};

    /// (1) Garbage bytes at the manifest key => decodePartManifest throws CORRUPTED_DATA.
    {
        const ManifestRef ref = manifestRefFor("garbage-body");
        const ManifestId id{.root_namespace = ns, .ref = ref};
        b->putIfAbsent(layout.manifestKey(id), "not a valid manifest body");
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { s->readManifest(id); });
    }

    /// (2) A ref naming a manifest id with NO object present => readManifest throws FILE_DOESNT_EXIST
    /// (INV-NO-DANGLE), carrying the manifest key.
    {
        const ManifestRef ref = manifestRefFor("absent-body");
        const ManifestId id{.root_namespace = ns, .ref = ref};
        expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->readManifest(id); });
    }
}

/// ---------- ref lifecycle: dropRef / updateRefPayload / dropNamespace ----------

TEST(CasStore, DropRefAppendsJournalAtomically)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);

    const ManifestId id = publishPart(s, ns.string(), "part_1", "payload-1");
    const ManifestRef manifest_ref = id.ref;

    const auto before = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);

    s->dropRef(ns, "part_1");

    auto after = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_TRUE(after.refs.empty());
    EXPECT_GT(after.shard_version, before.shard_version);   /// advanced past the published value
    ASSERT_FALSE(after.journal.empty());

    /// The drop appends a removal RootOwnerEvent: old_binding = the committed binding being removed,
    /// new_binding = none (true removal ⇒ GC folds -1 + cleanup of the named manifest).
    const RootOwnerEvent & last = after.journal.back();
    ASSERT_TRUE(last.old_binding.has_value());
    EXPECT_FALSE(last.new_binding.has_value());
    EXPECT_EQ(last.old_binding->owner_kind, OwnerKind::Committed);
    EXPECT_EQ(last.old_binding->ref_name, "part_1");
    EXPECT_EQ(last.old_binding->manifest_ref, manifest_ref);
    EXPECT_EQ(last.transition_version, after.shard_version);   /// transition_version == committed shard_version

    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());

    /// Dropping a missing ref is fail-closed, never a silent no-op.
    expectThrowsCode(DB::ErrorCodes::FILE_DOESNT_EXIST, [&] { s->dropRef(ns, "no_such_ref"); });
}

TEST(CasStore, UpdateRefPayloadMutatesWithoutJournal)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);

    const ManifestId id = publishPart(s, ns.string(), "part_1", "payload-1");
    const ManifestRef manifest_ref = id.ref;

    /// Seed a mutable file first (publish leaves mutable_files empty).
    s->updateRefPayload(ns, "part_1", [](RootRef & r) { r.mutable_files["txn_version.txt"] = "1"; });

    const auto seeded = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    const size_t journal_before = seeded.journal.size();
    const uint64_t version_before = seeded.shard_version;

    s->updateRefPayload(ns, "part_1", [](RootRef & r) { r.mutable_files["txn_version.txt"] = "7"; });

    auto after = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_EQ(after.refs.at("part_1").mutable_files.at("txn_version.txt"), "7");
    EXPECT_EQ(after.refs.at("part_1").manifest_ref, manifest_ref);
    EXPECT_EQ(after.shard_version, version_before + 1);   /// +1 from the mutate CAS
    EXPECT_EQ(after.journal.size(), journal_before);      /// no reachability change ⇒ no journal record

    /// A mutator that changes manifest_ref is rejected, and the manifest is left UNTOUCHED.
    expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR, [&]
    {
        s->updateRefPayload(ns, "part_1", [](RootRef & r)
            { r.manifest_ref = manifestRefFor("other"); });
    });
    auto unchanged = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_EQ(unchanged.shard_version, version_before + 1);   /// throw aborted before casPut
    EXPECT_EQ(unchanged.refs.at("part_1").manifest_ref, manifest_ref);
    EXPECT_EQ(unchanged.refs.at("part_1").mutable_files.at("txn_version.txt"), "7");
}

TEST(CasStore, DropRefSurvivesCasConflict)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shard = shardOfForTest("part_1", s->poolMeta().root_shards);

    publishPart(s, ns.string(), "part_1", "payload-1");
    const auto before = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);

    /// Inject one artificial Conflict: the loop must re-read the (unchanged) manifest and re-apply.
    b->failNextCasPut(layout.rootShardKey(ns, shard));
    s->dropRef(ns, "part_1");

    auto after = decodeRootShard(b->get(layout.rootShardKey(ns, shard))->bytes);
    EXPECT_TRUE(after.refs.empty());
    EXPECT_GT(after.shard_version, before.shard_version);   /// advanced (exact delta depends on the fake)
    ASSERT_FALSE(after.journal.empty());
    /// Exactly one removal event for part_1 — the mutate must not double-append across the retry.
    size_t removes = 0;
    for (const RootOwnerEvent & rec : after.journal)
        if (rec.old_binding && !rec.new_binding
            && rec.old_binding->owner_kind == OwnerKind::Committed && rec.old_binding->ref_name == "part_1")
            ++removes;
    EXPECT_EQ(removes, 1u);
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasStore, DropNamespaceTombstonesAndRemovesFiles)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    Layout layout("p");
    RootNamespace ns{"srv1/tbl"};
    const uint64_t shards = s->poolMeta().root_shards;

    /// Three refs, published through the real Build (each routed to its shard; shards may collide).
    const std::vector<String> ref_names{"alpha", "bravo", "charlie"};
    for (const String & name : ref_names)
        publishPart(s, ns.string(), name, "payload-" + name);

    /// Record which shards actually hold a manifest after the publishes.
    std::set<uint64_t> touched_shards;
    for (const String & name : ref_names)
        touched_shards.insert(shardOfForTest(name, shards));

    /// Two verbatim files.
    s->putNamespaceFile(ns, "format_version.txt", "1\n");
    s->putNamespaceFile(ns, "uuid.txt", "abc");

    s->dropNamespace(ns);

    /// Every TOUCHED shard manifest still EXISTS, with empty refs and a removal journal record.
    for (uint64_t shard : touched_shards)
    {
        auto obj = b->get(layout.rootShardKey(ns, shard));
        ASSERT_TRUE(obj.has_value());
        auto after = decodeRootShard(obj->bytes);
        EXPECT_TRUE(after.refs.empty());
        bool has_remove = false;
        for (const RootOwnerEvent & rec : after.journal)
            if (rec.old_binding && !rec.new_binding && rec.old_binding->owner_kind == OwnerKind::Committed)
                has_remove = true;
        EXPECT_TRUE(has_remove);
    }

    /// UNTOUCHED shards (no manifest) remain absent — no tombstone manifest minted.
    for (uint64_t shard = 0; shard < shards; ++shard)
        if (!touched_shards.count(shard))
            EXPECT_FALSE(b->get(layout.rootShardKey(ns, shard)).has_value());

    /// Verbatim files gone; listRefs empty.
    EXPECT_FALSE(s->getNamespaceFile(ns, "format_version.txt").has_value());
    EXPECT_FALSE(s->getNamespaceFile(ns, "uuid.txt").has_value());
    EXPECT_TRUE(s->listRefs(ns).empty());
}

TEST(CasStore, ListNamespacesFromRefsTree)
{
    /// listNamespaces = LIST-based discovery (Task 4): enumerates distinct full namespace strings
    /// from ref shards under `cas/refs/`. The wiring uses it for directory-style enumeration of
    /// opaque namespace strings (M-W).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});

    EXPECT_TRUE(s->listNamespaces("").empty());   /// fresh pool: no ref shards yet

    /// Write actual ref shards so LIST(cas/refs/) can discover them.
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), RootNamespace{"srv1/tbl"},
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), RootNamespace{"shadow/bk1/tbl"},
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});
    DB::Cas::tests::publishCommittedTransition(*b, s->layout(), RootNamespace{"shadow/bk2/tbl"},
        "ref1", std::nullopt, DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1});

    const auto all = s->listNamespaces("");
    EXPECT_EQ(all.size(), 3u);
    const auto shadows = s->listNamespaces("shadow/");
    ASSERT_EQ(shadows.size(), 2u);
    /// listNamespaces returns results from an unordered_set; sort for deterministic comparison.
    auto sorted_shadows = shadows;
    std::sort(sorted_shadows.begin(), sorted_shadows.end());
    EXPECT_EQ(sorted_shadows[0], "shadow/bk1/tbl");
    EXPECT_EQ(sorted_shadows[1], "shadow/bk2/tbl");
    EXPECT_TRUE(s->listNamespaces("nope/").empty());
}

namespace
{
/// Blocks the FIRST head() call made after `arm()` on a latch so followers attach to the
/// single-flight entry while the leader is still in-flight. Deterministic: no sleeps.
/// Calls before `arm()` pass through immediately.
class GatedHeadBackend : public DB::Cas::InMemoryBackend
{
public:
    DB::Cas::HeadResult head(const String & key) override
    {
        {
            std::unique_lock lock(m);
            ++head_calls;
            if (armed && !leader_in_head)
            {
                leader_in_head = true;
                cv.notify_all();
                gate.wait(lock, [this] { return released; });
            }
        }
        return DB::Cas::InMemoryBackend::head(key);
    }

    /// Enable the gate: the NEXT head() call will be the "leader" and will block until release().
    void arm()
    {
        std::lock_guard lock(m);
        armed = true;
        leader_in_head = false;
        released = false;
    }

    void waitLeaderInHead()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return leader_in_head; });
    }

    void release()
    {
        {
            std::lock_guard lock(m);
            released = true;
        }
        gate.notify_all();
    }

    uint64_t headCalls() const
    {
        std::lock_guard lock(m);
        return head_calls;
    }

private:
    mutable std::mutex m;
    std::condition_variable cv;
    std::condition_variable gate;
    uint64_t head_calls = 0;
    bool armed = false;
    bool leader_in_head = false;
    bool released = false;
};

/// Gates the FIRST get() made after `arm()`. It SNAPSHOTS the value as-of get-entry (modelling an
/// object-store read that began before a concurrent write committed), then blocks on a latch so the
/// test can land a write + cache-invalidation, then returns the SNAPSHOT (stale bytes) — not the
/// post-write value. Calls before arm(), and every call once the leader is gated, pass through
/// immediately. Deterministic: no sleeps. Used to reproduce the read-your-writes decode-cache
/// poisoning race (B157): an in-flight reader populating the TTL fast-path with a decode that a
/// committed write has already superseded + invalidated.
class GatedGetBackend : public DB::Cas::InMemoryBackend
{
public:
    std::optional<DB::Cas::GetResult> get(const String & key, DB::Cas::Range range = {}) override
    {
        auto snapshot = DB::Cas::InMemoryBackend::get(key, range);
        {
            std::unique_lock lock(m);
            if (armed && !leader_in_get)
            {
                leader_in_get = true;
                cv.notify_all();
                gate.wait(lock, [this] { return released; });
            }
        }
        return snapshot;
    }

    /// Enable the gate: the NEXT get() call will be the "leader" and will block until release().
    void arm()
    {
        std::lock_guard lock(m);
        armed = true;
        leader_in_get = false;
        released = false;
    }

    void waitLeaderInGet()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return leader_in_get; });
    }

    void release()
    {
        {
            std::lock_guard lock(m);
            released = true;
        }
        gate.notify_all();
    }

private:
    std::mutex m;
    std::condition_variable cv;
    std::condition_variable gate;
    bool armed = false;
    bool leader_in_get = false;
    bool released = false;
};
}

TEST(CasStoreSingleFlight, ConcurrentResolvesCoalesceToOneHead)
{
    using namespace DB::Cas;

    /// Use GatedHeadBackend throughout. `open` + `publishPart` will call head() some times;
    /// snapshot the counter afterwards and assert that exactly ONE more head() happens during
    /// the concurrent burst (the single-flight leader's shard HEAD).
    auto b = std::make_shared<GatedHeadBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    /// Arm the gate: the NEXT head() call (the leader's shard HEAD) will block until release().
    b->arm();
    const uint64_t calls_before = b->headCalls();

    const RootNamespace ns{"srv1/tbl"};
    constexpr int followers = 8;
    std::vector<std::thread> threads;
    std::vector<std::optional<Resolved>> results(followers + 1);

    threads.emplace_back([&] { results[0] = s->resolveRef(ns, "part_1"); });
    b->waitLeaderInHead();
    for (int i = 0; i < followers; ++i)
        threads.emplace_back([&, i] { results[i + 1] = s->resolveRef(ns, "part_1"); });

    b->release();
    for (auto & t : threads)
        t.join();

    /// Single-flight: exactly ONE head() must have fired during the concurrent burst.
    EXPECT_EQ(b->headCalls() - calls_before, 1u);
    for (const auto & r : results)
    {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->manifest_id, results[0]->manifest_id);
    }
}

/// ---------- Pillar B TTL decode cache (Task 3): opt-in bounded-TTL warm-hit path ----------

TEST(CasStoreDecodeTtl, WarmHitWithinTtlSkipsHead)
{
    using namespace DB::Cas;
    /// TTL of 60 s — easily satisfied in any test run.
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .shard_decode_cache_ttl_ms = std::chrono::milliseconds{60000}});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    /// Prime the cache: one force-fresh resolve (allow_stale=false) so the entry exists and
    /// validated_at is set.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    b->resetCounts();

    /// Warm hit within TTL: MUST skip both HEAD and GET.
    ASSERT_TRUE(s->resolveRef(ns, "part_1", /*allow_stale=*/true).has_value());
    EXPECT_EQ(b->headTotal(), 0u);   /// no HEAD
    EXPECT_EQ(b->getTotal(), 0u);    /// no GET
}

TEST(CasStoreDecodeTtl, ForceFreshAlwaysHeads)
{
    using namespace DB::Cas;
    auto b = std::make_shared<DB::Cas::tests::CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .shard_decode_cache_ttl_ms = std::chrono::milliseconds{60000}});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    /// Prime the cache.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    b->resetCounts();

    /// Force-fresh resolve (allow_stale defaults false): MUST HEAD regardless of cached TTL.
    /// Single-flight guarantees exactly one HEAD per miss; resolving one ref touches one shard.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_EQ(b->headTotal(), 1u);   /// exactly one HEAD
}

TEST(CasStore, MountpointObjectRoundTrip)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    auto store = DB::Cas::Store::open(b, DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    const String key = "srv1/clickhouse_access_check_abc";
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
    store->putMountpointObject(key, "probe-bytes");
    auto got = store->getMountpointObject(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "probe-bytes");
    store->removeMountpointObject(key);
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
}

/// Read-your-writes coherence (B157): an in-flight reader whose GET snapshots a shard manifest that
/// PREDATES a concurrent publish must NOT poison the TTL decode cache with that stale decode after
/// the publish's invalidation erase has already run — otherwise a later allow_stale read serves the
/// stale decode within the TTL window and the just-published ref looks absent (→ "no ref" → the
/// adopted part is wrongly marked broken in the real server).
TEST(CasStoreDecodeTtl, ConcurrentWriteDuringGetDoesNotPoisonStaleEntry)
{
    using namespace DB::Cas;
    auto b = std::make_shared<GatedGetBackend>();
    /// root_shards = 1: every ref maps to shard 0, so publishing part_2 invalidates the SAME shard
    /// the gated reader is decoding for part_1. TTL of 60 s — comfortably covers the test.
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .shard_decode_cache_ttl_ms = std::chrono::milliseconds{60000}});

    const std::string ns_str = "srv1/tbl";
    const RootNamespace ns{ns_str};
    /// Publish part_1 (the shard now holds one ref; publish's casPut also erased the decode cache).
    publishPart(s, ns_str, "part_1", "payload-1");

    /// Arm the gate: the next get() (the reader's leader GET inside loadShardDecoded, after a cache
    /// miss) snapshots the gen-with-part_1 bytes, then blocks.
    b->arm();
    std::optional<Resolved> reader_result;
    std::thread reader([&] { reader_result = s->resolveRef(ns, "part_1", /*allow_stale=*/true); });
    b->waitLeaderInGet();

    /// While the reader's GET is parked on the stale snapshot, publish part_2 to the SAME shard.
    /// casPut commits the gen-with-{part_1,part_2} manifest and invalidates the decode cache.
    publishPart(s, ns_str, "part_2", "payload-2");

    /// Release the reader: its GET returns the stale gen-with-part_1 snapshot (no part_2). Without
    /// the coherence guard it caches that decode AFTER the invalidation erase, poisoning the cache.
    b->release();
    reader.join();
    ASSERT_TRUE(reader_result.has_value());   /// part_1 was always present; the reader still resolves it

    /// THE ASSERTION: a subsequent allow_stale resolve of the just-published part_2 MUST see it.
    /// With the poisoning bug it hits the stale cached decode (no part_2) and returns nullopt.
    EXPECT_TRUE(s->resolveRef(ns, "part_2", /*allow_stale=*/true).has_value())
        << "stale decode poisoned the TTL fast-path: just-published part_2 invisible (B157)";
}

TEST(CasStore, ListMirroredChildren)
{
    using namespace DB::Cas;
    auto b = std::make_shared<InMemoryBackend>();
    auto store = Store::open(b, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    /// Seed two shadow archives by writing a verbatim file into each (creates the prefix in S3).
    store->putNamespaceFile(RootNamespace{"shadow/bk1/store/3f2/3f2a-uuid@cas@"}, "x", "1");
    store->putNamespaceFile(RootNamespace{"shadow/bk2/store/3f2/3f2a-uuid@cas@"}, "x", "1");
    auto children = store->listMirroredChildren("shadow/");
    std::sort(children.begin(), children.end());
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], "bk1");
    EXPECT_EQ(children[1], "bk2");
}

// =====================================================================
// B164b writer-path backpressure tests
// =====================================================================

namespace ProfileEvents
{
extern const Event CasManifestBackpressureCount;
extern const Event CasManifestBackpressureMicroseconds;
extern const Event CasManifestHardLimitExceeded;
}

TEST(CasStoreBackpressure, WriterMutationAboveSoftLimitDelaysThenCommits)
{
    using namespace DB::Cas;
    using namespace std::chrono_literals;

    std::atomic<size_t> delay_count{0};
    std::atomic<uint64_t> total_delayed_ms{0};

    auto b = std::make_shared<InMemoryBackend>();
    /// Very small limits so even a single publish triggers backpressure.
    /// root_shards=128 so many refs spread out to avoid same-shard contention.
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 128,
        .gc_trim_min_events = 0,
        .manifest_soft_limit = 1,    /// tiny: every non-empty shard triggers soft limit
        .manifest_hard_limit = 500,  /// close to soft limit so delay_ms is non-zero
        .manifest_max_delay_ms = 100,
    });

    /// Install a recording delay hook.
    s->setBackpressureDelayHook([&](std::chrono::milliseconds d)
    {
        ++delay_count;
        total_delayed_ms += d.count();
    });

    const RootNamespace ns{"srv1/tbl"};

    /// Publish two refs. At soft_limit=1, the first publish already produces a body > 1 byte
    /// (the encoded shard has at least a header + ref payload). Each publishPart calls both
    /// precommitAdd and promote, each doing one mutateShard → up to 2 delays per publish.
    publishPart(s, ns.string(), "part_1", "hello");
    EXPECT_GT(delay_count.load(), 0u)
        << "backpressure delay should fire on first publish";
    EXPECT_GT(total_delayed_ms.load(), 0u);

    /// A second publish uses a fresh delay budget (one-per-call for each mutateShard).
    const auto count_before = delay_count.load();
    publishPart(s, ns.string(), "part_2", "world");
    EXPECT_GT(delay_count.load(), count_before)
        << "second publish also gets delays (one per mutateShard call)";

    /// Both parts should be visible.
    auto r1 = s->resolveRef(ns, "part_1");
    ASSERT_TRUE(r1.has_value());
    auto r2 = s->resolveRef(ns, "part_2");
    ASSERT_TRUE(r2.has_value());

    /// Metrics counters should reflect the delays.
    using ProfileEvents::global_counters;
    EXPECT_GT(global_counters[ProfileEvents::CasManifestBackpressureCount].load(), 0u);
    EXPECT_GT(global_counters[ProfileEvents::CasManifestBackpressureMicroseconds].load(), 0u);
}

TEST(CasStoreBackpressure, HardLimitBlocksPromoteBeforeCommit)
{
    using namespace DB::Cas;

    auto b = std::make_shared<InMemoryBackend>();
    /// publishPart calls precommitAdd then promote. The hard limit fires during promote
    /// (the encoded body grows past `manifest_hard_limit`). The precommitAdd may have already
    /// committed (size < hard_limit), but the promoting ref must NOT become visible.
    /// This tests the invariant: hard limit throws before the overflowing mutation commits,
    /// not that zero bytes were written on the shard.
    /// Use root_shards=2 so both precommitAdd and promote land on the same shard (shardOf).
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 2,
        .gc_trim_min_events = 0,
        .manifest_soft_limit = 1,          /// tiny
        .manifest_hard_limit = 100,        /// small — promote body (~170) exceeds this
        .manifest_max_delay_ms = 100,
    });

    /// Count delay hook calls; we expect some (precommitAdd may delay), but never commit past hard.
    std::atomic<size_t> delay_calls{0};
    s->setBackpressureDelayHook([&](std::chrono::milliseconds) { ++delay_calls; });

    /// Record baseline hard-limit counter.
    const auto hard_before = ProfileEvents::global_counters[ProfileEvents::CasManifestHardLimitExceeded].load();

    const RootNamespace ns{"srv1/tbl"};

    /// publishPart: precommitAdd appends a journal event (body < 200, may delay), then promote tries
    /// to append another event + ref entry. If the body exceeds 200, it throws.
    /// The hard-limit exception must abort before the overflowing promote mutation can
    /// commit the promoted ref — even though the preceding precommitAdd may have committed.
    ASSERT_THROW(
        publishPart(s, ns.string(), "part_1", "payload_that_makes_the_body_hit_the_hard_limit"),
        DB::Exception);

    /// Hard-limit metric incremented.
    EXPECT_GT(
        ProfileEvents::global_counters[ProfileEvents::CasManifestHardLimitExceeded].load(),
        hard_before);
    /// Nothing was committed (no ref was published).
    EXPECT_FALSE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasStoreBackpressure, GcMutationBypassesBackpressure)
{
    using namespace DB::Cas;

    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 1,
        .gc_trim_min_events = 0,
        .manifest_soft_limit = 1,          /// tiny
        .manifest_hard_limit = 500,        /// close to soft so delay_ms > 0
        .manifest_max_delay_ms = 100,
    });

    std::atomic<size_t> gc_delay_count{0};
    s->setBackpressureDelayHook([&](std::chrono::milliseconds) { ++gc_delay_count; });

    const RootNamespace ns{"srv1/tbl"};

    /// Populate the shard with a publish so it has content.
    /// This WILL trigger the delay hook (Writer origin), but we record the count before GC.
    publishPart(s, ns.string(), "part_1", "content");
    const size_t delays_from_publish = gc_delay_count.load();
    EXPECT_GT(delays_from_publish, 0u);   /// publish should have triggered delays

    /// GC mutation (fence): pass RootMutationOrigin::Gc. The encoded body is above soft_limit=1,
    /// but GC must bypass backpressure — the delay count should NOT increase.
    /// fence writes fence_round into the shard. This is a GC-owned operation.
    s->mutateShardForTest(ns, 0, [&](RootShard & root)
    {
        root.fence_round = 42;
    }, RootMutationOrigin::Gc, RootMutationKind::Fence);

    /// No additional delay was called despite body > soft limit.
    EXPECT_EQ(gc_delay_count.load(), delays_from_publish)
        << "GC mutation must not trigger backpressure delay";

    /// Hard-limit metric — record baseline before this test (shared global counters).
    const auto hard_before_gc = ProfileEvents::global_counters[ProfileEvents::CasManifestHardLimitExceeded].load();

    /// Hard-limit metric unchanged (we didn't hit the hard limit).
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasManifestHardLimitExceeded].load(), hard_before_gc);
}

TEST(CasStoreBackpressure, OneDelayPerCallNotPerAttempt)
{
    using namespace DB::Cas;

    /// Verify that after one delay + retry on a fresh read, the second iteration does NOT delay
    /// again (delayed_once guards against infinite delay).

    std::atomic<size_t> delay_count{0};

    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 1,
        .gc_trim_min_events = 0,
        .manifest_soft_limit = 1,          /// tiny
        .manifest_hard_limit = 500,        /// close to soft so delay_ms > 0
        .manifest_max_delay_ms = 100,
    });

    s->setBackpressureDelayHook([&](std::chrono::milliseconds) { ++delay_count; });

    const RootNamespace ns{"srv1/tbl"};

    /// publishPart calls precommitAdd + promote = 2 mutateShard calls, each with its own
    /// delayed_once flag. So each publishPart causes exactly 2 delays.
    /// After the first publishPart → 2 delays.
    const auto b4 = delay_count.load();
    publishPart(s, ns.string(), "part_1", "hello");
    EXPECT_EQ(delay_count.load(), b4 + 2)
        << "exactly 2 delays per publish (precommitAdd + promote), one each";

    /// A second consecutive publish (shard body still above soft limit) also delays exactly twice.
    publishPart(s, ns.string(), "part_2", "world");
    EXPECT_EQ(delay_count.load(), b4 + 4)
        << "second publish: exactly 2 more delays";

    /// Verify the publishes succeeded.
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
    ASSERT_TRUE(s->resolveRef(ns, "part_2").has_value());
}

TEST(CasStoreBackpressure, ZeroMaxDelayDoesNotDelay)
{
    using namespace DB::Cas;

    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 1,
        .gc_trim_min_events = 0,
        .manifest_soft_limit = 1,          /// tiny
        .manifest_hard_limit = 1ULL << 20,
        .manifest_max_delay_ms = 0,        /// 0 = backpressure disabled
    });

    bool hook_called = false;
    s->setBackpressureDelayHook([&](std::chrono::milliseconds) { hook_called = true; });

    const RootNamespace ns{"srv1/tbl"};
    publishPart(s, ns.string(), "part_1", "hello");

    EXPECT_FALSE(hook_called) << "with manifest_max_delay_ms=0, no delay should occur";
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
}

TEST(CasStoreBackpressure, BackpressureRequiresHardGtSoft)
{
    using namespace DB::Cas;

    /// When hard_limit == soft_limit, backpressure is inactive (delay would be division by zero).
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 1,
        .gc_trim_min_events = 0,
        .manifest_soft_limit = 1000,
        .manifest_hard_limit = 1000,       /// equal — backpressure inactive
        .manifest_max_delay_ms = 100,
    });

    bool hook_called = false;
    s->setBackpressureDelayHook([&](std::chrono::milliseconds) { hook_called = true; });

    const RootNamespace ns{"srv1/tbl"};
    publishPart(s, ns.string(), "part_1", "hello");

    EXPECT_FALSE(hook_called) << "with soft==hard, no backpressure";
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());
}

/// Task 2: incarnation stamp at shard birth.
///
/// When the shard object does not yet exist (create-if-absent path in `mutateShard`),
/// the birth incarnation passed by the caller must be stamped into `RootShard::incarnation`
/// before the mutate callback runs.  Subsequent mutations must leave it untouched.
TEST(CasStore, ShardBornCarriesIncarnation)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 4,
        .gc_trim_min_events = 0,
    });
    const Layout & layout = s->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// Shard 0 does not exist yet — the first mutate is a create-if-absent.
    /// Pass a non-zero birth incarnation; verify it is stamped on the created shard.
    s->mutateShardForTest(ns, 0, [](RootShard &) {},
        RootMutationOrigin::Writer, RootMutationKind::Precommit,
        ShardIncarnation{.writer_epoch = 9, .build_sequence = 2});

    const auto got = b->get(layout.rootShardKey(ns, 0));
    ASSERT_TRUE(got.has_value()) << "shard must have been created";
    const RootShard root = decodeRootShard(got->bytes);
    EXPECT_EQ(root.incarnation.writer_epoch,   9u);
    EXPECT_EQ(root.incarnation.build_sequence, 2u);
}

/// Task 2 INC-MONO test.
///
/// Confirm that after the shard object is reclaimed (deleted from the backend — simulating Task 6
/// reclaim that does not exist yet) and then recreated via a new `precommitAdd`-style mutation, the
/// reborn shard carries a STRICTLY GREATER incarnation than the first.
///
/// INC-MONO decision: within a single Store process `writer_epoch` is constant and `build_sequence`
/// is strictly-increasing (see `allocateBuildSeq`).  Across process restarts `allocateWriterEpoch`
/// provides a durable-monotone `writer_epoch`, so a fresh process always opens with a higher epoch.
/// Therefore `ShardIncarnation{writer_epoch, build_seq}` is a safe incarnation source — strictly
/// increasing per `(ns, shard)` path on every reclaim-and-recreate event.  No dedicated sticky
/// counter is needed.
TEST(CasStore, RebornShardIncarnationStrictlyGreater)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = Store::open(b, PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 4,
        .gc_trim_min_events = 0,
    });
    const Layout & layout = s->layout();
    const RootNamespace ns{"00/aa@cas@"};

    /// First birth: shard does not exist.
    s->mutateShardForTest(ns, 0, [](RootShard &) {},
        RootMutationOrigin::Writer, RootMutationKind::Precommit,
        ShardIncarnation{.writer_epoch = 9, .build_sequence = 2});

    const auto first_got = b->get(layout.rootShardKey(ns, 0));
    ASSERT_TRUE(first_got.has_value());
    const ShardIncarnation first_inc = decodeRootShard(first_got->bytes).incarnation;
    EXPECT_EQ(first_inc.writer_epoch,   9u);
    EXPECT_EQ(first_inc.build_sequence, 2u);

    /// Simulate reclaim (Task 6 does not exist yet): delete the shard object exactly, as
    /// GC reclaim will do when it lands.
    ASSERT_EQ(b->deleteExact(layout.rootShardKey(ns, 0), first_got->token).kind, DeleteOutcome::Kind::Deleted);

    /// Second birth (higher build_sequence within the same writer_epoch): create-if-absent again.
    s->mutateShardForTest(ns, 0, [](RootShard &) {},
        RootMutationOrigin::Writer, RootMutationKind::Precommit,
        ShardIncarnation{.writer_epoch = 9, .build_sequence = 3});

    const auto reborn_got = b->get(layout.rootShardKey(ns, 0));
    ASSERT_TRUE(reborn_got.has_value());
    const ShardIncarnation reborn_inc = decodeRootShard(reborn_got->bytes).incarnation;
    EXPECT_EQ(reborn_inc.writer_epoch,   9u);
    EXPECT_EQ(reborn_inc.build_sequence, 3u);

    /// The INC-MONO invariant: the reborn incarnation must be strictly greater (lexicographic).
    EXPECT_LT(first_inc, reborn_inc)
        << "INC-MONO: reborn shard incarnation must be strictly greater than the first";
}

/// ==== ack-floor beat (spec 2026-07-02-cas-gc-ack-floor-fence-redesign) ====

namespace
{

/// Delegating backend whose `get` of one armed key throws — drives the beat's fail-closed ack path.
class GetFailingBackend final : public DB::Cas::Backend
{
public:
    explicit GetFailingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    String fail_key;   /// empty = fault disarmed

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override
    {
        if (!fail_key.empty() && k == fail_key)
            throw DB::Exception(DB::ErrorCodes::FILE_DOESNT_EXIST, "injected gc/state read fault");
        return inner->get(k, r);
    }
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, b, m); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsentStream(k, m); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, b, e, m); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, b, e, m); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }

private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

}

TEST(CasStoreBeat, AckAdvancesOnlyAfterViewLoad)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    /// Publish gc/state at round 3 (empty retired list); the next beat must load and advertise it.
    GcState st;
    st.round = 3;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));
    store->renewWatermarkOnce();

    const auto got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(decodeMountLease(got->bytes).observed_gc_round, 3u);
    EXPECT_EQ(store->retireView().round(), 3u);
}

TEST(CasLeaseViewDecouple, RenewAdvertisesInstalledRoundNotPublished)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    /// Publish a NEWER round but do NOT sync the view. The isolated renewal path must advertise the
    /// round the view is currently INSTALLED at (0), proving it does not load the view.
    GcState st;
    st.round = 5;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    store->renewLeaseOnlyForTest();

    const auto got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got.has_value());
    const MountLease after_renew = decodeMountLease(got->bytes);
    EXPECT_EQ(after_renew.observed_gc_round, 0u) << "renewal must advertise the installed round, not the published one";
    EXPECT_GT(after_renew.seq, 1u) << "the lease was renewed (seq advanced)";
    EXPECT_EQ(store->retireView().round(), 0u) << "renewal must not install a newer view";

    /// A sync installs round 5; the NEXT isolated renewal then advertises it.
    store->syncRetiredView();
    EXPECT_EQ(store->retireView().round(), 5u);
    store->renewLeaseOnlyForTest();
    const auto got2 = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got2.has_value());
    EXPECT_EQ(decodeMountLease(got2->bytes).observed_gc_round, 5u);
}

TEST(CasLeaseViewDecouple, RenewWatermarkOnceComposesSyncThenRenew)
{
    /// renewWatermarkOnce is the composed test driver: it syncs the view THEN renews, so a single
    /// call still makes observed_gc_round follow the freshly-published round (the contract the GC
    /// pipeline tests rely on).
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    GcState st;
    st.round = 7;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    store->renewWatermarkOnce();

    const auto got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(decodeMountLease(got->bytes).observed_gc_round, 7u);
    EXPECT_EQ(store->retireView().round(), 7u);
}

TEST(CasStoreBeat, ViewAdvanceEmitsRetiredViewAdvanceEvent)
{
    /// Introspection (copy-forward Task 3): a beat that INSTALLS a newer retired view emits exactly
    /// one `retired_view_advance` event carrying the installed round, the prior round, and the loaded
    /// retired entry count; a beat that observes nothing new is silent.
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);
    std::vector<CasEvent> seen;
    store->setEventSink([&](const CasEvent & e){ seen.push_back(e); });

    GcState st;
    st.round = 3;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));
    store->renewWatermarkOnce();

    size_t beats = 0;
    for (const CasEvent & e : seen)
        if (e.type == CasEventType::RetiredViewAdvance)
        {
            ++beats;
            EXPECT_EQ(e.round, 3u);
            EXPECT_EQ(e.detail.at("from_round"), "0");
            EXPECT_EQ(e.detail.at("retired_entries"), "0");
        }
    EXPECT_EQ(beats, 1u);

    /// Nothing new published: the next beat installs nothing and emits nothing.
    seen.clear();
    store->renewWatermarkOnce();
    for (const CasEvent & e : seen)
        EXPECT_NE(e.type, CasEventType::RetiredViewAdvance) << "an unchanged view must not emit retired_view_advance";
    store->setEventSink(nullptr);
}

TEST(CasStoreBeat, GcStateReadFailureLeavesAckUnchanged)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto failing = std::make_shared<GetFailingBackend>(inner);
    auto store = DB::Cas::Store::open(failing,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0});

    GcState st;
    st.round = 2;
    inner->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));
    store->renewWatermarkOnce();
    const auto before_got = inner->get(store->layout().mountKey("test"));
    ASSERT_TRUE(before_got.has_value());
    const MountLease before = decodeMountLease(before_got->bytes);
    EXPECT_EQ(before.observed_gc_round, 2u);

    /// Arm the fault: gc/state unreadable. The beat still renews the lease (seq advances), but the
    /// ack must not move — an ack may never claim a view that was not actually loaded.
    failing->fail_key = store->layout().gcStateKey();
    store->renewWatermarkOnce();
    const auto after_got = inner->get(store->layout().mountKey("test"));
    ASSERT_TRUE(after_got.has_value());
    const MountLease after = decodeMountLease(after_got->bytes);
    EXPECT_EQ(after.observed_gc_round, 2u);
    EXPECT_EQ(after.seq, before.seq + 1);
}

namespace
{

/// Delegating backend that fences the mount slot IN PLACE the first time a `get` returns a present
/// body for the armed key — reproducing the S13 window: the GC's token-guarded fence-out lands
/// between the keeper adopt's GET and its CAS. The caller's subsequent token-guarded `putOverwrite`
/// then fails `PreconditionFailed`, the adopt re-reads, sees `gc_fenced`, and throws
/// `MountFencedException` — which `Store::open`'s fence-recovery loop must turn into a fresh-epoch
/// retry rather than a permanent wedge (P3.1 vector C).
class FenceInAdoptWindowBackend final : public DB::Cas::Backend
{
public:
    explicit FenceInAdoptWindowBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    String fence_key;   /// empty = fault disarmed; set to the mount key to arm the one-shot fence

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override
    {
        auto got = inner->get(k, r);
        if (!fence_key.empty() && k == fence_key && got.has_value())
        {
            /// One-shot: fence the slot in place exactly as `computeHeartbeatFloor` does (preserve the
            /// body, gc_fenced = true, seq + 1, token-guarded against the value we just read), then
            /// disarm so the retry can adopt cleanly.
            DB::Cas::MountLease fenced = DB::Cas::decodeMountLease(got->bytes);
            fenced.gc_fenced = true;
            fenced.seq += 1;
            inner->putOverwrite(k, DB::Cas::encodeMountLease(fenced), got->token);
            fence_key.clear();
        }
        return got;
    }
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsent(k, b, m); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & m = {}) override { return inner->putIfAbsentStream(k, m); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->putOverwrite(k, b, e, m); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & m = {}) override { return inner->casPut(k, b, e, m); }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }

private:
    std::shared_ptr<DB::Cas::Backend> inner;
};

}

TEST(CasStoreMountFence, OpenRecoversFromFenceInAdoptWindowWithFreshEpoch)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto fencing = std::make_shared<FenceInAdoptWindowBackend>(inner);
    /// Arm the one-shot fence on the mount slot. Store::open first claims the mount (fresh mint), then
    /// the keeper adopts it — the adopt's GET trips the fence, its CAS fails, and open must recover.
    const DB::Cas::Layout layout("p");
    fencing->fence_key = layout.mountKey("test");

    DB::Cas::StorePtr store;
    ASSERT_NO_THROW(
        store = DB::Cas::Store::open(fencing,
            DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0}))
        << "open must recover from a fence in the adopt window, not wedge (exit-49 S13 bug)";
    ASSERT_TRUE(store);

    /// The final live lease is unfenced and at a HIGHER writer_epoch than the first attempt (a fence
    /// costs an epoch): the first claim took epoch 1, got fenced, the retry took epoch 2 and mounted.
    const auto got = inner->get(layout.mountKey("test"));
    ASSERT_TRUE(got.has_value());
    const MountLease final_lease = decodeMountLease(got->bytes);
    EXPECT_FALSE(final_lease.gc_fenced);
    EXPECT_GT(final_lease.writer_epoch, 1u) << "recovery must draw a fresh writer_epoch";
    EXPECT_TRUE(fencing->fence_key.empty()) << "the one-shot fence must have fired";
}

TEST(CasStoreBeat, DrainBlocksAckWhileMutationInFlight)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    GcState st;
    st.round = 1;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    std::promise<void> entered, release;
    std::atomic<bool> released{false};

    std::thread mutator([&]
    {
        store->mutateShardForTest(RootNamespace{"ns"}, 0, [&](RootShard &)
        {
            entered.set_value();
            release.get_future().wait();
        }, RootMutationOrigin::Writer, RootMutationKind::Promote);
    });
    entered.get_future().wait();

    /// The beat must park on the drain while the mutation is in flight (the mutation holds the
    /// shared side of the view gate for its whole call).
    auto beat = std::async(std::launch::async, [&]
    {
        const uint64_t r = store->syncRetiredView();
        /// Order proof: by the time the beat returns, the mutation MUST have been released — the
        /// drain forbids installing a view over an in-flight old-view mutation.
        EXPECT_TRUE(released.load());
        return r;
    });

    /// Bounded negative wait: this asserts the blocking occurred (it is an assertion on an
    /// intentionally-parked thread, not a sleep papering over a race).
    ASSERT_EQ(beat.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);

    released.store(true);
    release.set_value();
    mutator.join();
    EXPECT_EQ(beat.get(), 1u);
    EXPECT_EQ(store->retireView().round(), 1u);
}

/// Task 12: the write-fence deadline is a CLOCK_BOOTTIME instant (boottime includes VM-suspend time,
/// so a resumed sleeper sees its fence expired — unlike CLOCK_MONOTONIC, which freezes across suspend).
/// A CLOCK_MONOTONIC freeze cannot be simulated in a unit test, so we exercise the injected-fn seam: a
/// fake boot clock that we advance past the ttl must flip mayMutate to false and make a gated mutate
/// fail closed with ABORTED.
TEST(CasStore, WriteFenceUsesInjectedBootClock)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t fake_boot = 1'000'000;   /// arbitrary boottime origin (ms)
    auto store = DB::Cas::Store::open(backend, DB::Cas::PoolConfig{
        .pool_prefix = "p",
        .server_root_id = "test",
        .root_shards = 1,
        .gc_trim_min_events = 0,
        .mount_lease_ttl_ms = std::chrono::milliseconds(30000),
        .boot_ms_fn = [&] { return fake_boot; },
    });

    /// Freshly armed at open (deadline = fake_boot + ttl): well within the ttl, mutations are allowed.
    EXPECT_TRUE(store->mayMutate());
    EXPECT_NO_THROW(store->mutateShardForTest(RootNamespace{"ns"}, 0, [](RootShard &) {},
        RootMutationOrigin::Writer, RootMutationKind::Promote));

    /// Advance the boot clock just short of the deadline — still armed.
    fake_boot += 29999;
    EXPECT_TRUE(store->mayMutate());

    /// Cross the deadline (ttl elapsed with no renew — a resumed sleeper's view). The fence expires and
    /// a gated mutate must fail closed with ABORTED.
    fake_boot += 2;   /// now fake_boot = origin + 30001 > origin + 30000
    EXPECT_FALSE(store->mayMutate());
    EXPECT_THROW(store->mutateShardForTest(RootNamespace{"ns"}, 0, [](RootShard &) {},
        RootMutationOrigin::Writer, RootMutationKind::Promote), DB::Exception);
}

/// ==== self-remount after GC fence-out (liveness counterpart of the fence-out safety rule) ====

namespace
{

/// GC's fence-out, applied directly: preserve the body, set gc_fenced, bump seq (token-guarded).
void fenceOutMount(DB::Cas::Backend & backend, const String & mount_key)
{
    const auto got = backend.get(mount_key);
    ASSERT_TRUE(got.has_value());
    MountLease m = decodeMountLease(got->bytes);
    m.gc_fenced = true;
    m.seq += 1;
    ASSERT_EQ(backend.putOverwrite(mount_key, encodeMountLease(m), got->token).outcome,
              DB::Cas::PutOutcome::Done);
}

}

TEST(CasStoreRemount, FenceOutThenSelfRemountRestoresWrites)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);
    const String mount_key = store->layout().mountKey("test");
    const uint64_t epoch_before = decodeMountLease(backend->get(mount_key)->bytes).writer_epoch;
    EXPECT_EQ(store->liveWriterEpoch(), epoch_before);

    fenceOutMount(*backend, mount_key);

    /// The keeper's next renewal fails closed (foreign touch — never re-mint).
    EXPECT_THROW(store->renewWatermarkOnce(), DB::Exception);

    /// Self-remount claims a FRESH incarnation: epoch bumped, gc_fenced cleared, writes restored.
    ASSERT_TRUE(store->tryRemountOnce());
    const MountLease after = decodeMountLease(backend->get(mount_key)->bytes);
    EXPECT_EQ(after.writer_epoch, epoch_before + 1);
    EXPECT_FALSE(after.gc_fenced);
    EXPECT_EQ(store->liveWriterEpoch(), epoch_before + 1);

    /// The renewal path works again (the new keeper owns the slot)...
    EXPECT_NO_THROW(store->renewWatermarkOnce());
    /// ...and so does a ref-shard mutation.
    EXPECT_NO_THROW(store->mutateShardForTest(RootNamespace{"ns"}, 0, [](RootShard &) {},
                                              RootMutationOrigin::Writer, RootMutationKind::Publish));
}

TEST(CasStoreRemount, OldEpochBuildFailsClosedAfterRemount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);
    auto build = store->startBuild({});

    fenceOutMount(*backend, store->layout().mountKey("test"));
    ASSERT_TRUE(store->tryRemountOnce());

    /// The build was minted under the superseded incarnation — every further step fails closed.
    expectThrowsCode(DB::ErrorCodes::ABORTED,
        [&] { build->putBlob(DB::Cas::tests::idOf("x"), DB::Cas::BlobSource::fromString("x")); });

    /// A FRESH build under the live incarnation works.
    auto fresh = store->startBuild({});
    EXPECT_NO_THROW(fresh->putBlob(DB::Cas::tests::idOf("y"), DB::Cas::BlobSource::fromString("y")));
}

TEST(CasStoreRemount, ForeignOwnerIsNeverTakenOver)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);
    const String mount_key = store->layout().mountKey("test");

    /// A genuinely foreign uuid holds the mount (live or not — foreign is terminal for the claim).
    const auto got = backend->get(mount_key);
    MountLease foreign = decodeMountLease(got->bytes);
    foreign.server_uuid = foreign.server_uuid + DB::UInt128(1);
    foreign.seq += 1;
    ASSERT_EQ(backend->putOverwrite(mount_key, encodeMountLease(foreign), got->token).outcome,
              DB::Cas::PutOutcome::Done);

    EXPECT_FALSE(store->tryRemountOnce());
    /// The foreign body is untouched (no takeover, ever).
    EXPECT_EQ(decodeMountLease(backend->get(mount_key)->bytes).server_uuid, foreign.server_uuid);
}

/// ==== shard-mutation queue (spec 2026-07-03-cas-shard-mutation-queue) ====

namespace
{

/// Deterministic co-batching harness: get(target_key) BLOCKS while armed (signalling arrival), so a
/// test can enqueue more mutations while the leader sits inside its flush's first read. The carve
/// happens AFTER that read by design — everything enqueued during the read joins the batch.
class BlockingGetBackend final : public DB::Cas::Backend
{
public:
    explicit BlockingGetBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}

    void arm(const String & key)
    {
        std::lock_guard g(m);
        target = key;
        armed = true;
        entered = false;
    }
    void awaitEntered()
    {
        std::unique_lock lk(m);
        cv.wait(lk, [&] { return entered; });
    }
    void release()
    {
        std::lock_guard g(m);
        armed = false;
        cv.notify_all();
    }

    std::optional<DB::Cas::GetResult> get(const String & k, DB::Cas::Range r = {}) override
    {
        {
            std::unique_lock lk(m);
            if (armed && k == target)
            {
                entered = true;
                cv.notify_all();
                cv.wait(lk, [&] { return !armed; });
            }
        }
        return inner->get(k, r);
    }
    std::optional<DB::Cas::GetStreamResult> getStream(const String & k, DB::Cas::Range r = {}) override { return inner->getStream(k, r); }
    DB::Cas::HeadResult head(const String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const String & p, const String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutResult putIfAbsent(const String & k, const String & b, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsent(k, b, meta); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const String & k, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putIfAbsentStream(k, meta); }
    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e, const DB::Cas::ObjectMeta & meta = {}) override { return inner->putOverwrite(k, b, e, meta); }
    DB::Cas::CasResult casPut(const String & k, const String & b, const std::optional<DB::Cas::Token> & e, const DB::Cas::ObjectMeta & meta = {}) override
    {
        /// Count REF-SHARD traffic only: Store bootstrap (owner/epoch adopt) and mount renewals
        /// also casPut, and their adopt-existing Conflicts are normal — not the queue's concern.
        const bool is_ref_shard = k.find("cas/refs/") != String::npos;
        if (is_ref_shard)
            ++cas_puts;
        if (is_ref_shard && cas_conflict_hook)
            cas_conflict_hook();
        const auto res = inner->casPut(k, b, e, meta);
        if (is_ref_shard && res.outcome == DB::Cas::CasOutcome::Conflict)
            ++cas_conflicts;
        return res;
    }
    DB::Cas::DeleteOutcome deleteExact(const String & k, const DB::Cas::Token & t) override { return inner->deleteExact(k, t); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }

    std::atomic<size_t> cas_puts{0};
    std::atomic<size_t> cas_conflicts{0};
    std::function<void()> cas_conflict_hook;   /// runs BEFORE forwarding each casPut

private:
    std::shared_ptr<DB::Cas::Backend> inner;
    std::mutex m;
    std::condition_variable cv;
    String target;
    bool armed = false;
    bool entered = false;
};

/// Append one journal event under `name` — the minimal Ref-scoped mutation for queue tests.
std::function<void(RootShard &)> appendEventFor(const String & name)
{
    return [name](RootShard & root)
    {
        root.journal.push_back(RootOwnerEvent{
            .transition_version = root.shard_version + 1,
            .old_binding = std::nullopt,
            .new_binding = OwnerBinding{.owner_kind = OwnerKind::Committed, .ref_name = name,
                                        .build_id = UInt128(0), .manifest_ref = ManifestRef{1, 1, 1}},
            .is_tombstone = false});
    };
}

}

/// Two concurrent mutations of DIFFERENT refs on one shard co-batch into ONE casPut (the second
/// arrives while the leader sits in the flush's first read; the carve runs after that read).
TEST(CasShardQueue, CoBatchesTwoRefsIntoOneCasPut)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto blocking = std::make_shared<BlockingGetBackend>(inner);
    auto store = DB::Cas::Store::open(blocking,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0});
    const RootNamespace ns{"srv1/tbl"};
    /// Materialize the shard first so the batch path is the common existing-shard one.
    store->mutateShardScopedForTest(ns, 0, MutationScope::ref("seed"), appendEventFor("seed"));

    const String shard_key = store->layout().rootShardKey(ns, 0);
    blocking->cas_puts = 0;
    blocking->arm(shard_key);

    uint64_t v_a = 0;
    std::thread t_a([&] { v_a = store->mutateShardScopedForTest(ns, 0, MutationScope::ref("part_a"), appendEventFor("part_a")); });
    blocking->awaitEntered();   /// leader (t_a) is inside the flush read; enqueue a second mutation
    uint64_t v_b = 0;
    std::thread t_b([&] { v_b = store->mutateShardScopedForTest(ns, 0, MutationScope::ref("part_b"), appendEventFor("part_b")); });
    /// Deterministic co-batching: the leader's item stays in the queue until the carve (which runs
    /// AFTER the blocked read), so depth 2 == t_b is enqueued and will join the leader's batch.
    while (store->shardQueuePendingForTest(ns, 0) < 2)
        std::this_thread::yield();
    blocking->release();
    t_a.join();
    t_b.join();

    EXPECT_EQ(blocking->cas_puts.load(), 1u) << "both mutations must land in ONE casPut";
    EXPECT_EQ(blocking->cas_conflicts.load(), 0u);
    ASSERT_NE(v_a, 0u);
    ASSERT_NE(v_b, 0u);
    EXPECT_EQ(std::max(v_a, v_b), std::min(v_a, v_b) + 1) << "distinct consecutive transition versions";

    const auto root = decodeRootShard(inner->get(store->layout().rootShardKey(ns, 0))->bytes);
    EXPECT_EQ(root.shard_version, std::max(v_a, v_b));
}

/// The scope rule: two mutations of the SAME ref never share a flush — the second goes to the next
/// casPut (per-ref durable histories stay identical to the unbatched protocol).
TEST(CasShardQueue, SameRefMutationsSplitAcrossFlushes)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto blocking = std::make_shared<BlockingGetBackend>(inner);
    auto store = DB::Cas::Store::open(blocking,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0});
    const RootNamespace ns{"srv1/tbl"};
    store->mutateShardScopedForTest(ns, 0, MutationScope::ref("seed"), appendEventFor("seed"));

    const String shard_key = store->layout().rootShardKey(ns, 0);
    blocking->cas_puts = 0;
    blocking->arm(shard_key);

    std::thread t_a([&] { store->mutateShardScopedForTest(ns, 0, MutationScope::ref("part_x"), appendEventFor("part_x")); });
    blocking->awaitEntered();
    std::thread t_b([&] { store->mutateShardScopedForTest(ns, 0, MutationScope::ref("part_x"), appendEventFor("part_x")); });
    while (store->shardQueuePendingForTest(ns, 0) < 2)
        std::this_thread::yield();
    blocking->release();
    t_a.join();
    t_b.join();

    EXPECT_EQ(blocking->cas_puts.load(), 2u) << "same-ref mutations must flush separately (scope cut)";
    const auto root = decodeRootShard(inner->get(store->layout().rootShardKey(ns, 0))->bytes);
    size_t events_for_x = 0;
    for (const auto & e : root.journal)
        if (e.new_binding && e.new_binding->ref_name == "part_x")
            ++events_for_x;
    EXPECT_EQ(events_for_x, 2u);
}

/// A throwing closure is isolated: its caller gets the exception, the co-batched neighbor lands,
/// and the journal contains ONLY the survivor's event (snapshot restore).
TEST(CasShardQueue, ThrowingClosureIsIsolatedFromBatch)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto blocking = std::make_shared<BlockingGetBackend>(inner);
    auto store = DB::Cas::Store::open(blocking,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0});
    const RootNamespace ns{"srv1/tbl"};
    store->mutateShardScopedForTest(ns, 0, MutationScope::ref("seed"), appendEventFor("seed"));

    const String shard_key = store->layout().rootShardKey(ns, 0);
    blocking->arm(shard_key);

    std::exception_ptr thrown;
    std::thread t_a([&]
    {
        try
        {
            store->mutateShardScopedForTest(ns, 0, MutationScope::ref("bad"), [](RootShard & root)
            {
                root.journal.push_back(RootOwnerEvent{.transition_version = root.shard_version + 1,
                    .old_binding = std::nullopt, .new_binding = std::nullopt, .is_tombstone = false});
                throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "validation failed after partial edit");
            });
        }
        catch (...)
        {
            thrown = std::current_exception();
        }
    });
    blocking->awaitEntered();
    std::thread t_b([&] { store->mutateShardScopedForTest(ns, 0, MutationScope::ref("good"), appendEventFor("good")); });
    while (store->shardQueuePendingForTest(ns, 0) < 2)
        std::this_thread::yield();
    blocking->release();
    t_a.join();
    t_b.join();

    EXPECT_TRUE(thrown != nullptr) << "the throwing closure's caller must receive the exception";
    const auto root = decodeRootShard(inner->get(store->layout().rootShardKey(ns, 0))->bytes);
    size_t tombstoneless_null_events = 0;
    size_t good_events = 0;
    for (const auto & e : root.journal)
    {
        if (!e.old_binding && !e.new_binding && !e.is_tombstone)
            ++tombstoneless_null_events;
        if (e.new_binding && e.new_binding->ref_name == "good")
            ++good_events;
    }
    EXPECT_EQ(tombstoneless_null_events, 0u) << "the failed closure's partial edit must be rolled back";
    EXPECT_EQ(good_events, 1u) << "the innocent co-batched mutation must land";
}

/// A cross-writer CAS conflict (the only kind left: e.g. the GC leader on another replica) replays
/// the whole batch once — every mutation lands exactly once.
TEST(CasShardQueue, ConflictReplaysBatchExactlyOnce)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto blocking = std::make_shared<BlockingGetBackend>(inner);
    auto store = DB::Cas::Store::open(blocking,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1, .gc_trim_min_events = 0});
    const RootNamespace ns{"srv1/tbl"};
    store->mutateShardScopedForTest(ns, 0, MutationScope::ref("seed"), appendEventFor("seed"));
    const String shard_key = store->layout().rootShardKey(ns, 0);

    /// Foreign write between the leader's read and its casPut: the hook fires before the FIRST
    /// casPut forward and displaces the token once.
    bool fired = false;
    blocking->cas_conflict_hook = [&]
    {
        if (fired)
            return;
        fired = true;
        const auto got = inner->get(shard_key);
        ASSERT_TRUE(got.has_value());
        inner->putOverwrite(shard_key, got->bytes, got->token);   /// token displaced => leader's CAS conflicts
    };

    blocking->cas_puts = 0;
    store->mutateShardScopedForTest(ns, 0, MutationScope::ref("part_c"), appendEventFor("part_c"));
    blocking->cas_conflict_hook = nullptr;

    EXPECT_EQ(blocking->cas_conflicts.load(), 1u);
    EXPECT_EQ(blocking->cas_puts.load(), 2u) << "one conflicted attempt + one committed replay";
    const auto root = decodeRootShard(inner->get(store->layout().rootShardKey(ns, 0))->bytes);
    size_t c_events = 0;
    for (const auto & e : root.journal)
        if (e.new_binding && e.new_binding->ref_name == "part_c")
            ++c_events;
    EXPECT_EQ(c_events, 1u) << "replay must not duplicate the event";
}

/// Stress: many threads over few shards — every mutation lands exactly once, versions are dense,
/// and intra-server conflicts are structurally IMPOSSIBLE (one leader per shard at a time).
TEST(CasShardQueue, StressNoConflictsNoLostMutations)
{
    auto inner = std::make_shared<InMemoryBackend>();
    auto blocking = std::make_shared<BlockingGetBackend>(inner);
    auto store = DB::Cas::Store::open(blocking,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 4, .gc_trim_min_events = 0});
    const RootNamespace ns{"srv1/tbl"};

    constexpr size_t kThreads = 16;
    constexpr size_t kPerThread = 50;
    std::vector<std::thread> threads;
    std::atomic<size_t> failures{0};
    for (size_t t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t]
        {
            for (size_t i = 0; i < kPerThread; ++i)
            {
                const String name = "p" + std::to_string(t) + "_" + std::to_string(i);
                const uint64_t shard = (t * kPerThread + i) % 4;
                try
                {
                    store->mutateShardScopedForTest(ns, shard, MutationScope::ref(name), appendEventFor(name));
                }
                catch (...)
                {
                    ++failures;
                }
            }
        });
    for (auto & th : threads)
        th.join();

    EXPECT_EQ(failures.load(), 0u);
    EXPECT_EQ(blocking->cas_conflicts.load(), 0u) << "a single leader per shard makes intra-server conflicts impossible";
    uint64_t total_events = 0;
    uint64_t total_version = 0;
    for (uint64_t shard = 0; shard < 4; ++shard)
    {
        const auto root = decodeRootShard(inner->get(store->layout().rootShardKey(ns, shard))->bytes);
        total_events += root.journal.size();
        total_version += root.shard_version;
        /// Versions dense: every mutation bumped exactly once.
        EXPECT_EQ(root.shard_version, root.journal.empty() ? root.shard_version : root.journal.back().transition_version);
    }
    EXPECT_EQ(total_events, kThreads * kPerThread);
    EXPECT_EQ(total_version, kThreads * kPerThread);
    EXPECT_LE(blocking->cas_puts.load(), kThreads * kPerThread);
}

TEST(CasRetiredViewSyncer, StartStopIsCleanNoOp)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);
    /// Starting then stopping the syncer must be a clean lifecycle: the thread joins, no hang, no throw.
    store->startRetiredViewSync(std::chrono::milliseconds(5));
    store->stopRetiredViewSync();
    store->stopRetiredViewSync();   /// idempotent
    SUCCEED();
}

TEST(CasRetiredViewSyncer, RunningSyncerAdvancesPublishedRound)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = DB::Cas::tests::openStoreForTest(backend);

    GcState st;
    st.round = 4;
    backend->putIfAbsent(store->layout().gcStateKey(), encodeGcState(st));

    store->startRetiredViewSync(std::chrono::milliseconds(2));

    /// Bounded liveness assertion: the running syncer must install round 4 on its own. Poll the real
    /// condition (not a fixed sleep); fail if it never advances within a generous bound.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (store->retireView().round() != 4u && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();

    store->stopRetiredViewSync();
    EXPECT_EQ(store->retireView().round(), 4u) << "the syncer thread must advance the installed round on its own";
}
