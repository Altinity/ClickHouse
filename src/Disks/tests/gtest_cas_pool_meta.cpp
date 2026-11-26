#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include "cas_test_helpers.h"

#include <utility>
#include <vector>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int INVALID_STATE;
    extern const int NETWORK_ERROR;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

using namespace DB::Cas;

using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::FakeClock;
using DB::Cas::tests::expectThrowsCode;

namespace
{

CasRequests makeRequests(BackendPtr backend, FakeClock & clock, Fence fence = Fence::open())
{
    return CasRequests(std::move(backend), std::move(fence), clock.nowFn(), clock.sleepFn(), nullptr, DB::Cas::kDefaultRequestWindowMs);
}

}

TEST(CASPoolMeta, AdmitOrValidateEndsAtTheDeadlineUnderPerpetualConflict)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    Layout layout("pool");
    auto requests = makeRequests(backend, clock);

    auto create_op = requests.admit();
    const PoolMeta created = PoolMeta::createOrValidate(
        create_op, layout, /*blob_header_len=*/256, /*gc_shards=*/1,
        BlobHashAlgo::CityHash128, /*allow_new=*/false, /*allow_mint=*/true);
    EXPECT_EQ(created.algos_used, (std::vector<uint8_t>{static_cast<uint8_t>(BlobHashAlgo::CityHash128)}));

    /// Rig the key permanently hot: every write attempt races a concurrent rewrite of the SAME
    /// content, so the object's incarnation moves under every attempt and admission of a new algo
    /// never lands. `putOverwrite` mints a fresh incarnation even though the bytes are unchanged.
    const String key = layout.poolMetaKey();
    EXPECT_TRUE(clock.sleeps.empty());   /// nothing paced yet -- the trailing check below is about THIS call
    bool inside_hook = false;
    backend->onBeforeWrite(key, [&]
    {
        if (inside_hook)
            return;
        inside_hook = true;
        auto hook_requests = DB::Cas::tests::openRequestsForTest(BackendPtr(backend));
        auto hook_op = hook_requests.admit();
        if (auto cur = hook_op.read(key, Retry::once()))
            (void)hook_op.replace(key, cur->bytes, cur->etag, Retry::once());
        inside_hook = false;
    });

    /// `orThrow`'s `GaveUp{Deadline}` arm throws exactly `NETWORK_ERROR` (`throwCasWriteRetryLater`),
    /// pinning the deadline outcome apart from the two failures a wrong migration could also throw as
    /// a `DB::Exception` here: `LOGICAL_ERROR` (the absence branch) or `BAD_ARGUMENTS` (`allow_new`
    /// plumbing regressed).
    auto admit_op = requests.admit();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&]
    {
        (void)PoolMeta::createOrValidate(
            admit_op, layout, 256, /*gc_shards=*/1,
            BlobHashAlgo::XXH3_128, /*allow_new=*/true, /*allow_mint=*/false);
    });
    /// Bounded by the deadline, not a live-lock: it paced its retries rather than spinning.
    EXPECT_FALSE(clock.sleeps.empty());
}

/// An object removed from the bucket between the read and the write of an admission is an environment
/// fault, reported as `INVALID_STATE`; `LOGICAL_ERROR` is for program bugs and aborts debug builds.
TEST(CASPoolMeta, PoolMetaVanishingMidAdmissionIsInvalidStateNotLogicalError)
{
    FakeClock clock;
    auto backend = std::make_shared<CountingBackend>();
    Layout layout("pool");
    auto requests = makeRequests(backend, clock);

    auto create_op = requests.admit();
    (void)PoolMeta::createOrValidate(
        create_op, layout, /*blob_header_len=*/256, /*gc_shards=*/1,
        BlobHashAlgo::CityHash128, /*allow_new=*/false, /*allow_mint=*/true);

    const String key = layout.poolMetaKey();
    bool removed = false;
    backend->onBeforeWrite(key, [&]
    {
        if (removed)
            return;
        removed = true;
        auto hook_requests = DB::Cas::tests::openRequestsForTest(BackendPtr(backend));
        auto hook_op = hook_requests.admit();
        auto cur = hook_op.read(key, Retry::once());
        ASSERT_TRUE(cur.has_value());
        (void)hook_op.remove(key, cur->etag, Retry::once());
    });

    auto admit_op = requests.admit();
    expectThrowsCode(DB::ErrorCodes::INVALID_STATE, [&]
    {
        (void)PoolMeta::createOrValidate(
            admit_op, layout, 256, /*gc_shards=*/1,
            BlobHashAlgo::XXH3_128, /*allow_new=*/true, /*allow_mint=*/false);
    });
    EXPECT_TRUE(removed);
}

/// A build that cannot read generation 3 must refuse a pool whose floor is 3 at open.
TEST(CASPoolMeta, FloorThreeIsRefusedBelowTheChainCeiling)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("00112233445566778899aabbccddeeff");
    pm.blob_header_len = 256;
    pm.min_reader_generation = G_REF_CHAIN;
    pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    const String bytes = encodePoolMeta(pm);

    {
        DB::Cas::tests::ScopedReadCeiling c(2);
        expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { decodePoolMeta(bytes); });
    }
    {
        DB::Cas::tests::ScopedReadCeiling c(3);
        EXPECT_EQ(decodePoolMeta(bytes).min_reader_generation, 3u);
    }
}

namespace
{

PoolMeta generation4Meta(Dialect dialect)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("00112233445566778899aabbccddeeff");
    pm.blob_header_len = 256;
    pm.min_reader_generation = G_BLOB_INTENTS;
    pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    pm.token_dialect = dialect;
    return pm;
}

