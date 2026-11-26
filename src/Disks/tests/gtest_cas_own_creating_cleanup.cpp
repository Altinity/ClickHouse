#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/ProfileEvents.h>
#include "cas_test_helpers.h"

#include <fmt/format.h>

#include <chrono>
#include <memory>

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
