#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace DB::Cas;

namespace
{

/// A gate a test opens explicitly, so a task can be held in flight without a sleep.
struct Gate
{
    void wait()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return open_; });
    }
    void open()
    {
        std::lock_guard lock(m);
        open_ = true;
        cv.notify_all();
    }
    std::mutex m;
    std::condition_variable cv;
    bool open_ = false;
};

/// Spin until the drain has latched `stopping`. Bounded so a broken implementation fails the test
/// instead of hanging it.
void awaitStopLatched(const PoolPtr & store)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!store->detachedWorkStoppingForTest())
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "the drain never latched `stopping`";
        std::this_thread::yield();
    }
}

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend, PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    return Pool::open(backend, config);
}

/// A pool where any nonempty tail is over-threshold, so every mutation auto-dispatches a publish.
PoolPtr openPublishingPool(const std::shared_ptr<DB::Cas::tests::OrderedFaultBackend> & backend,
                           PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    config.snapshot_log_count_threshold = 0;
    config.snapshot_log_bytes_threshold = 1ULL << 40;
    /// One attempt, so a faulted PUT resolves to a definite non-committed outcome with no internal
    /// retry loop and no wall-clock wait -- the same budget the snapshot-ordering suite uses.
    config.cas_request_budget.max_attempts = 1;
    config.cas_request_budget.attempt_timeout_ms = 100;
    config.cas_request_budget.operation_deadline_ms = 5000;
    config.cas_request_budget.lease_safety_margin_ms = 100;
    return Pool::open(backend, config);
}

/// The same one-transaction publish every other ref suite drives, so a namespace reaches `Live` through
/// the REAL append lane (which is also what creates its `_ckpt`).
RefTxnId publishRef(const PoolPtr & store, const RootNamespace & ns, const String & ref, uint64_t ordinal)
{
    return store->appendRefOps(ns, MutationScope::ref(ref),
        [&ref, ordinal](const RefTableState & state)
        {
            std::vector<RefOp> ops;
            if (state.getLifecycle() != RefLifecycle::Live)
                ops.push_back(DB::Cas::tests::namespaceBirthOp());
            for (const RefOp & op : DB::Cas::tests::publishCommittedOps(ref, ManifestRef{1, ordinal, 1}))
                ops.push_back(op);
            return ops;
        },
        RootMutationOrigin::Writer, RootMutationKind::Publish);
}

}

/// A zero in-flight count must mean no tracked task still holds the pool. The hook fires at the exact
/// boundary between releasing the lease's pool reference and decrementing the count, so an
/// implementation that decrements first is caught HERE rather than by a racy post-hoc check.
TEST(CASDetachedWork, LeaseReleasesPoolBeforeDecrementing)
{
    auto backend = std::make_shared<InMemoryBackend>();

    std::weak_ptr<Pool> weak;
    std::atomic<long> use_count_at_boundary{-1};

    PoolConfig config;
    config.detached_lease_release_hook_for_test
        = [&use_count_at_boundary, &weak] { use_count_at_boundary.store(weak.use_count()); };

    auto store = openPlainPool(backend, config);
    weak = store;

    ASSERT_TRUE(store->tryDispatchDetached([](DetachedStopToken) {}));
    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));

    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_EQ(use_count_at_boundary.load(), 1L)
        << "at the release boundary the task still held a pool reference: the lease decremented "
           "before releasing it, so a zero count does not imply the pool is free";
    EXPECT_EQ(weak.use_count(), 1L);
}

/// The drain must not RETURN while a task is still running. Asserted as "the drain is still blocked
/// while the task is held" -- the only formulation that does not race the task's completion.
TEST(CASDetachedWork, DrainDoesNotReturnWhileWorkIsInFlight)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    ASSERT_TRUE(store->tryDispatchDetached([entered, release](DetachedStopToken)
    {
        entered->open();
        release->wait();
    }));
    entered->wait();

    auto drain = std::async(std::launch::async,
                            [&store] { return store->stopAndDrainDetachedWork(/*deadline_ms=*/60000); });

    awaitStopLatched(store);
    EXPECT_EQ(drain.wait_for(std::chrono::seconds(2)), std::future_status::timeout)
        << "the drain returned while a tracked task was still in flight";

    release->open();
    EXPECT_TRUE(drain.get());
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
}

