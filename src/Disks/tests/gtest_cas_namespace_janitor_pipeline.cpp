#include "cas_namespace_janitor_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/ThreadPool.h>
#include <Poco/Exception.h>
#include <array>
#include <chrono>
#include <future>
#include <thread>

/// The janitor phase with jobs on a real pool of 4 threads. Assertions hold for any interleaving.

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;
using namespace DB::Cas::tests::janitor;

namespace
{

const Layout & kLayout = janitorLayout();

JanitorRunContext parallel(JanitorFixture & f, ThreadPool & pool, size_t batch_keys)
{
    JanitorRunContext context = f.context(batch_keys);
    context.io_pool = &pool;
    return context;
}

size_t countPresent(Backend & backend, const std::vector<String> & keys, size_t begin, size_t end)
{
    size_t count = 0;
    for (size_t i = begin; i < end; ++i)
        count += present(backend, keys[i]) ? 1 : 0;
    return count;
}

}

TEST(CASNamespaceJanitorPipeline, PipelineListsNextPageWhileJobsRun)
{
    JanitorFixture f;
    ManualBarrier page_two_listed;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 401), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[0])
            page_two_listed.arriveAndWait();
    };
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 1)
            page_two_listed.release();
    };
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, 1000));

    EXPECT_EQ(result.batches_held, 0u) << "a round thread that waited for the batch would time the barrier out";
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 3000), 0u);
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorPipeline, NoBatchCapabilitySendsOneKeyRequestsWithoutProbe)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 403), 1000);
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, f.backend->bulkDeleteKeyLimit()));

    const auto calls = f.backend->bulkCalls();
    ASSERT_EQ(calls.size(), 1000u);
    for (const auto & call : calls)
    {
        EXPECT_EQ(call.keys.size(), 1u);
        EXPECT_NE(call.thread, std::this_thread::get_id());
    }
    EXPECT_TRUE(f.backend->exactRemoves().empty());
    EXPECT_EQ(result.batch_keys, 1u);
    EXPECT_EQ(result.delete_jobs, 1000u);
    EXPECT_EQ(result.batches_held, 0u);
    EXPECT_EQ(f.backend->callSizes(), f.backend->requestSizes()) << "no locally refused multi-key call";
}

TEST(CASNamespaceJanitorPipeline, HeldPageStopsCursorAndLaterPagesAreNotDeleted)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 404), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[1000])
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, 1000));

    EXPECT_EQ(countPresent(*f.backend, keys, 0, 1000), 0u);
    EXPECT_EQ(countPresent(*f.backend, keys, 1000, 2000), 1000u);
    EXPECT_EQ(countPresent(*f.backend, keys, 2000, 3000), 3000u - 2000u) << "the page behind a held one is never deleted";
    EXPECT_EQ(f.cursor(), keys[999]);
    EXPECT_EQ(result.batches_held, 1u);
    const auto calls = f.backend->bulkCalls();
    EXPECT_EQ(std::count_if(calls.begin(), calls.end(), [&](const auto & call) { return call.keys.front() == keys[1000]; }), 1)
        << "a local failure is not retried";
    EXPECT_GE(f.backend->namespaceLists(), 2u);
    EXPECT_LE(f.backend->namespaceLists(), 3u) << "the next page is listed at most once, while the held page's job runs";
}

TEST(CASNamespaceJanitorPipeline, HeldJobSkipsItsPageSiblingsAndHoldsTheCursor)
{
    JanitorFixture f;
    ManualBarrier first_job;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 411), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[0])
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 1)
            first_job.release();
    };
    /// One thread runs page 0's two jobs in order. The first waits until page 1 is listed, so both are enqueued
    /// before the first one fails and the second finds the stop flag.
    auto pool = makeJobPool(1);
    JanitorRunContext context = parallel(f, *pool, 500);
    context.on_job_start_for_test = [&](size_t page, size_t job)
    {
        if (page == 0 && job == 0)
            first_job.arriveAndWait();
    };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(result.delete_jobs_skipped, 1u);
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 1000), 1000u) << "the failed job and the skipped one sent nothing";
    EXPECT_FALSE(f.cursor().has_value()) << "the held page is not passed";
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.pages, 1u) << "the page listed behind the held one is dropped";
    EXPECT_EQ(f.backend->namespaceLists(), 2u);
}

TEST(CASNamespaceJanitorPipeline, ExceptionalExitWaitsForOutstandingJobs)
{
    JanitorFixture f;
    ManualBarrier job;
    std::atomic<bool> job_finished{false};
    (void)seedLogs(*f.backend, kLayout, life("dead", 412), 2000);
    auto pool = makeJobPool(4);
    JanitorRunContext context = parallel(f, *pool, 1000);
    size_t refreshes = 0;
    context.refresh_authority = [&]
    {
        if (++refreshes == 2)
            throw std::runtime_error("injected authority refresh failure");
    };
    context.on_job_start_for_test = [&](size_t, size_t)
    {
        job.arriveAndWait();
        job_finished = true;
    };

    auto run = std::async(std::launch::async, [&] { f.run(context); });
    job.waitUntilArrived();
    /// With the guard `run` cannot return while the job is blocked, so this wait times out; without it `run`
    /// has already thrown, with the job still holding references into its frame.
    const bool returned_early = run.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    job.release();
    EXPECT_THROW(run.get(), std::runtime_error);

    EXPECT_FALSE(returned_early) << "run returned while a job was still outstanding";
    EXPECT_TRUE(job_finished.load());
}

