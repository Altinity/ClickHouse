#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasEtag.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>

#include <atomic>
#include <functional>
#include <memory>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int INVALID_STATE;
    extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::DialectOverrideBackend;
using DB::Cas::tests::JournalEntry;
using DB::Cas::tests::JournalVerb;
using DB::Cas::tests::OperationForTest;
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

const String kGeneration3Refusal = "CAS pool meta: pool is at reader generation 3, this build needs 4; "
                                   "stop every server of the pool, then run `" + String(kMigratePoolCommandLine) + "`";
const String kGeneration3ReadRefusal = "CAS pool meta: pool is at reader generation 3, this build reads only generation 4; "
                                       "inspect it with the build that writes it (its `cas-fsck`), or migrate it first: stop "
                                       "every server of the pool, then run `" + String(kMigratePoolCommandLine) + "`";
const String kDialectRefusal = "CAS pool records token dialect 'emulated', this disk uses 'etag': identities are not comparable";

PoolConfig configFor(bool read_only = false, String prefix = "p")
{
    PoolConfig config{.pool_prefix = std::move(prefix), .server_root_id = "test"};
    config.read_only = read_only;
    return config;
}

/// Replaces the stored `_pool_meta` with the same pool at `floor` and `dialect`, as an older build or another
/// configuration left it.
void rewritePoolMeta(Backend & backend, uint64_t floor, std::optional<Dialect> dialect)
{
    OperationForTest op(backend);
    const auto stored = (*op).read(Layout("p").poolMetaKey(), Retry::standard());
    ASSERT_TRUE(stored.has_value());
    PoolMeta pm = decodePoolMeta(stored->bytes);
    pm.min_reader_generation = floor;
    pm.token_dialect = dialect;
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).replace(Layout("p").poolMetaKey(), encodePoolMeta(pm), stored->etag, Retry::standard())));
}

void seedGeneration4Pool(Backend & backend, Dialect dialect)
{
    PoolMeta pm = metaAt(G_BLOB_INTENTS, dialect);
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>(
        (*op).create(Layout("p").poolMetaKey(), encodePoolMeta(pm), Retry::standard())));
    CasRefCatalog::initializeEmptyForNewPool(*op, Layout("p"));
}

bool isBlobOrIntentKey(const String & key)
{
    return key.find("/blobs/") != String::npos || key.find("/blob-intents/") != String::npos;
}

/// Asserts that the journal read `_pool_meta` (the positive control) and sent no request on a blob or an intent.
void expectPoolMetaReadAndNoBlobRequest(const std::vector<JournalEntry> & journal)
{
    bool read_pool_meta = false;
    for (const JournalEntry & entry : journal)
        for (const String & key : entry.keys)
        {
            read_pool_meta |= entry.verb == JournalVerb::Read && key == Layout("p").poolMetaKey();
            EXPECT_FALSE(isBlobOrIntentKey(key)) << "request on " << key;
        }
    EXPECT_TRUE(read_pool_meta) << "the open never read _pool_meta";
}

uint64_t storedFloor(Backend & backend)
{
    OperationForTest op(backend);
    return decodePoolMeta((*op).read(Layout("p").poolMetaKey(), Retry::standard())->bytes).min_reader_generation;
}

void writeGcStateAtHeader(Backend & backend, uint32_t header)
{
    GcState state;
    state.header_generation = header;
    state.lease = GcLease{hexToU128("000000000000000000000000000000aa"), 1};
    OperationForTest op(backend);
    const auto existing = (*op).read(Layout("p").gcStateKey(), Retry::standard());
    if (existing)
        (*op).replace(Layout("p").gcStateKey(), encodeGcState(state), existing->etag, Retry::standard());
    else
        (*op).create(Layout("p").gcStateKey(), encodeGcState(state), Retry::standard());
}

