#include <gtest/gtest.h>

/// P1-T2 (CAS pluggable-blob-hash Phase 1, design 2026-07-11-cas-pluggable-blob-hash-design.md §4/§8):
/// `PoolMeta` records the pool-wide `blob_hash_algo` and `PoolMeta::createOrValidate` fail-closes on a
/// disk config that disagrees with an existing pool's recorded algo -- the pool-wide durability
/// invariant (never silently re-hash an existing pool). This does NOT yet change any hashing or path;
/// that is P1-T3.

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include "cas_test_helpers.h"

#include <cas_format.pb.h>

namespace DB::ErrorCodes
{
extern const int BAD_ARGUMENTS;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace Proto = ::clickhouse::cas::format;

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
