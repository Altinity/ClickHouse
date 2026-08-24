#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
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