/// The journal after the last read of `gc/state`: no request may follow the refused decode.
std::vector<JournalEntry> afterLastGcStateRead(const std::vector<JournalEntry> & journal)
{
    size_t last = journal.size();
    for (size_t i = 0; i < journal.size(); ++i)
        if (journal[i].verb == JournalVerb::Read && journal[i].keys.front() == Layout("p").gcStateKey())
            last = i;
    if (last == journal.size())
        return {};
    return {journal.begin() + static_cast<std::ptrdiff_t>(last) + 1, journal.end()};
}

}

TEST(CASBlobIntentGeneration, BuildIsGenerationFour)
{
    EXPECT_EQ(G_BLOB_INTENTS, 4u);
    EXPECT_EQ(G_BUILD, G_BLOB_INTENTS);
    EXPECT_EQ(Pool::mount_reader_floor, G_BLOB_INTENTS);
}

TEST(CASBlobIntentGeneration, ReadCeiling3RefusesPoolMetaAndGcState)
{
    const DB::Cas::tests::ScopedReadCeiling older_reader(G_REF_CHAIN);
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
        kGeneration3Refusal,
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

TEST(CASBlobIntentGeneration, WritableMountOfAGeneration3PoolIsRefusedAndNamesTheTool)
{
    auto backend = std::make_shared<CountingBackend>();
    DB::Cas::tests::seedOlderPoolForTest(*backend, G_REF_CHAIN);
    backend->clearJournal();
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, kGeneration3Refusal,
        [&] { static_cast<void>(Pool::open(backend, configFor())); });
    expectPoolMetaReadAndNoBlobRequest(backend->journal());
}

TEST(CASBlobIntentGeneration, ReadOnlyOpenIsRefused)
{
    auto backend = std::make_shared<CountingBackend>();
    DB::Cas::tests::seedOlderPoolForTest(*backend, G_REF_CHAIN);
    backend->clearJournal();
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, kGeneration3ReadRefusal,
        [&] { static_cast<void>(Pool::open(backend, configFor(/*read_only=*/true))); });
    expectPoolMetaReadAndNoBlobRequest(backend->journal());
}

TEST(CASBlobIntentGeneration, DecommissionOpenIsRefused)
{
    auto backend = std::make_shared<CountingBackend>();
    {
        PoolPtr member = Pool::open(backend, configFor());
    }
    rewritePoolMeta(*backend, G_REF_CHAIN, std::nullopt);
    backend->clearJournal();
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, kGeneration3Refusal,
        [&] { static_cast<void>(Pool::openForDecommission(backend, configFor(), "test")); });
    expectPoolMetaReadAndNoBlobRequest(backend->journal());
}

TEST(CASBlobIntentGeneration, RefusedWritableOpenWritesNothing)
{
    const auto expectNoWrite = [](const std::vector<JournalEntry> & journal)
    {
        bool read_pool_meta = false;
        for (const JournalEntry & entry : journal)
        {
            read_pool_meta |= entry.verb == JournalVerb::Read && entry.keys.front() == Layout("p").poolMetaKey();
            const bool write = entry.verb == JournalVerb::Write || entry.verb == JournalVerb::Remove
                || entry.verb == JournalVerb::RemoveMany || entry.verb == JournalVerb::Publish;
            EXPECT_FALSE(write) << "the refused open wrote " << (entry.keys.empty() ? String{} : entry.keys.front());
        }
        EXPECT_TRUE(read_pool_meta) << "the open never read _pool_meta";
    };
    {
        SCOPED_TRACE("a generation 3 pool");
        auto backend = std::make_shared<CountingBackend>();
        DB::Cas::tests::seedOlderPoolForTest(*backend, G_REF_CHAIN);
        backend->clearJournal();
        expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, kGeneration3Refusal,
            [&] { static_cast<void>(Pool::open(backend, configFor())); });
        expectNoWrite(backend->journal());
    }
    {
        SCOPED_TRACE("a generation 3 pool and an algorithm to admit");
        auto backend = std::make_shared<CountingBackend>();
        DB::Cas::tests::seedOlderPoolForTest(*backend, G_REF_CHAIN);
        backend->clearJournal();
        PoolConfig config = configFor();
        config.blob_hash_algo = BlobHashAlgo::XXH3_128;
        config.blob_hash_allow_new = true;
        expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, kGeneration3Refusal,
            [&] { static_cast<void>(Pool::open(backend, config)); });
        expectNoWrite(backend->journal());
    }
    {
        SCOPED_TRACE("another dialect");
        auto backend = std::make_shared<DialectOverrideBackend>(Dialect::ETag);
        seedGeneration4Pool(*backend, Dialect::Emulated);
        backend->clearJournal();
        expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS, kDialectRefusal,
            [&] { static_cast<void>(Pool::open(backend, configFor())); });
        expectNoWrite(backend->journal());
    }
}

