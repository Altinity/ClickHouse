#include "cas_namespace_janitor_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Poco/Exception.h>
#include <fmt/format.h>

/// The janitor phase with no pool, so jobs run inline in submit order: exact request orders and counts.

using namespace DB::Cas;
using namespace DB::Cas::tests;
using namespace DB::Cas::tests::janitor;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NETWORK_ERROR;
}

namespace
{

const Layout & kLayout = janitorLayout();

bool allAbsent(Backend & backend, const std::vector<String> & keys, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
        if (present(backend, keys[i]))
            return false;
    return true;
}

bool allPresent(Backend & backend, const std::vector<String> & keys, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
        if (!present(backend, keys[i]))
            return false;
    return true;
}

}

TEST(CASNamespaceJanitor, DeadLifeStreamKeysDrainInBatches)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 201), 2500);

    const NamespaceJanitorResult result = f.run(f.context());

    const auto calls = f.backend->bulkCalls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].keys.size(), 1000u);
    EXPECT_EQ(calls[1].keys.size(), 1000u);
    EXPECT_EQ(calls[2].keys.size(), 500u);
    EXPECT_TRUE(f.backend->exactRemoves().empty());
    EXPECT_EQ(result.pages, 3u);
    EXPECT_EQ(result.batches, 3u);
    EXPECT_EQ(result.delete_jobs, 3u);
    EXPECT_EQ(result.deleted, 2500u);
    EXPECT_EQ(result.batch_keys, 1000u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, keys.size()));
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, StateKeysNeverEnterTheBatchPath)
{
    for (const bool tokenless : {false, true})
    {
        SCOPED_TRACE(tokenless ? "tokenless listing" : "token listing");
        JanitorFixture f;
        f.backend->tokenless = tokenless;
        const NamespaceLifeId dead = life("dead", 203);
        const std::vector<String> stream = seedLogs(*f.backend, kLayout, dead, 1);
        const String ckpt = kLayout.refCkptKey(dead);
        const String file = kLayout.namespaceFilesPrefix(dead) + "data";
        createObj(*f.backend, ckpt, "ckpt");
        createObj(*f.backend, file, "file");
        /// Both state objects are rewritten between observation and delete: only an exact-token delete keeps the rewrite.
        f.backend->before_exact_remove = [&](const String & key, JanitorBackend::Access & access)
        {
            f.backend->writeUncounted(key, "winner", access);
        };

        const NamespaceJanitorResult result = f.run(f.context());

        const auto calls = f.backend->bulkCalls();
        ASSERT_EQ(calls.size(), 1u);
        EXPECT_EQ(calls[0].keys, stream);
        EXPECT_EQ(f.backend->exactRemoves(), (std::vector<String>{ckpt, file}));
        EXPECT_EQ(readObj(*f.backend, ckpt)->bytes, "winner");
        EXPECT_EQ(readObj(*f.backend, file)->bytes, "winner");
        EXPECT_FALSE(present(*f.backend, stream.front()));
        EXPECT_EQ(result.deleted, 1u);
    }
}

TEST(CASNamespaceJanitor, RecreatedLifeBetweenListAndDeleteKeepsItsKeys)
{
    JanitorFixture f;
    const NamespaceLifeId dropped = life("t", 211);
    const NamespaceLifeId recreated = life("t", 212);
    const std::vector<String> old_keys = seedLogs(*f.backend, kLayout, dropped, 3);
    const std::vector<String> new_keys{kLayout.refLogKey(recreated, RefTxnId{1, 1}), kLayout.refSnapshotKey(recreated, RefTxnId{1, 1})};
    f.backend->after_namespace_list = [&](size_t, JanitorBackend::Access & access)
    {
        RefCatalog live;
        live.entries.push_back(CatalogEntry{.ns = recreated.ns, .state = NsState::Live, .incarnation = recreated.incarnation});
        f.backend->writeUncounted(kLayout.refCatalogKey(), encodeRefCatalog(live), access);
        for (const String & key : new_keys)
            f.backend->writeUncounted(key, "new", access);
    };

    const NamespaceJanitorResult result = f.run(f.context());

    ASSERT_EQ(f.backend->bulkCalls().size(), 1u);
    EXPECT_EQ(f.backend->bulkCalls()[0].keys, old_keys);
    EXPECT_EQ(result.deleted, old_keys.size());
    for (const String & key : new_keys)
        EXPECT_EQ(readObj(*f.backend, key)->bytes, "new") << key;
}