/// A backend holding a generation 4 `_pool_meta` that records `dialect`.
std::shared_ptr<CountingBackend> backendWithGeneration4Meta(const Layout & layout, Dialect dialect)
{
    auto backend = std::make_shared<CountingBackend>();
    auto op = DB::Cas::tests::openRequestsForTest(BackendPtr(backend));
    auto admitted = op.admit();
    static_cast<void>(orThrow(admitted.create(layout.poolMetaKey(), encodePoolMeta(generation4Meta(dialect)), Retry::standard()), "seed"));
    return backend;
}

}

TEST(CASPoolMeta, TokenDialectRoundTrips)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS);
    for (const Dialect dialect : {Dialect::ETag, Dialect::Generation, Dialect::Emulated})
    {
        const String bytes = encodePoolMeta(generation4Meta(dialect));
        EXPECT_NE(bytes.find(fmt::format("\"token_dialect\":\"{}\"", dialectWord(dialect))), String::npos) << bytes;
        EXPECT_EQ(decodePoolMeta(bytes).token_dialect, dialect);
    }
}

TEST(CASPoolMeta, NoTokenDialectIsWrittenWhenUnset)
{
    PoolMeta pm = generation4Meta(Dialect::ETag);
    pm.min_reader_generation = G_REF_CHAIN;
    pm.token_dialect.reset();
    EXPECT_EQ(encodePoolMeta(pm).find("token_dialect"), String::npos);
    EXPECT_FALSE(decodePoolMeta(encodePoolMeta(pm)).token_dialect);
}

TEST(CASPoolMeta, TokenDialectMissingAtFloor4IsCorrupted)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS);
    String bytes = encodePoolMeta(generation4Meta(Dialect::ETag));
    const String field = ",\"token_dialect\":\"etag\"";
    const size_t at = bytes.find(field);
    ASSERT_NE(at, String::npos) << bytes;
    bytes.erase(at, field.size());
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { static_cast<void>(decodePoolMeta(bytes)); });
}

TEST(CASPoolMeta, TokenDialectBelowFloor4IsCorrupted)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS);
    PoolMeta pm = generation4Meta(Dialect::ETag);
    pm.min_reader_generation = G_REF_CHAIN;
    pm.token_dialect.reset();
    String bytes = encodePoolMeta(pm);
    const size_t close = bytes.rfind("}\n");
    ASSERT_NE(close, String::npos) << bytes;
    bytes.insert(close, ",\"token_dialect\":\"etag\"");
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { static_cast<void>(decodePoolMeta(bytes)); });
}

TEST(CASPoolMeta, EncodeRefusesAMetaTheDecoderWouldRefuse)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS);
    PoolMeta no_dialect = generation4Meta(Dialect::ETag);
    no_dialect.token_dialect.reset();
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { static_cast<void>(encodePoolMeta(no_dialect)); });

    PoolMeta dialect_below_floor = generation4Meta(Dialect::ETag);
    dialect_below_floor.min_reader_generation = G_REF_CHAIN;
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { static_cast<void>(encodePoolMeta(dialect_below_floor)); });
}

TEST(CASPoolMeta, RequireGeneration4PoolRefusesAFloor4MetaWithNoDialect)
{
    PoolMeta pm = generation4Meta(Dialect::ETag);
    pm.token_dialect.reset();
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { requireGeneration4Pool(pm, Dialect::ETag); });
}

TEST(CASPoolMeta, UnknownTokenDialectWordIsUnknownFormat)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS);
    String bytes = encodePoolMeta(generation4Meta(Dialect::ETag));
    const size_t at = bytes.find("\"etag\"");
    ASSERT_NE(at, String::npos);
    bytes.replace(at, 6, "\"wat\"");
    expectThrowsCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { static_cast<void>(decodePoolMeta(bytes)); });
}

TEST(CASPoolMeta, TokenDialectSurvivesAdmitOrValidate)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS);
    const Layout layout("pool");
    auto backend = backendWithGeneration4Meta(layout, Dialect::Generation);
    auto requests = DB::Cas::tests::openRequestsForTest(BackendPtr(backend));
    auto op = requests.admit();

    /// A new algorithm is admitted through the read-modify-write; the dialect must come out unchanged.
    const PoolMeta admitted = PoolMeta::createOrValidate(
        op, layout, /*blob_header_len=*/256, /*gc_shards=*/1, BlobHashAlgo::XXH3_128, /*allow_new=*/true, /*allow_mint=*/false);
    ASSERT_EQ(admitted.algos_used.size(), 2u);
    EXPECT_EQ(admitted.token_dialect, Dialect::Generation);

    const auto stored = op.read(layout.poolMetaKey(), Retry::standard());
    ASSERT_TRUE(stored);
    EXPECT_EQ(decodePoolMeta(stored->bytes).token_dialect, Dialect::Generation);
}

TEST(CASPoolMeta, TokenDialectSurvivesRaisingTheFloor)
{
    DB::Cas::tests::ScopedReadCeiling c(G_BLOB_INTENTS + 1);
    const Layout layout("pool");
    auto backend = backendWithGeneration4Meta(layout, Dialect::ETag);
    auto requests = DB::Cas::tests::openRequestsForTest(BackendPtr(backend));
    auto op = requests.admit();

    const PoolMeta raised = PoolMeta::raiseMinReaderGeneration(op, layout, G_BLOB_INTENTS + 1);
    EXPECT_EQ(raised.min_reader_generation, G_BLOB_INTENTS + 1);
    EXPECT_EQ(raised.token_dialect, Dialect::ETag);
    const auto stored = op.read(layout.poolMetaKey(), Retry::standard());
    ASSERT_TRUE(stored);
    EXPECT_EQ(decodePoolMeta(stored->bytes).token_dialect, Dialect::ETag);
}