TEST(CASBlobIntentGeneration, RefusedOpenStartsNoWorker)
{
    const auto counted = [](const std::shared_ptr<std::atomic<uint64_t>> & starts)
    {
        PoolConfig config = configFor();
        config.background_watermark = true;
        config.worker_factory = [starts](std::function<void()> body)
        {
            ++*starts;
            return ThreadFromGlobalPool(std::move(body));
        };
        return config;
    };
    auto backend = std::make_shared<CountingBackend>();
    DB::Cas::tests::seedOlderPoolForTest(*backend, G_REF_CHAIN);
    auto starts = std::make_shared<std::atomic<uint64_t>>(0);
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, kGeneration3Refusal,
        [&] { static_cast<void>(Pool::open(backend, counted(starts))); });
    EXPECT_EQ(starts->load(), 0u) << "a worker started on a pool the open refused";

    /// Positive control: the same configuration starts workers on a pool this build serves.
    auto served = std::make_shared<std::atomic<uint64_t>>(0);
    {
        PoolPtr pool = Pool::open(std::make_shared<CountingBackend>(), counted(served));
    }
    EXPECT_GE(served->load(), 1u);
}

TEST(CASBlobIntentGeneration, NewPoolIsCreatedAtGeneration4WithItsDialect)
{
    for (const Dialect dialect : {Dialect::Emulated, Dialect::ETag})
    {
        SCOPED_TRACE(dialectWord(dialect));
        auto backend = std::make_shared<DialectOverrideBackend>(dialect);
        PoolPtr pool = Pool::open(backend, configFor());
        EXPECT_EQ(pool->poolMeta().min_reader_generation, G_BLOB_INTENTS);
        EXPECT_EQ(pool->poolMeta().token_dialect, dialect);
        OperationForTest op(*backend);
        const PoolMeta stored = decodePoolMeta((*op).read(Layout("p").poolMetaKey(), Retry::standard())->bytes);
        EXPECT_EQ(stored.min_reader_generation, G_BLOB_INTENTS);
        EXPECT_EQ(stored.token_dialect, dialect);
    }
}

TEST(CASBlobIntentGeneration, MountNeverRaisesTheFloor)
{
    auto backend = std::make_shared<CountingBackend>();
    DB::Cas::tests::seedOlderPoolForTest(*backend, G_REF_CHAIN);
    backend->resetCounts();
    EXPECT_ANY_THROW(static_cast<void>(Pool::open(backend, configFor())));
    EXPECT_EQ(storedFloor(*backend), G_REF_CHAIN);
    EXPECT_EQ(backend->writeCount(Layout("p").poolMetaKey()), 0u);
    EXPECT_GE(backend->getCount(Layout("p").poolMetaKey()), 1u) << "the open never read _pool_meta";

    /// On a generation 4 pool the mount writes `_pool_meta` neither.
    auto fresh = std::make_shared<CountingBackend>();
    {
        PoolPtr first = Pool::open(fresh, configFor());
    }
    fresh->resetCounts();
    PoolPtr second = Pool::open(fresh, configFor());
    EXPECT_EQ(fresh->writeCount(Layout("p").poolMetaKey()), 0u);
    EXPECT_GE(fresh->getCount(Layout("p").poolMetaKey()), 1u);
}