TEST(CASNamespaceJanitor, LeaseReplacedBeforeFinalBatch)
{
    JanitorFixture f;
    const NamespaceLifeId dropped = life("t", 215);
    const NamespaceLifeId recreated = life("t", 216);
    const std::vector<String> old_keys = seedLogs(*f.backend, kLayout, dropped, 10);
    const std::vector<String> new_keys{kLayout.refLogKey(recreated, RefTxnId{1, 1}), kLayout.refSnapshotKey(recreated, RefTxnId{1, 1})};
    std::atomic<bool> durable_lease_is_ours{true};
    std::atomic<bool> authority_held{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access & access)
    {
        if (!durable_lease_is_ours.exchange(false))
            return;
        RefCatalog live;
        live.entries.push_back(CatalogEntry{.ns = recreated.ns, .state = NsState::Live, .incarnation = recreated.incarnation});
        f.backend->writeUncounted(kLayout.refCatalogKey(), encodeRefCatalog(live), access);
        for (const String & key : new_keys)
            f.backend->writeUncounted(key, "new", access);
        f.backend->writeUncounted(kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = "second-leader"}), access);
    };
    JanitorRunContext context = f.context();
    context.liveness = [&] { return authority_held.load(); };
    context.refresh_authority = [&] { authority_held = durable_lease_is_ours.load(); };

    const NamespaceJanitorResult result = f.run(context);

    ASSERT_EQ(f.backend->bulkCalls().size(), 1u);
    EXPECT_EQ(f.backend->bulkCalls()[0].keys, old_keys);
    EXPECT_TRUE(allAbsent(*f.backend, old_keys, 0, old_keys.size()));
    for (const String & key : new_keys)
        EXPECT_EQ(readObj(*f.backend, key)->bytes, "new") << key;
    EXPECT_EQ(f.cursor(), String("second-leader")) << "the competing leader's cursor stands";
    EXPECT_FALSE(result.cursor_advanced);
    for (const String & anomaly : result.anomalies)
        EXPECT_EQ(anomaly.find("cursor publication"), String::npos) << "a Conflict is silent: " << anomaly;

    const size_t lists_before = f.backend->namespaceLists();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)f.run(context); });
    EXPECT_EQ(f.backend->namespaceLists(), lists_before) << "the next refresh stops the deposed leader before its first LIST";
}

TEST(CASNamespaceJanitor, PhaseBudgetCheckedBetweenPages)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 221), 5000);
    f.backend->after_namespace_list = [&](size_t, JanitorBackend::Access &) { f.clock.now += 8'000; };
    JanitorRunContext context = f.context();
    context.budget_ms = 20'000;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 3u) << "pages start at 0, 8 and 16 s; none at 24 s";
    EXPECT_EQ(result.pages, 3u);
    EXPECT_TRUE(result.budget_exhausted);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 3000)) << "the page in progress at the deadline completes";
    EXPECT_TRUE(allPresent(*f.backend, keys, 3000, 5000));
    EXPECT_EQ(f.cursor(), keys[2999]);
}

TEST(CASNamespaceJanitor, LaterPageReadFailurePublishesPrefix)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 223), 4000);
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 2)
            f.backend->failNextReadWith(kLayout.refCatalogKey(),
                std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected catalog read failure")));
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.deleted, 2000u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_EQ(f.cursor(), keys[1999]) << "the complete prefix is published; no reset";
    EXPECT_TRUE(allPresent(*f.backend, keys, 2000, 4000));
}

TEST(CASNamespaceJanitor, ObservedAuthorityLossPublishesNothing)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 224), 4000);
    size_t refreshes = 0;
    bool held = true;
    JanitorRunContext context = f.context();
    context.liveness = [&] { return held; };
    context.refresh_authority = [&]
    {
        if (++refreshes == 3)
            held = false;
    };

    (void)f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 2u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 2000)) << "earlier pages' deletes stand";
    EXPECT_TRUE(allPresent(*f.backend, keys, 2000, 4000));
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitor, SteadyStateTakesOnePage)
{
    const CatalogEntry live{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128{225}};
    JanitorFixture f(RefCatalog{.entries = {live}});
    (void)seedLogs(*f.backend, kLayout, NamespaceLifeId::fromCatalogEntry(live.ns, live.incarnation), 5000);
    f.backend->resetCounts();

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.batches, 0u);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_EQ(f.backend->writeCount(kLayout.gcMaintenanceStateKey()), 1u);
}

