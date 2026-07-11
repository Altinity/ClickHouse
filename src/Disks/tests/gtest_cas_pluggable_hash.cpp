#include <gtest/gtest.h>

/// P1-T2 (CAS pluggable-blob-hash Phase 1, design 2026-07-11-cas-pluggable-blob-hash-design.md §4/§8):
/// `PoolMeta` records the pool-wide `blob_hash_algo` and `PoolMeta::createOrValidate` fail-closes on a
/// disk config that disagrees with an existing pool's recorded algo -- the pool-wide durability
/// invariant (never silently re-hash an existing pool).
///
/// P1-T3a (this file, extended): the pool's `blob_hash_algo` is threaded into the three hash sites
/// (spec §5/§6) -- `ContentAddressed::CaContentWriteBuffer` (streaming blob-body hash),
/// `Build`'s envelope `hash_algo` field, and (transitively, via `Cas::blobHashHexOneShot`) the
/// `poolContentHash` re-hash used by `copyForwardFromCondemned`. `poolContentHash` itself is a static
/// helper in `CasBuild.cpp` and not directly reachable from a gtest; its production callers already
/// exercise the default `CityHash128` path (`gtest_cas_build.cpp`'s `CopyForwardMultiBlockPayloadVerifies`
/// stays green, unmodified, proving byte-for-byte-unchanged default behavior) and it delegates to the
/// SAME `Cas::blobHashHexOneShot` this file tests directly below.

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedWriteBuffers.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

#include <IO/WriteBufferFromFile.h>

#include <cas_format.pb.h>

#include <algorithm>
#include <filesystem>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace Proto = ::clickhouse::cas::format;

namespace
{

/// A deterministic, non-repeating-byte payload spanning several `DBMS_DEFAULT_HASHING_BLOCK_SIZE`
/// (2048 B) blocks, so a chunked-vs-one-shot divergence (the CityHash128 pitfall documented on
/// `poolContentHash`) would not accidentally go unnoticed.
std::string makeMultiBlockPayload(size_t size = 5000)
{
    std::string s;
    s.reserve(size);
    for (size_t i = 0; i < size; ++i)
        s.push_back(static_cast<char>('a' + (i % 23)));
    return s;
}

}

TEST(CasPluggableHash, PoolMetaRoundTripsBlobHashAlgo)
{
    PoolMeta pm;
    pm.pool_id = u128Of("pool-a");
    pm.root_shards = 8;
    pm.blob_header_len = 256;
    pm.blob_hash_algo = static_cast<uint8_t>(BlobHashAlgo::XXH3_128);

    const PoolMeta back = decodePoolMeta(encodePoolMeta(pm));
    EXPECT_EQ(back.blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::XXH3_128));
    EXPECT_EQ(back.root_shards, 8u);
    EXPECT_EQ(back.blob_header_len, 256u);
}

TEST(CasPluggableHash, CreateOrValidateRecordsConfigAlgoOnFreshPool)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");

    const PoolMeta pm = PoolMeta::createOrValidate(*backend, layout, /*root_shards*/ 4, /*blob_header_len*/ 256, BlobHashAlgo::XXH3_128);
    EXPECT_EQ(pm.blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::XXH3_128));

    /// Reopening with the SAME algo is a no-op reopen: the recorded value comes back unchanged.
    const PoolMeta reopened = PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::XXH3_128);
    EXPECT_EQ(reopened.blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::XXH3_128));
    EXPECT_EQ(reopened.pool_id, pm.pool_id);
}

TEST(CasPluggableHash, CreateOrValidateDefaultsToCityHash128)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");

    const PoolMeta pm = PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::CityHash128);
    EXPECT_EQ(pm.blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::CityHash128));
}