TEST(CASBlobIntentGeneration, GcStateBelowHeader4FailsTheRoundWithNoRequest)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolPtr pool = Pool::open(backend, configFor());
    writeGcStateAtHeader(*backend, G_REF_CHAIN);
    Gc gc(pool, hexToU128("000000000000000000000000000000bb"));
    backend->clearJournal();
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, "gc/state is at generation 3 in a generation 4 pool",
        [&] { static_cast<void>(DB::Cas::tests::runRegularRoundReclaiming(gc)); });
    const auto journal = backend->journal();
    ASSERT_TRUE(std::any_of(journal.begin(), journal.end(), [](const JournalEntry & e)
        { return e.verb == JournalVerb::Read && e.keys.front() == Layout("p").gcStateKey(); })) << "gc/state was never read";
    for (const JournalEntry & entry : afterLastGcStateRead(journal))
        for (const String & key : entry.keys)
            EXPECT_TRUE(key.find("gc/server-roots/") != String::npos)
                << "request after the refused gc/state read: " << key;
}

TEST(CASBlobIntentGeneration, RebuildAndDryRunRefuseAGcStateBelowHeader4)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolPtr pool = Pool::open(backend, configFor());
    writeGcStateAtHeader(*backend, G_REF_CHAIN);
    Gc gc(pool, hexToU128("000000000000000000000000000000bb"));
    backend->resetCounts();
    for (const bool force : {false, true})
        expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, "gc/state is at generation 3 in a generation 4 pool",
            [&] { static_cast<void>(gc.rebuildBaseline(force)); });
    expectThrowsCodeWithMessage(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, "gc/state is at generation 3 in a generation 4 pool",
        [&] { static_cast<void>(gc.previewDeletes()); });
    EXPECT_EQ(backend->deleteTotal(), 0u);
}

TEST(CASBlobIntentGeneration, PoolPrefixOver668BytesIsRefusedAtCreation)
{
    ASSERT_EQ(Layout::maxPoolPrefixBytesForBlobIntents(), 668u);
    auto refused = std::make_shared<CountingBackend>();
    expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS,
        "pool prefix of 669 bytes leaves no room for blob intent keys; the limit is 668",
        [&] { static_cast<void>(Pool::open(refused, configFor(false, String(669, 'a')))); });
    EXPECT_EQ(refused->writeTotal(), 0u) << "a refused creation wrote something";

    auto accepted = std::make_shared<CountingBackend>();
    PoolPtr pool = Pool::open(accepted, configFor(false, String(668, 'a')));
    EXPECT_EQ(pool->poolMeta().min_reader_generation, G_BLOB_INTENTS);
}

TEST(CASBlobIntentDialect, WritableMountIsRefused)
{
    auto backend = std::make_shared<DialectOverrideBackend>(Dialect::ETag);
    seedGeneration4Pool(*backend, Dialect::Emulated);
    backend->clearJournal();
    expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS, kDialectRefusal,
        [&] { static_cast<void>(Pool::open(backend, configFor())); });
    expectPoolMetaReadAndNoBlobRequest(backend->journal());
}

TEST(CASBlobIntentDialect, ReadOnlyOpenIsRefused)
{
    auto backend = std::make_shared<DialectOverrideBackend>(Dialect::ETag);
    seedGeneration4Pool(*backend, Dialect::Emulated);
    backend->clearJournal();
    expectThrowsCodeWithMessage(DB::ErrorCodes::BAD_ARGUMENTS, kDialectRefusal,
        [&] { static_cast<void>(Pool::open(backend, configFor(/*read_only=*/true))); });
    expectPoolMetaReadAndNoBlobRequest(backend->journal());
}