TEST(CASNamespaceJanitor, DeferredRoundTakesOneSuppressedPage)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 226), 3000);
    f.backend->resetCounts();

    const NamespaceJanitorResult result = f.run(f.context(), /*suppress_deletes=*/ true);

    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_TRUE(f.backend->bulkCalls().empty());
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_EQ(f.backend->writeCount(kLayout.gcMaintenanceStateKey()), 0u);
}

TEST(CASNamespaceJanitor, MidStreamPassWrapsToTheStart)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 228), 5000);
    createObj(*f.backend, kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = keys[1999]}));
    f.backend->resetCounts();

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.backend->namespaceLists(), 5u) << "pages 3, 4, 5, then 1, 2; the page that began the pass is not listed again";
    EXPECT_EQ(result.pages, 5u);
    EXPECT_EQ(result.deleted, 5000u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 5000));
    EXPECT_EQ(f.cursor(), String{});
    EXPECT_EQ(f.backend->writeCount(kLayout.gcMaintenanceStateKey()), 1u) << "one cursor publication per phase";
}

TEST(CASNamespaceJanitor, WrapStopsAtAnAllLivePage)
{
    const CatalogEntry live{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128{229}};
    JanitorFixture f(RefCatalog{.entries = {live}});
    const NamespaceLifeId live_life = NamespaceLifeId::fromCatalogEntry(live.ns, live.incarnation);
    const NamespaceLifeId dead_life = life("dead", 230);
    ASSERT_LT(kLayout.refLogKey(live_life, RefTxnId{1, 1}), kLayout.refLogKey(dead_life, RefTxnId{1, 1}));
    std::vector<String> keys = seedLogs(*f.backend, kLayout, live_life, 1000);
    const std::vector<String> dead_keys = seedLogs(*f.backend, kLayout, dead_life, 4000);
    keys.insert(keys.end(), dead_keys.begin(), dead_keys.end());
    createObj(*f.backend, kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = keys[1999]}));
    f.backend->resetCounts();

    (void)f.run(f.context());

    EXPECT_EQ(f.backend->namespaceLists(), 4u) << "pages 3, 4, 5, then the all-live page 1; page 2 is not listed";
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 2000));
    EXPECT_TRUE(allAbsent(*f.backend, keys, 2000, 5000));
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, WrapDoesNotRevisitKeysPastTheStartCursor)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 231), 5000);
    /// A key that stays behind after the first part: the wrapped part must not list it a second time.
    f.backend->before_bulk = [bad = keys[3000]](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == bad)
            throw Poco::TimeoutException("injected throttle, forever");
    };
    createObj(*f.backend, kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = keys[1499]}));
    f.backend->resetCounts();
    JanitorRunContext context = f.context(f.backend->bulkDeleteKeyLimit());
    context.budget_ms = 10'000'000;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(result.leaked, 1u) << "the stuck key is attempted once per phase";
    EXPECT_EQ(result.deleted, 4999u);
    EXPECT_EQ(f.backend->namespaceLists(), 6u) << "4 pages from the cursor to the end, then 2 up to the cursor";
    EXPECT_TRUE(present(*f.backend, keys[3000]));
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, MidStreamPassOverLivePagesTakesOnePage)
{
    const CatalogEntry live{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128{232}};
    JanitorFixture f(RefCatalog{.entries = {live}});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, NamespaceLifeId::fromCatalogEntry(live.ns, live.incarnation), 5000);
    createObj(*f.backend, kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = keys[3999]}));
    f.backend->resetCounts();

    (void)f.run(f.context());

    EXPECT_EQ(f.backend->namespaceLists(), 1u) << "an all-live page that reaches the end stops the pass before any wrap";
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 5000));
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, HeldPageKeepsTheCursorUntilItClears)
{
    JanitorFixture f;
    (void)seedLogs(*f.backend, kLayout, life("dead", 228), 4000);
    JanitorRunContext one_page = f.context();
    one_page.budget_ms = 0;

    const NamespaceJanitorResult a = f.run(one_page);
    EXPECT_TRUE(a.cursor_advanced);
    const std::optional<String> after_a = f.cursor();

    std::atomic<bool> hold{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        if (hold)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    const NamespaceJanitorResult b = f.run(one_page);
    const NamespaceJanitorResult c = f.run(one_page);
    EXPECT_FALSE(b.cursor_advanced);
    EXPECT_FALSE(c.cursor_advanced);
    EXPECT_EQ(f.cursor(), after_a);

    hold = false;
    const NamespaceJanitorResult d = f.run(one_page);
    const NamespaceJanitorResult e = f.run(one_page);
    EXPECT_TRUE(d.cursor_advanced);
    EXPECT_TRUE(e.cursor_advanced);
    EXPECT_NE(f.cursor(), after_a);
}

TEST(CASNamespaceJanitor, NonPocoExceptionHoldsThePage)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 230), 2000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        throw std::runtime_error("injected non-Poco failure");
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(result.batches_leaked, 0u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 2000));
}