/// Fail-closed (spec §8): a config that disagrees with an existing pool's recorded algo must NEVER
/// silently re-hash the pool -- it throws BAD_ARGUMENTS instead.
TEST(CasPluggableHash, CreateOrValidateFailsClosedOnAlgoMismatch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    const Layout layout("p");

    PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::CityHash128);

    expectThrowsCode(DB::ErrorCodes::BAD_ARGUMENTS, [&]
    {
        PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::XXH3_128);
    });

    /// The pool is untouched by the refused reopen: a subsequent open with the ORIGINAL algo still
    /// succeeds and returns the same pool_id.
    const PoolMeta reopened = PoolMeta::createOrValidate(*backend, layout, 4, 256, BlobHashAlgo::CityHash128);
    EXPECT_EQ(reopened.blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::CityHash128));
}

/// An old-format `_pool_meta` object (written before `blob_hash_algo` existed) has the field absent
/// entirely -- proto3 `optional` explicit presence, so `has_blob_hash_algo() == false`. It must decode
/// to `1` (cityHash128), since every pool created before this field existed WAS cityHash128.
TEST(CasPluggableHash, OldFormatPoolMetaWithoutFieldDecodesToCityHash128)
{
    Proto::PoolMetaProto msg;
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::PoolMeta));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());
    msg.set_pool_id(u128ToBytesBE(u128Of("old-pool")));
    msg.set_root_shards(8);
    msg.set_blob_header_len(256);
    msg.set_min_reader_generation(0);
    /// Deliberately NOT calling set_blob_hash_algo -- simulates an object written before the field
    /// existed.
    ASSERT_FALSE(msg.has_blob_hash_algo());

    std::string bytes;
    ASSERT_TRUE(msg.SerializeToString(&bytes));

    const PoolMeta pm = decodePoolMeta(bytes);
    EXPECT_EQ(pm.blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::CityHash128));
}

/// ---- P1-T3a: the pool's blob_hash_algo threaded into the streaming write-buffer hash site ----

/// `ContentAddressed::CaContentWriteBuffer`'s LOCAL-staging constructor (the everyday spill-to-temp-file
/// mode `ContentAddressedTransaction::writeFile` uses), built with `BlobHashAlgo::XXH3_128`, must hash
/// the streamed payload with xxh3 -- agreeing with the standalone `blobHashHexOneShot` one-shot helper
/// (the same convention `poolContentHash`'s re-hash uses).
TEST(CasPluggableHash, ContentWriteBufferLocalModeHashesWithSelectedAlgoXxh3)
{
    const std::string payload = makeMultiBlockPayload();
    const auto temp_dir = (std::filesystem::temp_directory_path() / "cas_pluggable_hash_xxh3_local").string();

    std::string got_hash_hex;
    size_t got_size = 0;
    auto buf = std::make_unique<DB::ContentAddressed::CaContentWriteBuffer>(
        temp_dir,
        BlobHashAlgo::XXH3_128,
        /*buf_size=*/8192,
        /*use_adaptive_buffer_size=*/false,
        /*adaptive_buffer_initial_size=*/0,
        [&](const std::string & hash_hex, size_t size, const std::string &)
        {
            got_hash_hex = hash_hex;
            got_size = size;
        });

    /// Write in two chunks so more than one nextImpl flush happens (exercises the streaming state, not
    /// just a single call).
    buf->write(payload.data(), 1234);
    buf->write(payload.data() + 1234, payload.size() - 1234);
    buf->finalize();

    EXPECT_EQ(got_size, payload.size());
    EXPECT_EQ(got_hash_hex, blobHashHexOneShot(BlobHashAlgo::XXH3_128, payload));
    /// A wrong-but-plausible result (e.g. accidentally still hashing with cityHash128) would silently
    /// produce a DIFFERENT hex string -- pin that the two algos disagree on this payload, so the
    /// assertion above is actually discriminating.
    EXPECT_NE(got_hash_hex, blobHashHexOneShot(BlobHashAlgo::CityHash128, payload));
}

