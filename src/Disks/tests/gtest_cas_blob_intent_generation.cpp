#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using DB::Cas::tests::expectThrowsCodeWithMessage;

namespace
{

PoolMeta metaAt(uint64_t floor, std::optional<Dialect> dialect)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("00112233445566778899aabbccddeeff");
    pm.blob_header_len = 256;
    pm.min_reader_generation = floor;
    pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    pm.token_dialect = dialect;
    return pm;
}

}

TEST(CASBlobIntentGeneration, GenerationFourIsAboveTheBuild)
{
    EXPECT_EQ(G_BLOB_INTENTS, 4u);
    EXPECT_EQ(G_BUILD, 3u);
}

TEST(CASBlobIntentGeneration, ReadCeiling3RefusesPoolMetaAndGcState)
{
    const String pool_meta = encodePoolMeta(metaAt(G_BLOB_INTENTS, Dialect::ETag));
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        "CAS pool meta: pool requires reader generation 4 but this build supports at most 3",
        [&] { static_cast<void>(decodePoolMeta(pool_meta)); });

    GcState state;
    state.header_generation = G_BLOB_INTENTS;
    const String gc_state = encodeGcState(state);
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        "CAS cas_gc_state: object requires reader generation 4 but this build supports at most 3",
        [&] { static_cast<void>(decodeGcState(gc_state)); });
}

TEST(CASBlobIntentGeneration, RequireGeneration4PoolRefusesFloor3)
{
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION,
        "CAS pool meta: pool is at reader generation 3, this build needs 4; "
        "stop every server of the pool and run 'clickhouse-disks cas-migrate-pool'",
        [&] { requireGeneration4Pool(metaAt(G_REF_CHAIN, std::nullopt), Dialect::ETag); });
}

TEST(CASBlobIntentGeneration, RequireGeneration4PoolRefusesAnotherDialect)
{
    expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS,
        "CAS pool records token dialect 'etag', this disk uses 'generation': identities are not comparable",
        [&] { requireGeneration4Pool(metaAt(G_BLOB_INTENTS, Dialect::ETag), Dialect::Generation); });
}

TEST(CASBlobIntentGeneration, RequireGeneration4PoolAcceptsItsDialect)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
        EXPECT_NO_THROW(requireGeneration4Pool(metaAt(G_BLOB_INTENTS, dialect), dialect));
}

TEST(CASBlobIntentGeneration, OperationReportsItsBackendDialect)
{
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
    {
        auto requests = DB::Cas::tests::openRequestsForTest(BackendPtr(std::make_shared<DB::Cas::tests::DialectOverrideBackend>(dialect)));
        EXPECT_EQ(requests.admit().backendDialect(), dialect);
    }
}
