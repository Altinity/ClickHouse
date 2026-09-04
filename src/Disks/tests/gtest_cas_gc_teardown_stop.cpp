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