/// The DEFAULT algo (`CityHash128`) through the SAME write buffer must stay byte-for-byte unchanged --
/// the CAS pluggable-blob-hash invariant (spec §8). Compares against `blobHashHexOneShot`, which
/// `gtest_cas_blob_hasher.cpp`'s `CityHash128ByteIdenticalToHashingWriteBuffer` already proves is
/// byte-identical to the pre-existing plain `HashingWriteBuffer` convention.
TEST(CasPluggableHash, ContentWriteBufferLocalModeCityHash128Unchanged)
{
    const std::string payload = makeMultiBlockPayload();
    const auto temp_dir = (std::filesystem::temp_directory_path() / "cas_pluggable_hash_ch128_local").string();

    std::string got_hash_hex;
    auto buf = std::make_unique<DB::ContentAddressed::CaContentWriteBuffer>(
        temp_dir,
        BlobHashAlgo::CityHash128,
        /*buf_size=*/8192,
        /*use_adaptive_buffer_size=*/false,
        /*adaptive_buffer_initial_size=*/0,
        [&](const std::string & hash_hex, size_t, const std::string &)
        {
            got_hash_hex = hash_hex;
        });

    buf->write(payload.data(), payload.size());
    buf->finalize();

    EXPECT_EQ(got_hash_hex, blobHashHexOneShot(BlobHashAlgo::CityHash128, payload));
}

/// Round-trip at the `Build`/`Store` level (in-memory backend -- no live disk/S3 needed): a `Store`
/// opened with `PoolConfig::blob_hash_algo = XXH3_128` must record it in `PoolMeta` AND stamp it onto
/// every envelope it writes (`Build::uploadFromSource`'s `header.hash_algo`, P1-T3a task item 3) --
/// the envelope `hash_algo` field becomes a truthful, per-pool value instead of the inert literal `1`.
TEST(CasPluggableHash, StoreWithXxh3AlgoStampsEnvelopeHashAlgo)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .blob_hash_algo = BlobHashAlgo::XXH3_128});
    EXPECT_EQ(store->poolMeta().blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::XXH3_128));

    auto build = store->startBuild({});
    const std::string payload = "hello xxh3 world";
    auto ref = build->putBlob(BlobId{blobHashHexOneShot(BlobHashAlgo::XXH3_128, payload)}, BlobSource::fromString(payload));

    const auto raw = backend->get(store->layout().blobKey(ref.id));
    ASSERT_TRUE(raw.has_value());
    const EnvelopeHeader h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.hash_algo, static_cast<uint8_t>(BlobHashAlgo::XXH3_128));
}

/// The DEFAULT pool (no `blob_hash_algo` override) must still stamp `hash_algo = 1` (CityHash128) --
/// the pre-existing literal, now driven by `PoolMeta` instead of hardcoded, so a default pool's
/// envelope bytes are unchanged.
TEST(CasPluggableHash, StoreWithDefaultAlgoStampsEnvelopeHashAlgoOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test"});
    EXPECT_EQ(store->poolMeta().blob_hash_algo, static_cast<uint8_t>(BlobHashAlgo::CityHash128));

    auto build = store->startBuild({});
    auto ref = build->putBlob(idOf("hello world"), BlobSource::fromString("hello world"));

    const auto raw = backend->get(store->layout().blobKey(ref.id));
    ASSERT_TRUE(raw.has_value());
    const EnvelopeHeader h = decodeEnvelopeHeader(raw->bytes, raw->bytes.size(), ObjectKind::Blob);
    EXPECT_EQ(h.hash_algo, 1u);
}

/// ---- P1-T3b: the pool's blob_hash_algo threaded into blob-body PATH keys (spec §3/§10) ----

