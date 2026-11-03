#include <gtest/gtest.h>

#include <Interpreters/ClusterFunctionReadTask.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Common/FailPoint.h>
#include <Common/tests/gtest_global_context.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace DB::FailPoints
{
extern const char swarm_mode_during_task_retry_wait[];
}

TEST(StorageObjectStorageSwarm, StopDoesNotBlockDuringTaskRetryWait)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();
    DB::FailPointInjection::enableFailPoint(DB::FailPoints::swarm_mode_during_task_retry_wait);

    std::atomic<int> callbacks{0};
    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        ++callbacks;
        auto response = std::make_shared<DB::ClusterFunctionReadTaskResponse>();
        response->retry_after_us = 60'000'000;
        return response;
    };

    std::exception_ptr error;
    std::thread worker([&]()
    {
        try
        {
            DB::StorageObjectStorageSource::ReadTaskIterator iterator(
                callback,
                /*max_threads_count=*/1,
                /*is_archive_=*/false,
                /*object_storage_=*/nullptr,
                /*table_location_=*/"",
                context);
            (void)iterator.next(/*processor=*/0);
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });

    bool stop_finished = false;
    bool stopped = false;
    std::future<bool> stop_result;
    try
    {
        DB::FailPointInjection::waitForPause(DB::FailPoints::swarm_mode_during_task_retry_wait);

        /// `stopSwarmMode` takes the admission mutex. It must return while the worker is still
        /// paused in the retry wait. A lock held across that wait would block until the pause ends.
        stop_result = std::async(std::launch::async, [&] { return context->stopSwarmMode(); });
        stop_finished = stop_result.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        if (stop_finished)
            stopped = stop_result.get();
    }
    catch (...)
    {
        DB::FailPointInjection::disableFailPoint(DB::FailPoints::swarm_mode_during_task_retry_wait);
        if (stop_result.valid())
            stop_result.wait();
        worker.join();
        context->startSwarmMode();
        throw;
    }

    DB::FailPointInjection::disableFailPoint(DB::FailPoints::swarm_mode_during_task_retry_wait);
    if (stop_result.valid())
        stop_result.wait();
    worker.join();
    context->startSwarmMode();

    ASSERT_FALSE(error);
    ASSERT_TRUE(stop_finished);
    EXPECT_TRUE(stopped);
    /// Prefetch asks once and does not wait. The following `next` asks once, then sees swarm mode
    /// stopped and must not ask again.
    EXPECT_EQ(callbacks.load(), 2);
}