TEST(CASNamespaceJanitorPipeline, ParallelPathScheduleRefusalHolds)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 408), 2000);
    auto pool = makeJobPool(4);
    std::optional<size_t> refuse_at = 10;
    JanitorRunContext context = parallel(f, *pool, 1);
    context.schedule_refuse_at_for_test = &refuse_at;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_FALSE(refuse_at.has_value()) << "the seam resets once it fires";
    EXPECT_EQ(result.delete_jobs, 10u);
    EXPECT_EQ(result.batches + result.delete_jobs_skipped, 10u);
    EXPECT_EQ(f.backend->bulkCalls().size(), result.batches);
    EXPECT_EQ(result.deleted, result.batches);
    EXPECT_EQ(2000 - countPresent(*f.backend, keys, 0, 2000), result.batches);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.batches_held, 1u) << "the refusal";
    EXPECT_EQ(f.backend->namespaceLists(), 1u) << "a refusal sets the stop flag before the next page is listed";
    EXPECT_FALSE(f.cursor().has_value());
}

namespace
{

/// Blocks the `gc/state` read of the janitor's second refresh -- the first one after its first LIST --
/// until the test releases it.
class RefreshGateBackend : public JanitorBackend
{
public:
    RefreshGateBackend(ManualBarrier & gate_, String gc_state_key_) : gate(gate_), gc_state_key(std::move(gc_state_key_)) {}

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key == gc_state_key && namespaceLists() == 1 && !gated.exchange(true))
            gate.arriveAndWait();
        return JanitorBackend::read(key, access);
    }

private:
    ManualBarrier & gate;
    const String gc_state_key;
    std::atomic<bool> gated{false};
};

}

TEST(CASNamespaceJanitorIntegration, RefreshInFlightKeepsAuthority)
{
    ManualBarrier refresh_read;
    std::atomic<size_t> page_one_applied{0};
    std::array<std::atomic<bool>, 2> first_attempt_failed{};
    std::vector<String> keys;
    std::map<String, UInt64> row;
    auto backend = std::make_shared<RefreshGateBackend>(refresh_read, Layout("p").gcStateKey());
    backend->setStorageLimit(500);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_io_concurrency = 4;
    auto store = Pool::open(backend, config);
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    /// With the live checkpoint, 2,999 dead keys fill exactly three pages: page 1 has 999, cut 500 + 499.
    keys = seedLogs(*backend, layout, life("dead", 1), 2999);
    backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        /// Each page-1 job fails once after the refresh read has begun, so its reissue samples authority
        /// while the refresh is in flight.
        for (size_t job = 0; job < first_attempt_failed.size(); ++job)
        {
            if (batch.front().str() == keys[500 * job] && !first_attempt_failed[job].exchange(true))
            {
                refresh_read.waitUntilArrived();
                throw Poco::TimeoutException("injected throttle; the reissue is gated inside the refresh");
            }
        }
    };
    backend->after_bulk = [&](const JanitorBackend::Keys & batch)
    {
        const String & first = batch.front().str();
        if ((first == keys[0] || first == keys[500]) && ++page_one_applied == 2)
            refresh_read.release();
    };
    Gc gc(store, UInt128{411});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["batches_held"], 0u) << "a refresh that stores false first would hold both page-1 jobs";
    EXPECT_EQ(row["delete_jobs_skipped"], 0u);
    EXPECT_EQ(row["janitor_deleted"], 2999u);
    for (const String & key : keys)
        EXPECT_FALSE(present(*backend, key)) << key;
    EXPECT_EQ(publishedCursor(store->openRequests(), layout), String{});
}

TEST(CASNamespaceJanitorIntegration, TeardownDuringPhaseHoldsWithoutLeaking)
{
    Pool * store_ptr = nullptr;
    std::map<String, UInt64> row;
    auto backend = std::make_shared<JanitorBackend>();
    backend->setStorageLimit(500);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_io_concurrency = 1;
    auto store = Pool::open(backend, config);
    store_ptr = store.get();
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    const std::vector<String> keys = seedLogs(*backend, layout, life("dead", 1), 999);
    /// One pool thread runs the two jobs in order: the teardown begins in the first job's request, so the
    /// second is refused at admission.
    std::atomic<bool> torn_down{false};
    backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        if (!torn_down.exchange(true))
            store_ptr->beginTeardown();
    };
    Gc gc(store, UInt128{412});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    /// The round's later phases fail on the closed open plane; only the janitor's accounting is asserted.
    try
    {
        (void)runRegularRoundReclaiming(gc);
    }
    catch (const DB::Exception &)
    {
    }
    gc.setPhaseSink({});

    EXPECT_GE(row["batches_held"] + row["delete_jobs_skipped"], 1u);
    EXPECT_EQ(row["leaked"], 0u);
    CasOperation verify = store->mountRequests().admit();
    for (size_t i = 500; i < 999; ++i)
        EXPECT_TRUE(verify.head(keys[i], Retry::once()).has_value()) << "refused at admission: " << keys[i];
}
