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
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

#include <IO/WriteBufferFromFile.h>

#include <cas_format.pb.h>

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
