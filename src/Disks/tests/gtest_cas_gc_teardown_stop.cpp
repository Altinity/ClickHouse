#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasDetachedWork.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

/// A disk's teardown must not wait out a GC round. The pool's teardown flag is the open request
/// plane's fence, so a round in flight is refused at its next request, its next retry sleep or its
/// next streamed refill; the joins stay and the round becomes short. These tests pin the arm, the
/// plane wiring, the sleep wiring, and the scheduler's behaviour around a round that was cut.

namespace DB::ErrorCodes
{
extern const int NETWORK_ERROR;
}

namespace CurrentMetrics
{
extern const Metric LocalThread;
extern const Metric LocalThreadActive;
extern const Metric LocalThreadScheduled;
}

using namespace DB::Cas;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::expectThrowsCode;

namespace
{

PoolPtr openPlainPool(const std::shared_ptr<InMemoryBackend> & backend, PoolConfig config = {})
{
    config.pool_prefix = "p";
    config.server_root_id = "test";
    return Pool::open(backend, config);
}

}

/// The arm is idempotent, observable, and closes the door to new detached work; a drain after an
/// early arm has nothing to wait for.
TEST(CASGCTeardownStop, BeginTeardownIsIdempotentAndRefusesNewDetachedWork)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);

    EXPECT_FALSE(store->teardownBegun());
    store->beginTeardown();
    EXPECT_TRUE(store->teardownBegun());
    store->beginTeardown();
    EXPECT_TRUE(store->teardownBegun()) << "a second arm changes nothing";
    EXPECT_TRUE(store->detachedWorkStoppingForTest());

    EXPECT_FALSE(store->tryDispatchDetached([](DetachedStopToken) {}))
        << "no detached task is accepted once teardown began";
    EXPECT_TRUE(store->stopAndDrainDetachedWork(/*deadline_ms=*/1000))
        << "the drain after an early arm finds nothing in flight and returns at once";
}

/// The open plane -- GC, FSCK, the probe -- refuses after the arm, before anything reaches the
/// backend; the mount plane, which the ref-lane drain and the farewell need alive, does not.
TEST(CASGCTeardownStop, OpenPlaneRefusesAfterTeardownBeganAndTheMountPlaneDoesNot)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPlainPool(backend);
    {
        CasOperation op = store->openRequests().admit();
        orThrow(op.create("p/probe", "v", Retry::once()), "create");
    }
    backend->resetCounts();

    store->beginTeardown();

    CasOperation refused = store->openRequests().admit();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)refused.read("p/probe", Retry::standard()); });
    CasOperation resumed = store->openRequests().resume(/*admitted_generation=*/0);
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)resumed.read("p/probe", Retry::standard()); });
    EXPECT_EQ(backend->getTotal(), 0u) << "a refused admission never reaches the backend";

    CasOperation mount = store->mountRequests().admit();
    ASSERT_TRUE(mount.read("p/probe", Retry::once()).has_value())
        << "the mount plane is not the open plane: teardown's own drain and farewell run on it";
    EXPECT_EQ(backend->getTotal(), 1u);
}

/// The open plane's sleep is the interruptible one, in production wiring and after the test seam is
/// cleared. Arming FIRST makes this a wiring test: a predicate `wait_for` whose predicate already
/// holds returns without waiting, so a plane still wired to the plain sleep cannot pass. The deadline
/// is the assertion; no sleep orders any thread.
TEST(CASGCTeardownStop, OpenPlaneSleepReturnsAtOnceOnceTeardownBegan)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    store->beginTeardown();

    auto paused = std::async(std::launch::async, [&store] { store->openRequests().pause(60'000); });
    EXPECT_EQ(paused.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "the open plane's sleep must observe the arm; a plain sleep holds for the full minute";

    /// Clearing the seam must put the interruptible sleep back, not the engine's plain one.
    store->setCasRetrySleepForTest([](uint64_t) {});
    store->setCasRetrySleepForTest({});
    auto paused_again = std::async(std::launch::async, [&store] { store->openRequests().pause(60'000); });
    EXPECT_EQ(paused_again.wait_for(std::chrono::seconds(10)), std::future_status::ready)
        << "resetting the retry-sleep seam left the open plane on the plain sleep";
}

/// A read-ahead worker resumes under the plane's generation and is refused at its first gate; the
/// fold learns it at the take site. An unconsumed future has its exception dropped by the read-ahead's
/// destructor, so the test consumes it.
TEST(CASGCTeardownStop, ReadAheadWorkerIsRefusedAndTheTakeSiteSeesIt)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPlainPool(backend);
    {
        CasOperation op = store->openRequests().admit();
        orThrow(op.create("p/k1", "one", Retry::once()), "create");
    }
    ThreadPool pool{CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
                    CurrentMetrics::LocalThreadScheduled,
                    /*max_threads*/ 2, /*max_free_threads*/ 2, /*queue_size*/ 0};
    CasOperation op = store->openRequests().admit();
    GcReadAhead reads(op, store->openRequests(), pool, /*concurrency=*/2);

    store->beginTeardown();
    backend->resetCounts();
    reads.hintRead("p/k1");
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)reads.takeRead("p/k1"); });
    EXPECT_EQ(backend->getTotal(), 0u) << "the worker was refused before it reached the backend";
}
