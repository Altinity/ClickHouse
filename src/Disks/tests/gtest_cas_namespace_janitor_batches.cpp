#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>

#include <atomic>

/// Batch deletes of dead-life `_log`/`_snap` keys: one page through `NamespaceJanitor`, then whole rounds
/// through `Gc`, which adds the page loop, the time budget and the GC I/O pool.

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace
{

void createObj(Backend & backend, const String & key, const String & bytes)
{
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once())));
}

bool present(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).head(key, Retry::standard()).has_value();
}

size_t countPresent(Backend & backend, const std::vector<String> & keys)
{
    size_t n = 0;
    for (const String & key : keys)
        n += present(backend, key) ? 1 : 0;
    return n;
}

NamespaceLifeId life(const char * name, uint64_t id)
{
    return NamespaceLifeId::fromCatalogEntry(RootNamespace{name}, DB::UInt128{id});
}

std::vector<String> seedLogs(Backend & backend, const Layout & layout, const NamespaceLifeId & of, uint64_t count)
{
    std::vector<String> keys;
    for (uint64_t i = 1; i <= count; ++i)
    {
        keys.push_back(layout.refLogKey(of, RefTxnId{.writer_epoch = 1, .ref_sequence = i}));
        createObj(backend, keys.back(), "log");
    }
    return keys;
}

GcMaintenanceReadResult readState(CasRequests & requests, const Layout & layout)
{
    auto op = requests.admit();
    return readGcMaintenanceState(op, layout);
}

/// Every batch delete fails; single-key behaviour is untouched.
class FailingBulkBackend : public CountingBackend
{
public:
    void removeManyWriteOnce(const std::vector<WriteOnceKey> &, TransportAccess &) override
    {
        throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR, "bulk delete failed");
    }
};

/// The first batch delete goes through and flips `delete_done`, which a liveness predicate reads.
class FlipAfterBulkBackend : public CountingBackend
{
public:
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        CountingBackend::removeManyWriteOnce(keys, access);
        delete_done = true;
    }
    std::atomic<bool> delete_done{false};
};

/// A store without a batch delete: more than one key in a request is `NOT_IMPLEMENTED`.
class NoBatchDeleteBackend : public CountingBackend
{
public:
    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        if (keys.size() > 1)
            throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "no batch delete");
        ++single_key_requests;
        CountingBackend::removeManyWriteOnce(keys, access);
    }
    std::atomic<size_t> single_key_requests{0};
};

/// Each `LIST` of the namespace root moves the clock, so a test can spend the janitor's budget in one page.
class ClockOnNamespaceListBackend : public CountingBackend
{
public:
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        if (prefix.ends_with("/cas/ns/"))
            clock_ms += step_ms;
        return CountingBackend::list(prefix, cursor, limit, access);
    }
    std::atomic<uint64_t> clock_ms{1};
    uint64_t step_ms = 0;
};

template <typename BackendT>
PoolPtr openPool(std::shared_ptr<BackendT> backend, std::optional<size_t> refuse_at = std::nullopt)
{
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test"};
    config.gc_fold_max_defer_rounds = 0;
    config.gc_io_concurrency = 4;
    config.gc_io_pool_refuse_at_for_test = refuse_at;
    return Pool::open(std::move(backend), std::move(config));
}

/// One reclaiming round; returns the `namespace_cleanup` phase metrics.
std::map<String, UInt64> runRoundAndGetJanitorMetrics(Gc & gc)
{
    std::map<String, UInt64> metrics;
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "namespace_cleanup")
            metrics = record.metrics;
    });
    const RoundReport report = runRegularRoundReclaiming(gc);
    gc.setPhaseSink({});
    EXPECT_TRUE(report.acquired_lease);
    return metrics;
}

}

