#include <gtest/gtest.h>

#include <Interpreters/ClusterFunctionReadTask.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Common/FailPoint.h>
#include <Common/tests/gtest_global_context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
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

TEST(StorageObjectStorageSwarm, ConcurrentTaskAdmissionsDoNotSerialize)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable both_inside;
    size_t inside = 0;
    std::atomic<bool> overlapped{false};

    /// Each admission waits for the other to enter the callback. With an exclusive admission
    /// lock the second one cannot enter until the first leaves, so the wait times out.
    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        std::unique_lock lock(mutex);
        ++inside;
        both_inside.notify_all();
        if (both_inside.wait_for(lock, std::chrono::seconds(5), [&] { return inside >= 2; }))
            overlapped = true;
        return std::make_shared<DB::ClusterFunctionReadTaskResponse>();
    };

    auto run_admission = [&]()
    {
        DB::StorageObjectStorageSource::ReadTaskIterator iterator(
            callback,
            /*max_threads_count=*/1,
            /*is_archive_=*/false,
            /*object_storage_=*/nullptr,
            /*table_location_=*/"",
            context);
    };

    std::thread first(run_admission);
    std::thread second(run_admission);
    first.join();
    second.join();

    EXPECT_TRUE(overlapped.load());
}

TEST(StorageObjectStorageSwarm, TaskResponseReadDoesNotBlockStop)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable ready;
    bool lock_released = false;
    bool finish_read = false;
    std::atomic<int> callbacks{0};

    /// The response read releases the admission lock, then waits. `stopSwarmMode` must return
    /// during that wait. The callback still returns, so the worker query is not failed.
    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        ++callbacks;
        context->releaseSwarmAdmissionLock();

        std::unique_lock lock(mutex);
        lock_released = true;
        ready.notify_all();
        ready.wait(lock, [&] { return finish_read; });
        return std::make_shared<DB::ClusterFunctionReadTaskResponse>();
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
            (void)iterator;
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });

    bool entered = false;
    {
        std::unique_lock lock(mutex);
        entered = ready.wait_for(lock, std::chrono::seconds(5), [&] { return lock_released; });
    }
    if (!entered)
    {
        {
            std::lock_guard lock(mutex);
            finish_read = true;
        }
        ready.notify_all();
        worker.join();
        context->startSwarmMode();
        FAIL() << "Task callback did not release the admission lock";
    }

    auto stop_result = std::async(std::launch::async, [&] { return context->stopSwarmMode(); });
    const bool stop_finished = stop_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    bool stopped = false;
    if (stop_finished)
        stopped = stop_result.get();

    {
        std::lock_guard lock(mutex);
        finish_read = true;
    }
    ready.notify_all();
    if (stop_result.valid())
        stop_result.wait();
    worker.join();
    context->startSwarmMode();

    ASSERT_FALSE(error);
    ASSERT_TRUE(stop_finished);
    EXPECT_TRUE(stopped);
    EXPECT_EQ(callbacks.load(), 1);
}

TEST(StorageObjectStorageSwarm, SiblingAdmissionDoesNotPinStopDuringResponseRead)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    /// Stands in for one connection's `callback_mutex`. The task response read holds it.
    std::mutex connection_mutex;
    std::mutex mutex;
    std::condition_variable ready;
    int reached_connection = 0;
    bool reader_in_receive = false;
    bool finish_read = false;

    /// Same order as the `TCPHandler` read-task callback: drop the caller's admission lock,
    /// wait for the connection mutex, then hold the admission lock only around the send.
    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        context->releaseSwarmAdmissionLock();
        {
            std::lock_guard lock(mutex);
            ++reached_connection;
        }
        ready.notify_all();

        std::lock_guard connection_lock(connection_mutex);
        {
            auto admission = context->lockSwarmMode();
            if (!context->isSwarmModeEnabled() || context->isSwarmTaskAdmissionInterrupted())
                return std::make_shared<DB::ClusterFunctionReadTaskResponse>();
        }

        std::unique_lock lock(mutex);
        if (!reader_in_receive)
        {
            reader_in_receive = true;
            ready.notify_all();
            ready.wait(lock, [&] { return finish_read; });
        }
        return std::make_shared<DB::ClusterFunctionReadTaskResponse>();
    };

    std::exception_ptr error;
    std::thread worker([&]()
    {
        try
        {
            DB::StorageObjectStorageSource::ReadTaskIterator iterator(
                callback,
                /*max_threads_count=*/2,
                /*is_archive_=*/false,
                /*object_storage_=*/nullptr,
                /*table_location_=*/"",
                context);
            (void)iterator;
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });

    bool both_reached = false;
    {
        std::unique_lock lock(mutex);
        both_reached = ready.wait_for(lock, std::chrono::seconds(5), [&] { return reached_connection >= 2 && reader_in_receive; });
    }
    if (!both_reached)
    {
        {
            std::lock_guard lock(mutex);
            finish_read = true;
        }
        ready.notify_all();
        worker.join();
        context->startSwarmMode();
        FAIL() << "Sibling admissions did not reach the response read";
    }

    auto stop_result = std::async(std::launch::async, [&] { return context->stopSwarmMode(); });
    const bool stop_finished = stop_result.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    bool stopped = false;
    if (stop_finished)
        stopped = stop_result.get();

    {
        std::lock_guard lock(mutex);
        finish_read = true;
    }
    ready.notify_all();
    if (stop_result.valid())
        stop_result.wait();
    worker.join();
    context->startSwarmMode();

    ASSERT_FALSE(error);
    ASSERT_TRUE(stop_finished);
    EXPECT_TRUE(stopped);
}

TEST(StorageObjectStorageSwarm, InterruptUnblocksTaskAdmissionBeforeStop)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable ready;
    bool in_admission = false;
    std::atomic<int> callbacks{0};

    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        if (++callbacks == 1)
        {
            auto response = std::make_shared<DB::ClusterFunctionReadTaskResponse>();
            response->retry_after_us = 1;
            return response;
        }

        std::unique_lock lock(mutex);
        in_admission = true;
        ready.notify_all();
        ready.wait(lock, [&] { return context->isSwarmTaskAdmissionInterrupted(); });
        return std::make_shared<DB::ClusterFunctionReadTaskResponse>();
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

    bool entered = false;
    {
        std::unique_lock lock(mutex);
        entered = ready.wait_for(lock, std::chrono::seconds(5), [&] { return in_admission; });
    }
    if (!entered)
    {
        context->interruptSwarmTaskAdmission();
        ready.notify_all();
        worker.join();
        context->startSwarmMode();
        FAIL() << "Task admission callback did not start";
    }

    auto stop_result = std::async(std::launch::async, [&] { return context->stopSwarmMode(); });
    /// The callback still holds the admission lock, so stop cannot finish yet.
    EXPECT_EQ(stop_result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);

    context->interruptSwarmTaskAdmission();
    ready.notify_all();

    ASSERT_EQ(stop_result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(stop_result.get());

    worker.join();
    context->startSwarmMode();

    ASSERT_FALSE(error);
    EXPECT_EQ(callbacks.load(), 2);
}