TEST(CASNamespaceJanitor, AuthorityLostBeforeRetryHolds)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 230), 3000);
    f.backend->failNextBulkRemoveWith(std::make_exception_ptr(Poco::TimeoutException("injected throttle")));
    JanitorRunContext context = f.context(500);
    context.liveness = [&] { return f.backend->bulkRemoveCalls() == 0; };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->bulkRemoveCalls(), 1u);
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 1000));
    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.batches, 1u);
    EXPECT_EQ(f.backend->bulkCalls().size(), 1u) << "no second batch";
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitor, AuthorityLostDuringSuccessfulBatchCounts)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 231), 3000);
    std::atomic<bool> held{true};
    f.backend->onBeforeBulkRemove([&] { held = false; });
    JanitorRunContext context = f.context(500);
    context.liveness = [&] { return held.load(); };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->bulkRemoveCalls(), 1u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 500));
    EXPECT_TRUE(allPresent(*f.backend, keys, 500, 1000));
    EXPECT_EQ(result.batches, 1u);
    EXPECT_EQ(result.deleted, 500u);
    EXPECT_EQ(result.batches_held, 0u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitor, EmptyNamespaceTakesOnePage)
{
    JanitorFixture f;
    const NamespaceJanitorResult result = f.run(f.context());
    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.keys, 0u);
    EXPECT_EQ(result.delete_jobs, 0u);
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, FirstPageCatalogReadFailureThrowsAndPublishesNothing)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 232), 10);
    f.backend->failNextReadWith(kLayout.refCatalogKey(),
        std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected catalog read failure")));

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)f.run(f.context()); });

    EXPECT_TRUE(f.backend->bulkCalls().empty());
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, keys.size()));
}

TEST(CASNamespaceJanitor, ClockGoingBackwardsDoesNotExhaustTheBudget)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 233), 2000);
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 0)
            f.clock.now -= 5'000;
    };
    JanitorRunContext context = f.context();
    context.budget_ms = 20'000;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 2u);
    EXPECT_FALSE(result.budget_exhausted);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 2000));
}

TEST(CASNamespaceJanitorIntegration, PoolWrappedStorageLimitCutsJanitorBatches)
{
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setStorageLimit(250);
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/ 0);
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    /// With the live checkpoint, 999 dead keys fill exactly one page.
    const std::vector<String> keys = seedLogs(*backend, layout, life("dead", 1), 999);
    std::map<String, UInt64> row;
    Gc gc(store, UInt128{252});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["batch_keys"], 250u);
    EXPECT_EQ(row["batches"], 4u) << "a decorator that dropped the forwarding would send 999 one-key requests";
    for (const String & key : keys)
        EXPECT_FALSE(present(*backend, key)) << key;
}