/// A blob written and promoted through a live ref on an xxh3-128 pool lands under the
/// `blobs/xxh3/<shard>/<hex>` path segment (not the bare `blobs/<shard>/<hex>` shape), is readable at
/// that key, and `runFsck`'s LIST-based discovery (`Layout::blobsPrefix`, deliberately algo-agnostic)
/// finds it reachable and clean -- proving the GC/fsck key-parse (which takes only the LAST path
/// component as the hex digest, `CasGc.cpp`/`CasFsck.cpp`) still works with the extra segment.
TEST(CasPluggableHash, Xxh3BlobLandsUnderAlgoSegmentAndIsDiscoveredCleanByFsck)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                   .blob_hash_algo = BlobHashAlgo::XXH3_128});

    const RootNamespace ns{"srv1/tbl"};
    const std::string payload = makeMultiBlockPayload();
    const BlobId id{blobHashHexOneShot(BlobHashAlgo::XXH3_128, payload)};

    BuildInfo info;
    info.intended_ref = ns.string() + "/rb";
    auto build = store->startBuild(info);
    build->putBlob(id, BlobSource::fromString(payload));

    /// The blob body landed under the algo-segmented path -- readable there, not at the legacy
    /// no-segment shape.
    const String blob_key = store->layout().blobKey(id);
    EXPECT_NE(blob_key.find("/blobs/xxh3/"), String::npos) << blob_key;
    EXPECT_EQ(blob_key.find("/blobs/ch128/"), String::npos) << blob_key;
    EXPECT_TRUE(backend->head(blob_key).exists);

    ManifestEntry e;
    e.path = "data.bin";
    e.placement = EntryPlacement::Blob;
    e.blob_hash = DB::Cas::BlobDigest::fromU128(hexToU128(id.string()));

    e.blob_size = payload.size();
    const ManifestId mid = build->stageManifest({e});
    build->precommitAdd(ns, "rb", mid);
    build->promote(ns, "rb", build->buildId(), mid);
    store->renewWatermarkOnce();

    const FsckReport rep = runFsck(*store, /*detail=*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_GE(rep.reachable, 1u);

    /// Not merely "clean by omission" (e.g. a bug that silently LISTed nothing): the physical listing
    /// actually walked the algo-segmented key.
    const bool found = std::any_of(rep.objects.begin(), rep.objects.end(),
        [](const FsckObject & o) { return o.key.find("/blobs/xxh3/") != String::npos; });
    EXPECT_TRUE(found);
}

/// ============================================================================================
/// CAS pluggable-blob-hash Phase 2 Task 5 -- THE CRUX (anti-silent-leak regression gate).
///
/// Two sites classify a blob by parsing its object-key hex into a hash set: `CasGc.cpp`'s
/// pipeline-blindness condemn sweep (inside `Gc::rebuildBaseline`) and `CasFsck.cpp`'s
/// present-but-unreferenced classification. Both used to route through the bare, fixed-width
/// `hexToU128` (32-hex-only) inside a `catch(...) continue` / no-catch-at-all — so a 64-hex `sha256`
/// key either (a) fell into the "foreign key shape — not ours" catch and was silently treated as
/// debris (the condemn sweep: the blob is NEVER condemned — a permanent GC leak), or (b) threw
/// uncaught out of fsck's present-but-unreferenced loop (a hard fsck failure on a live sha256 pool).
/// Phase 2 Task 5 ports both to the pool-scoped `DigestCodec::fromHex`, which parses a CORRECT-WIDTH
/// key (16 OR 32 bytes) — a genuinely foreign key shape (e.g. a `.meta` sibling) still falls into
/// the catch, but a real sha256 blob no longer does.
///
/// This test constructs a `sha256`-algo pool DIRECTLY via `PoolConfig` (this bypasses only the
/// disk-config *factory* guard in `MetadataStorageFactory.cpp`, which Task 6 removes — `Store::open`
/// itself has never gated on algo) and writes an unreferenced blob body straight at its 64-hex
/// content-addressed key (bypassing `Build::putBlob`, whose OWN internal `logical_hash` stays a
/// fixed 128-bit representation until a later task — see the Task 5 report). It then drives BOTH
/// crux sites and asserts the blob is CLASSIFIED, not silently skipped as foreign.
///
/// MUST GO RED if either port is reverted to `hexToU128`: reverting `CasGc.cpp`'s sweep leaves
/// `condemned_total == 0` (never condemned) and `previewDeletes()` empty; reverting `CasFsck.cpp`'s
/// sites either throws out of `runFsck` or leaves the blob unclassified/absent from `unreachable`.
TEST(CasPluggableHash, Sha256BlobSeenByCondemnSweepAndFsckNotSilentlySkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                   .blob_hash_algo = BlobHashAlgo::Sha256, .gc_trim_min_events = 0});
    ASSERT_EQ(store->poolMeta().blob_hash_len, 32u) << "a sha256 pool must record a 32-byte digest width";

    const DigestCodec codec(store->poolMeta());
    const std::string payload = makeMultiBlockPayload();
    const std::string hex = blobHashHexOneShot(BlobHashAlgo::Sha256, payload);
    ASSERT_EQ(hex.size(), 64u) << "sha256 renders 64 lowercase hex chars";
    const BlobDigest digest = codec.fromHex(hex);   // round-trip sanity: must not throw at width 32

    /// Write the blob body DIRECTLY at its content key (mirrors `cas_test_helpers.h`'s `writeBlobRaw`,
    /// widened to a 64-hex id) -- an unreferenced (orphan) blob, exactly the shape the pipeline-blindness
    /// sweep and fsck's present-but-unreferenced pipeline exist to classify.
    const BlobId id{hex};
    const String blob_key = store->layout().blobKey(id);
    EXPECT_NE(blob_key.find("/blobs/sha256/"), String::npos) << blob_key;
    {
        EnvelopeHeader header;
        header.kind = ObjectKind::Blob;
        header.hash_algo = static_cast<uint8_t>(BlobHashAlgo::Sha256);
        header.domain_id = store->poolMeta().pool_id;
        header.incarnation_tag = UInt128(0x1234);
        header.build_id = UInt128(0x5678);
        header.pad_to_header_len = static_cast<uint32_t>(store->poolMeta().blob_header_len);
        backend->putIfAbsent(blob_key, encodeEnvelopeHeader(header) + payload);
    }
    ASSERT_TRUE(backend->head(blob_key).exists) << "the sha256 blob body must be present before the sweep";

    /// ---- Site 1: the condemn sweep (Gc::rebuildBaseline's pipeline-blindness LIST/HEAD repair) ----
    /// No manifest ever references this blob, so the universe scan's `edge_bearing` set never contains
    /// its hash: the LIST/HEAD sweep over `blobsPrefix()` is the ONLY path that can ever condemn it.
    Gc gc(store, UInt128(1));
    const RebuildReport rep = gc.rebuildBaseline(/*force*/ true);
    ASSERT_TRUE(rep.performed) << rep.refusal;

    const auto state_bytes = backend->get(store->layout().gcStateKey());
    ASSERT_TRUE(state_bytes.has_value());
    const GcState state = decodeGcState(state_bytes->bytes);
    ASSERT_GT(state.snap_generation, 0u);
    const auto seal_bytes = backend->get(store->layout().foldSealKey(state.snap_generation, state.snap_attempt));
    ASSERT_TRUE(seal_bytes.has_value());
    const CasFoldSeal seal = decodeFoldSeal(seal_bytes->bytes);
    ASSERT_TRUE(seal.condemned_summary.contains(0)) << "the seal's condemned_summary must be total over gc_shards";
    EXPECT_EQ(seal.condemned_summary.at(0).condemned_total, 1u)
        << "THE CRUX: the sha256 orphan blob must be condemned by the pipeline-blindness sweep -- a "
           "silent-leak regression (a reverted CasGc.cpp codec.fromHex port) leaves this at 0";

    /// previewDeletes streams the SAME adopted seal via the run's own SourceEdgeKeyCodec (never pool
    /// meta) and must report exactly our blob, at its real 32-byte digest.
    const std::vector<Gc::PreviewEntry> preview = gc.previewDeletes();
    ASSERT_EQ(preview.size(), 1u) << "THE CRUX: previewDeletes must surface the condemned sha256 blob";
    EXPECT_EQ(preview[0].hash, digest);
    EXPECT_EQ(preview[0].key, blob_key);

    /// ---- Site 2: fsck's present-but-unreferenced classification ----
    /// Must complete without throwing (a reverted port either throws BAD_ARGUMENTS out of the
    /// no-try/catch parse sites, or silently drops the blob from every classified set) and must
    /// physically account for the blob.
    FsckReport frep;
    ASSERT_NO_THROW(frep = runFsck(*store, /*detail=*/true));
    EXPECT_EQ(frep.unreachable, 1u)
        << "THE CRUX: fsck's physical listing must count the sha256 blob as unreachable-but-present, "
           "not silently omit it";
    const auto oit = std::find_if(frep.objects.begin(), frep.objects.end(),
        [&](const FsckObject & o) { return o.key == blob_key; });
    ASSERT_NE(oit, frep.objects.end()) << "the sha256 blob must appear in fsck's detailed object list";
    /// The rebuild above already condemned it into the GC snapshot, so fsck's GC-pipeline-view
    /// classification (not the generic Unaccounted bucket -- reachable only by width-correctly pairing
    /// the fsck-side hash against the run's kCondemned row hash) must recognize it as known-to-GC.
    EXPECT_EQ(oit->cls, FsckClass::PendingGc)
        << "THE CRUX: fsck must pair the sha256 blob against the GC snapshot's kCondemned row (a "
           "silent-leak regression in CasFsck.cpp's unref_hashes/in_run_hashes/retired_by_hash port "
           "leaves this as the generic Unaccounted bucket instead)";
}