TEST(CASNamespaceJanitorBatches, DeadStreamKeysGoInOneBatchAndStateKeysStayExact)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    createObj(*backend, layout.refCatalogKey(), encodeRefCatalog({}));
    const NamespaceLifeId dead = life("dead", 301);
    std::vector<String> stream = seedLogs(*backend, layout, dead, 3);
    stream.push_back(layout.refSnapshotKey(dead, RefTxnId{.writer_epoch = 1, .ref_sequence = 2}));
    createObj(*backend, stream.back(), "snap");
    const String ckpt = layout.refCkptKey(dead);
    createObj(*backend, ckpt, "ckpt");
    backend->resetCounts();

    const NamespaceJanitorResult result = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });

    EXPECT_EQ(backend->bulkRemoveCalls(), 1u);
    EXPECT_EQ(backend->headTotal(), 0u);
    EXPECT_EQ(result.deleted, 5u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(countPresent(*backend, stream), 0u);
    EXPECT_FALSE(present(*backend, ckpt));
}

TEST(CASNamespaceJanitorBatches, LiveStreamKeysAreRetained)
{
    auto backend = std::make_shared<CountingBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    const CatalogEntry entry{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = DB::UInt128{302}};
    createObj(*backend, layout.refCatalogKey(), encodeRefCatalog(RefCatalog{.entries = {entry}}));
    const std::vector<String> stream
        = seedLogs(*backend, layout, NamespaceLifeId::fromCatalogEntry(entry.ns, entry.incarnation), 3);
    backend->resetCounts();

    const NamespaceJanitorResult result = NamespaceJanitor(requests, layout, 100).runOnePage(false, [] { return true; });

    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(backend->deleteTotal(), 0u);
    EXPECT_EQ(backend->bulkRemoveCalls(), 0u);
    EXPECT_EQ(countPresent(*backend, stream), 3u);
}

TEST(CASNamespaceJanitorBatches, FailedBatchLeaksAndCursorAdvances)
{
    auto backend = std::make_shared<FailingBulkBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    createObj(*backend, layout.refCatalogKey(), encodeRefCatalog({}));
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 303), 4);

    const NamespaceJanitorResult result = NamespaceJanitor(requests, layout, 2).runOnePage(false, [] { return true; });

    EXPECT_EQ(result.deleted, 0u);
    EXPECT_EQ(result.leaked, 2u);
    EXPECT_EQ(countPresent(*backend, stream), 4u);
    const auto state = readState(requests, layout);
    ASSERT_EQ(state.status, GcMaintenanceReadStatus::Valid);
    EXPECT_FALSE(state.state->janitor_cursor.empty()) << "a failed batch is leak-only: it must not pin the cursor";
}

TEST(CASNamespaceJanitorBatches, AuthorityLostAfterBatchHoldsCursor)
{
    auto backend = std::make_shared<FlipAfterBulkBackend>();
    CasRequests requests(backend, Fence::open());
    const Layout layout("p");
    createObj(*backend, layout.refCatalogKey(), encodeRefCatalog({}));
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 304), 4);

    const NamespaceJanitorResult result
        = NamespaceJanitor(requests, layout, 2).runOnePage(false, [&] { return !backend->delete_done.load(); });

    EXPECT_EQ(result.deleted, 2u);
    EXPECT_EQ(countPresent(*backend, stream), 2u);
    EXPECT_EQ(readState(requests, layout).status, GcMaintenanceReadStatus::Absent)
        << "a tenure that lost authority must not publish progress";
}

TEST(CASNamespaceJanitorBatches, RoundDrainsEveryPageOfDebris)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 311), 2500);
    backend->resetCounts();

    Gc gc(store, DB::UInt128{312});
    const auto metrics = runRoundAndGetJanitorMetrics(gc);

    EXPECT_EQ(countPresent(*backend, stream), 0u);
    /// Three pages of debris, then one empty page from the stream start ends the pass.
    EXPECT_EQ(metrics.at("janitor_pages"), 4u);
    EXPECT_EQ(metrics.at("janitor_deleted"), 2500u);
    EXPECT_EQ(metrics.at("leaked"), 0u);
    EXPECT_EQ(backend->listCount(layout.namespaceRootPrefix()), 4u);
    /// Every page is split across the 4 pool threads.
    EXPECT_EQ(backend->bulkRemoveCalls(), 12u);
}