TEST(CASNamespaceJanitorIntegration, FoldingRoundDrainsWithinBudget)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, 0);
    const Layout & layout = store->layout();
    const NamespaceLifeId live = seedLiveNamespace(*backend, layout);
    const NamespaceLifeId dead = life("dead", 1);
    /// Dead `_files` list before the live life's, so the pass reaches live keys after 3,500 dead ones.
    ASSERT_LT(layout.namespaceFilesPrefix(dead), layout.namespaceFilesPrefix(live));
    std::vector<String> dead_keys;
    for (size_t i = 0; i < 3500; ++i)
    {
        dead_keys.push_back(layout.namespaceFilesPrefix(dead) + fmt::format("f{:05}", i));
        createObj(*backend, dead_keys.back(), "dead");
    }
    for (size_t i = 0; i < 1500; ++i)
        createObj(*backend, layout.namespaceFilesPrefix(live) + fmt::format("f{:05}", i), "live");
    backend->resetCounts();
    std::map<String, UInt64> row;
    Gc gc(store, UInt128{254});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["janitor_pages"], 5u) << "four pages with dead keys, then one without";
    EXPECT_EQ(row["janitor_deleted"], 3500u);
    EXPECT_EQ(row["budget_exhausted"], 0u);
    EXPECT_EQ(backend->writeCount(layout.gcMaintenanceStateKey()), 1u) << "one cursor publication per phase";
    for (const String & key : dead_keys)
        EXPECT_FALSE(present(*backend, key)) << key;
}

TEST(CASNamespaceJanitorIntegration, GcRoundStopsAtTheBudgetOnItsMonotonicClockAndTheNextRoundResumes)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = Pool::open(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0});
    const Layout & layout = store->layout();
    const NamespaceLifeId live = seedLiveNamespace(*backend, layout);
    const NamespaceLifeId dead = life("dead", 1);
    ASSERT_LT(layout.namespaceFilesPrefix(dead), layout.namespaceFilesPrefix(live));
    std::vector<String> dead_keys;
    for (size_t i = 0; i < 3500; ++i)
    {
        dead_keys.push_back(layout.namespaceFilesPrefix(dead) + fmt::format("f{:05}", i));
        createObj(*backend, dead_keys.back(), "dead");
    }
    for (size_t i = 0; i < 1500; ++i)
        createObj(*backend, layout.namespaceFilesPrefix(live) + fmt::format("f{:05}", i), "live");
    uint64_t mono_ms = 1'000;
    /// Each clock read moves past the 20 s budget, so the phase stops after its first page.
    uint64_t mono_step_ms = 30'000;
    std::map<String, UInt64> row;
    Gc gc(store, UInt128{255}, {}, [&] { return mono_ms += mono_step_ms; });
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    EXPECT_EQ(row["budget_exhausted"], 1u);
    EXPECT_EQ(row["janitor_pages"], 1u) << "the first page runs; the clock has passed the budget before a second starts";
    EXPECT_EQ(row["cursor_advanced"], 1u);
    EXPECT_EQ(row["janitor_deleted"], 1000u);
    EXPECT_TRUE(allAbsent(*backend, dead_keys, 0, 1000));
    EXPECT_TRUE(allPresent(*backend, dead_keys, 1000, 3500));

    mono_step_ms = 0;
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["budget_exhausted"], 0u);
    EXPECT_EQ(row["janitor_deleted"], 2500u) << "the second round resumes at the persisted cursor";
    EXPECT_TRUE(allAbsent(*backend, dead_keys, 0, 3500));
}

TEST(CASNamespaceJanitor, HeldPageReportsNoLeaks)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 302), 1000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[0])
            throw Poco::TimeoutException("injected throttle, every attempt");
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };

    const NamespaceJanitorResult result = f.run(f.context(500));

    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_FALSE(f.cursor().has_value()) << "a held page publishes nothing";
    EXPECT_EQ(result.leaked, 0u) << "its keys are listed again next round, so none is leaked yet";
}

TEST(CASNamespaceJanitor, ExhaustedTransientBatchLeaksAndAdvances)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 301), 1500);
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        throw Poco::TimeoutException("injected throttle, every attempt");
    };
    JanitorRunContext context = f.context();
    context.budget_ms = 20'000;

    const NamespaceJanitorResult result = f.run(context);

    const auto calls = f.backend->bulkCalls();
    ASSERT_GT(calls.size(), 1u) << "the policy reissues the batch before giving up";
    for (const auto & call : calls)
        EXPECT_EQ(call.keys, calls.front().keys) << "no per-key or partial re-cut";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_EQ(result.batches_held, 0u);
    EXPECT_TRUE(f.backend->exactRemoves().empty());
    EXPECT_TRUE(result.budget_exhausted) << "the 90 s retry window on the phase clock exceeds the budget";
    EXPECT_EQ(f.cursor(), keys[999]);
}