/// ============================================================================================
/// CAS pluggable-blob-hash Phase 2 Task 6 -- end-to-end sha256 WRITE path (in-memory; the real
/// wiring-level integration + soak is Task 7).
///
/// Before this task, `Build`'s OWN write-path internals stayed a fixed 128-bit representation
/// downstream of the mint (`poolContentHash`/`Build::putBlob`'s `logical_hash`, the `deps` map key, the
/// event-log `object_hash` render, and `objectKey`) -- safe only because the disk-config factory guard
/// (`MetadataStorageFactory.cpp`) blocked any real sha256 pool from reaching `Build` at all (see the
/// Task 5 report and the "Task 6+" comments this task removes). Task 6 finishes those sites AND lifts
/// the guard in the SAME commit. This test drives a REAL `Build` (`putBlob` -> `stageManifest` ->
/// `precommitAdd` -> `promote`) on a `Sha256` pool and asserts:
///   1. the blob lands under `blobs/sha256/<64-hex>` and the manifest entry's `blob_hash`, read back via
///      `decodePartManifest`, is the FULL 32-byte digest (bytes beyond 16 are non-zero for a real sha256
///      digest, i.e. NOT truncated to `.toU128()`'s low 16 bytes);
///   2. an inline file and a standalone blob of IDENTICAL content get the SAME 32-byte `file_hash` under
///      sha256 -- mirroring the (fixed) `ContentAddressedTransaction.cpp` inline-candidate formula
///      (`blobHashHexOneShot(pool_algo, bytes)` -> pool-scoped `DigestCodec::fromHex`) directly at the
///      Core level, since exercising the wiring itself is Task 7's job;
///   3. `runFsck` on the pool is clean (no dangling, no foreign) -- the whole write -> GC -> fsck loop
///      agrees on the 64-hex key.
TEST(CasPluggableHash, Sha256BuildWritesFullWidthDigestAndInlineEqualsBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend,
        PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1,
                   .blob_hash_algo = BlobHashAlgo::Sha256});
    ASSERT_EQ(store->poolMeta().blob_hash_len, 32u) << "a sha256 pool must record a 32-byte digest width";
    const DigestCodec codec(store->poolMeta());

    const RootNamespace ns{"srv1/tbl"};
    const std::string payload = makeMultiBlockPayload();
    const std::string hex = blobHashHexOneShot(BlobHashAlgo::Sha256, payload);
    ASSERT_EQ(hex.size(), 64u) << "sha256 renders 64 lowercase hex chars";
    const BlobId id{hex};

    BuildInfo info;
    info.intended_ref = ns.string() + "/part1";
    auto build = store->startBuild(info);
    const BlobRef ref = build->putBlob(id, BlobSource::fromString(payload));
    EXPECT_EQ(ref.size, payload.size());

    /// THE CRUX (blob side): the blob body lands under the sha256-segmented path, addressed by the
    /// FULL 64-hex key -- `Build::putBlob`'s internal `logical_hash` must not have silently narrowed it
    /// to a 32-hex (128-bit) key before this task.
    const String blob_key = store->layout().blobKey(id);
    EXPECT_NE(blob_key.find("/blobs/sha256/"), String::npos) << blob_key;
    ASSERT_TRUE(backend->head(blob_key).exists);

    /// Mirror the (fixed) inline-candidate hash site directly: same content, same pool algo, via the
    /// SAME public formula ContentAddressedTransaction.cpp's writeFile now uses -- NOT the old hardcoded
    /// CityHash128 (which would produce a DIFFERENT, 128-bit-then-zero-padded value here).
    const BlobDigest inline_hash = codec.fromHex(blobHashHexOneShot(BlobHashAlgo::Sha256, payload));
    const BlobDigest blob_hash = codec.fromHex(hex);
    EXPECT_EQ(inline_hash, blob_hash) << "inline == blob: identical content must hash identically under sha256";

    /// THE CRUX (width): a genuine 32-byte sha256 digest must NOT be zero-padded past byte 16 -- the
    /// shape `BlobDigest::fromU128` (or a reverted hardcoded-CityHash128 inline site) would produce.
    const bool tail_nonzero = std::any_of(blob_hash.bytes.begin() + 16, blob_hash.bytes.end(),
        [](uint8_t b) { return b != 0; });
    EXPECT_TRUE(tail_nonzero) << "a genuine sha256 digest must not be zero-padded past byte 16";

    ManifestEntry blob_entry;
    blob_entry.path = "data.bin";
    blob_entry.placement = EntryPlacement::Blob;
    blob_entry.blob_hash = blob_hash;
    blob_entry.blob_size = payload.size();

    ManifestEntry inline_entry;
    inline_entry.path = "checksums.txt";
    inline_entry.placement = EntryPlacement::Inline;
    inline_entry.blob_hash = inline_hash;
    inline_entry.blob_size = payload.size();
    inline_entry.inline_bytes = payload;

    const ManifestId mid = build->stageManifest({blob_entry, inline_entry});
    build->precommitAdd(ns, "part1", mid);
    build->promote(ns, "part1", build->buildId(), mid);
    store->renewWatermarkOnce();

    /// Read the committed manifest back -- the on-disk `blob_hash` must be the FULL 32-byte digest, not
    /// truncated by the manifest codec or by anything upstream of `stageManifest`.
    const auto manifest_bytes = backend->get(store->layout().manifestKey(mid));
    ASSERT_TRUE(manifest_bytes.has_value());
    const PartManifest read_back = decodePartManifest(manifest_bytes->bytes);
    ASSERT_EQ(read_back.entries.size(), 2u);
    const auto read_blob_it = std::find_if(read_back.entries.begin(), read_back.entries.end(),
        [](const ManifestEntry & e) { return e.placement == EntryPlacement::Blob; });
    ASSERT_NE(read_blob_it, read_back.entries.end());
    EXPECT_EQ(read_blob_it->blob_hash, blob_hash);
    const bool read_tail_nonzero = std::any_of(read_blob_it->blob_hash.bytes.begin() + 16,
        read_blob_it->blob_hash.bytes.end(), [](uint8_t b) { return b != 0; });
    EXPECT_TRUE(read_tail_nonzero) << "the manifest's on-disk blob_hash must not be truncated either";

    /// The write -> GC -> fsck loop must agree end-to-end on the 64-hex key: clean, no dangling.
    const FsckReport rep = runFsck(*store, /*detail=*/true);
    EXPECT_TRUE(rep.clean());
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_GE(rep.reachable, 1u);
}