/// A task must see the stop that is already latched. The handshake matters: releasing the task before
/// the drain latches would make a CORRECT implementation record `false`.
TEST(CASDetachedWork, TaskObservesStopTokenOnceLatched)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    auto entered = std::make_shared<Gate>();
    auto release = std::make_shared<Gate>();
    std::atomic<bool> saw_stop{false};

    ASSERT_TRUE(store->tryDispatchDetached([entered, release, &saw_stop](DetachedStopToken token)
    {
        entered->open();
        release->wait();
        saw_stop.store(token.stopping());
    }));
    entered->wait();

    auto drain = std::async(std::launch::async,
                            [&store] { return store->stopAndDrainDetachedWork(/*deadline_ms=*/60000); });
    awaitStopLatched(store);
    release->open();

    EXPECT_TRUE(drain.get());
    EXPECT_TRUE(saw_stop.load());
}

/// After stopping, no new detached work may be created.
TEST(CASDetachedWork, DispatchIsRefusedAfterStop)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    ASSERT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/10000));
    EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {}));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
}

/// A dispatch that cannot allocate must leave the count untouched, not stranded above zero.
TEST(CASDetachedWork, FailedAdmissionLeavesNoStrandedCount)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::ThrowBeforeLaunch;
    auto store = openPlainPool(backend, config);

    EXPECT_ANY_THROW(store->tryDispatchDetached([](DetachedStopToken) {}));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000))
        << "a stranded count makes every later drain run to its full deadline";
}

/// A launch that fails after admission must roll the count back, and must not throw at its caller.
TEST(CASDetachedWork, FailedLaunchRollsBackAndDoesNotThrow)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig config;
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::RefuseLaunch;
    auto store = openPlainPool(backend, config);

    EXPECT_NO_THROW(EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {})));
    EXPECT_EQ(store->detachedWorkInFlightForTest(), 0u);
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000));
}

/// A dispatch that fails must not fail the mutation that triggered it, and must not strand the
/// publisher's single-flight reservation.
TEST(CASDetachedWork, FailedPublisherDispatchKeepsMutationAndClearsReservation)
{
    auto backend = std::make_shared<DB::Cas::tests::OrderedFaultBackend>();
    PoolConfig config;
    config.detached_dispatch_fault_for_test = DetachedDispatchFault::ThrowBeforeLaunch;
    auto store = openPublishingPool(backend, config);
    const RootNamespace ns{"srv1/dispatch_fail"};

    EXPECT_NO_THROW(publishRef(store, ns, "ref_1", 1))
        << "a best-effort maintenance dispatch must never fail an otherwise-successful mutation";
    EXPECT_EQ(store->pendingSnapshotPublishesForTest(ns), 0)
        << "the reservation was stranded: quiescence and dropNamespace would wait on it forever";
}

/// Settlement must survive a throwing error handler. Today it is a bare call after the handler, so a
/// handler that throws skips it and strands the reservation for the life of the process.
///
/// The publish is FAULTED deliberately: with a healthy backend it would succeed, the handler would
/// never run, and this test would pass while exercising nothing.
TEST(CASDetachedWork, SettlementSurvivesAThrowingErrorHandler)
{
    auto backend = std::make_shared<DB::Cas::tests::OrderedFaultBackend>();
    PoolConfig config;
    config.publish_error_hook_for_test
        = [] { throw std::runtime_error("injected: the error handler itself throws"); };
    auto store = openPublishingPool(backend, config);
    const RootNamespace ns{"srv1/handler_throws"};

    /// Arm the fault so the publisher's own PUT fails and its `catch` is entered. Use the same arming
    /// call the snapshot-ordering suite uses against this backend.
    backend->armPutFailure("_snap/", 1);

    ASSERT_NO_THROW(publishRef(store, ns, "ref_1", 1));
    store->waitForSnapshotPublishSettleForTest(ns);
    EXPECT_EQ(store->pendingSnapshotPublishesForTest(ns), 0);
}