TEST(CASNamespaceJanitor, SlowDownForeverDoesNotStarveLaterDebris)
{
    const auto seeded = [](JanitorFixture & f)
    {
        f.backend->setBatchDeleteSupported(false);
        const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 302), 3000);
        f.backend->before_bulk = [bad = keys[499]](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
        {
            if (batch.front().str() == bad)
                throw Poco::TimeoutException("injected throttle, forever");
        };
        return keys;
    };
    {
        SCOPED_TRACE("budget above the retry window");
        JanitorFixture f;
        const std::vector<String> keys = seeded(f);
        JanitorRunContext context = f.context(f.backend->bulkDeleteKeyLimit());
        context.budget_ms = 10'000'000;
        const NamespaceJanitorResult result = f.run(context);
        EXPECT_EQ(result.leaked, 1u);
        EXPECT_EQ(result.batches_leaked, 1u);
        EXPECT_EQ(result.deleted, 2999u);
        EXPECT_TRUE(present(*f.backend, keys[499]));
        EXPECT_EQ(f.cursor(), String{});
    }
    {
        SCOPED_TRACE("default budget");
        JanitorFixture f;
        const std::vector<String> keys = seeded(f);
        JanitorRunContext context = f.context(f.backend->bulkDeleteKeyLimit());
        context.budget_ms = 20'000;
        const NamespaceJanitorResult first = f.run(context);
        EXPECT_TRUE(first.budget_exhausted);
        EXPECT_EQ(f.cursor(), keys[999]);
        const NamespaceJanitorResult second = f.run(context);
        EXPECT_EQ(second.deleted, 2000u);
        EXPECT_EQ(f.cursor(), String{});
        const NamespaceJanitorResult wrapped = f.run(context);
        EXPECT_EQ(wrapped.leaked, 1u) << "the next pass leaks the same key again";
        EXPECT_EQ(wrapped.keys, 1u);
    }
}

TEST(CASNamespaceJanitor, PerKeyErrorInOkResponseLeaksTheBatch)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 303), 1000);
    std::atomic<bool> fail{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access & access)
    {
        if (!fail)
            return;
        for (const WriteOnceKey & key : batch)
            if (key.str() != keys[500])
                f.backend->removeUncounted(key.str(), access);
        throw Poco::TimeoutException("the survivor answers a throttle on every attempt");
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.deleted, 0u) << "a failed batch counts nothing as deleted";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_EQ(f.cursor(), String{});
    fail = false;
    const NamespaceJanitorResult next = f.run(f.context());
    EXPECT_EQ(next.keys, 1u) << "the next pass lists only the survivor";
    EXPECT_EQ(next.deleted, 1u);
}

TEST(CASNamespaceJanitor, UnknownCapabilityRejectionStopsThePhase)
{
    JanitorFixture f;
    f.backend->setStoreRejectsBatches(true);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 304), 3000);

    const NamespaceJanitorResult result = f.run(f.context(f.backend->bulkDeleteKeyLimit()));

    EXPECT_EQ(f.backend->requestSizes(), (std::vector<size_t>{1000}));
    EXPECT_EQ(f.backend->callSizes().size(), 1u) << "no one-key call for the rejected keys";
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_EQ(result.batches_held, 1u);

    const NamespaceJanitorResult next = f.run(f.context(f.backend->bulkDeleteKeyLimit()));
    EXPECT_EQ(next.batch_keys, 1u);
    EXPECT_EQ(next.deleted, 3000u);
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, CapabilityFlippedByAnotherUserStopsThePhase)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 305), 3000);
    f.backend->after_bulk = [&](const JanitorBackend::Keys &) { f.backend->setBatchDeleteSupported(false); };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.backend->requestSizes(), (std::vector<size_t>{1000}));
    EXPECT_EQ(f.backend->callSizes().size(), 2u) << "page 2's batch is refused without a request";
    EXPECT_EQ(f.backend->namespaceLists(), 2u);
    EXPECT_EQ(f.cursor(), keys[999]);
}