TEST(CASNamespaceJanitorBatches, RoundDrainsDebrisOnBothSidesOfTheCursor)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 320), 2500);
    {
        CasOperation op = store->openRequests().admit();
        const GcMaintenanceReadResult progress = readGcMaintenanceState(op, layout);
        ASSERT_TRUE(std::holds_alternative<Committed>(casGcMaintenanceState(
            op, layout, progress.etag, GcMaintenanceState{.janitor_cursor = stream[1499]}, Retry::standard())));
    }

    Gc gc(store, DB::UInt128{321});
    const auto metrics = runRoundAndGetJanitorMetrics(gc);

    EXPECT_EQ(metrics.at("janitor_deleted"), 2500u);
    EXPECT_EQ(countPresent(*backend, stream), 0u) << "the pass continues from the stream start after its last page";
}

TEST(CASNamespaceJanitorBatches, BudgetStopsThePassAfterThePageInProgress)
{
    auto backend = std::make_shared<ClockOnNamespaceListBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 313), 2500);
    backend->step_ms = 30'000;

    Gc gc(store, DB::UInt128{314}, {}, [&] { return backend->clock_ms.load(); });
    const auto first = runRoundAndGetJanitorMetrics(gc);

    EXPECT_EQ(first.at("janitor_pages"), 1u);
    EXPECT_EQ(first.at("janitor_deleted"), 1000u);
    ASSERT_TRUE(first.contains("budget_exhausted"));
    EXPECT_EQ(first.at("budget_exhausted"), 1u);
    EXPECT_EQ(countPresent(*backend, stream), 1500u);

    backend->step_ms = 0;
    const auto second = runRoundAndGetJanitorMetrics(gc);
    EXPECT_EQ(second.at("budget_exhausted"), 0u);
    EXPECT_EQ(countPresent(*backend, stream), 0u) << "the next round resumes from the published cursor";
}

TEST(CASNamespaceJanitorBatches, QuietPoolCostsOneListPerRound)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const RootNamespace live_namespace{"00/live@cas@"};
    fixture::admitLive(*backend, layout, live_namespace);
    seedLogs(*backend, layout, fixture::fixtureLife(live_namespace), 3);
    backend->resetCounts();

    Gc gc(store, DB::UInt128{315});
    const auto metrics = runRoundAndGetJanitorMetrics(gc);

    EXPECT_EQ(metrics.at("janitor_pages"), 1u);
    EXPECT_EQ(metrics.at("janitor_deleted"), 0u);
    EXPECT_EQ(backend->listCount(layout.namespaceRootPrefix()), 1u);
}

TEST(CASNamespaceJanitorBatches, StoreWithoutBatchDeleteDrainsKeyByKeyOnThePool)
{
    auto backend = std::make_shared<NoBatchDeleteBackend>();
    auto store = openPool(backend);
    const Layout & layout = store->layout();
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 316), 1200);

    Gc gc(store, DB::UInt128{317});
    const auto metrics = runRoundAndGetJanitorMetrics(gc);

    EXPECT_EQ(countPresent(*backend, stream), 0u);
    EXPECT_EQ(metrics.at("janitor_deleted"), 1200u);
    EXPECT_EQ(metrics.at("leaked"), 0u);
    EXPECT_EQ(backend->single_key_requests.load(), 1200u);
}

TEST(CASNamespaceJanitorBatches, RefusedEnqueueDeletesNothingAndTheNextRoundDrains)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPool(backend, /*refuse_at=*/0);
    const Layout & layout = store->layout();
    const std::vector<String> stream = seedLogs(*backend, layout, life("dead", 318), 100);

    Gc gc(store, DB::UInt128{319});
    const auto refused = runRoundAndGetJanitorMetrics(gc);
    EXPECT_EQ(refused.at("janitor_deleted"), 0u);
    EXPECT_EQ(countPresent(*backend, stream), 100u);

    const auto drained = runRoundAndGetJanitorMetrics(gc);
    EXPECT_EQ(drained.at("janitor_deleted"), 100u);
    EXPECT_EQ(countPresent(*backend, stream), 0u);
}
