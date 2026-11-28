#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/ProfileEvents.h>
#include "cas_test_helpers.h"

#include <fmt/format.h>

#include <atomic>
#include <chrono>
#include <memory>

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

namespace ProfileEvents
{
extern const Event CASOwnCreatingCancelled;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// Lease renewal parked an hour out, so the pool sends no request of its own during a test.
PoolPtr openOwnerPool(const std::shared_ptr<CountingBackend> & backend)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.mount_renew_period = std::chrono::milliseconds{3'600'000};
    return Pool::open(backend, config);
}

CreatorFence liveFenceOf(const PoolPtr & store)
{
    return CreatorFence{.server_root_id = store->poolConfig().server_root_id,
                        .writer_epoch = store->writerEpoch(), .fence_generation = store->fenceGeneration()};
}

uint64_t ownCancels()
{
    return ProfileEvents::global_counters[ProfileEvents::CASOwnCreatingCancelled];
}

uint64_t requestTotal(const CountingBackend & backend)
{
    return backend.getTotal() + backend.headTotal() + backend.listTotal() + backend.writeTotal()
        + backend.deleteTotal() + backend.getStreamTotal() + backend.publishTotal();
}

/// Three idle `Creating` rows under the pool's own live fence, as interrupted admissions leave them.
void seedThreeOwnRows(CasOperation & op, const PoolPtr & store)
{
    for (size_t i = 0; i < 3; ++i)
    {
        const CatalogEntry entry{.ns = RootNamespace{fmt::format("00/r{}@cas@", i)}, .state = NsState::Creating,
                                 .incarnation = UInt128{0x500 + i}, .creator = liveFenceOf(store)};
        CasRefCatalog::casAdmitEntry(op, store->layout(), store->poolConfig().gc_shards, entry);
    }
}

size_t ownCreatingRows(CasOperation & op, const PoolPtr & store)
{
    size_t n = 0;
    for (const CatalogEntry & entry : CasRefCatalog::read(op, store->layout()).catalog.entries)
        if (entry.state == NsState::Creating && entry.creator == liveFenceOf(store))
            ++n;
    return n;
}

}

TEST(CASOwnCreatingCleanup, AStopRequestAfterTheFirstCancelEndsThePass)
{
    {
        auto backend = std::make_shared<CountingBackend>();
        PoolPtr store = openOwnerPool(backend);
        CasRequests requests = openRequestsForTest(backend);
        CasOperation op = requests.admit();
        seedThreeOwnRows(op, store);
        store->cancelIdleOwnCreatingRows();
        ASSERT_EQ(ownCreatingRows(op, store), 0u) << "positive control: without a stop one pass cancels all three";
    }

    auto backend = std::make_shared<CountingBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedThreeOwnRows(op, store);

    const uint64_t cancels = ownCancels();
    store->cancelIdleOwnCreatingRows([&] { return ownCancels() - cancels >= 1; });
    EXPECT_EQ(ownCancels() - cancels, 1u);
    EXPECT_EQ(ownCreatingRows(op, store), 2u);

    store->cancelIdleOwnCreatingRows();
    EXPECT_EQ(ownCancels() - cancels, 3u);
    EXPECT_EQ(ownCreatingRows(op, store), 0u) << "the rows the stopped pass left wait for the next one";
}

TEST(CASOwnCreatingCleanup, AStopRequestBeforeThePassSendsNothing)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolPtr store = openOwnerPool(backend);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedThreeOwnRows(op, store);

    const uint64_t before = requestTotal(*backend);
    store->cancelIdleOwnCreatingRows([] { return true; });
    EXPECT_EQ(requestTotal(*backend) - before, 0u);
    EXPECT_EQ(ownCreatingRows(op, store), 3u);
}

/// A stop request that lands while the first row's cancel loses the catalog to another writer ends the pass at
/// that cancel's next submission, before any further catalog write.
TEST(CASOwnCreatingCleanup, AStopRequestDuringARowsConflictEndsThePass)
{
    auto backend = std::make_shared<CountingBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.mount_renew_period = std::chrono::milliseconds{3'600'000};
    config.retry_sleep_fn = [](uint64_t) {};
    PoolPtr store = Pool::open(backend, config);
    CasRequests requests = openRequestsForTest(backend);
    CasOperation op = requests.admit();
    seedThreeOwnRows(op, store);
    const String key = store->layout().refCatalogKey();

    std::atomic<bool> stop = false;
    size_t catalog_writes = 0;
    bool in_hook = false;
    /// Every catalog write of the pass meets an incarnation another writer just moved, as on a hot catalog.
    backend->onBeforeWrite(key, [&]
    {
        if (in_hook)
            return;
        in_hook = true;
        ++catalog_writes;
        CasRequests other_requests = openRequestsForTest(backend);
        CasOperation other = other_requests.admit();
        const auto current = other.read(key, Retry::once());
        EXPECT_TRUE(current && std::holds_alternative<Committed>(other.replace(key, current->bytes, current->etag, Retry::once())));
        in_hook = false;
        stop = true;
    });

    const uint64_t cancels = ownCancels();
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { store->cancelIdleOwnCreatingRows([&] { return stop.load(); }); });
    backend->onBeforeWrite(key, {});
    EXPECT_EQ(catalog_writes, 1u) << "the stopped cancel submitted again";
    EXPECT_EQ(ownCancels() - cancels, 0u);
    EXPECT_EQ(ownCreatingRows(op, store), 3u);
}
