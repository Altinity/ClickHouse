#include <gtest/gtest.h>

#include <Interpreters/ClusterFunctionReadTask.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Common/tests/gtest_global_context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

TEST(StorageObjectStorageSwarm, ConcurrentTaskAdmissionsDoNotSerialize)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable both_inside;
    size_t inside = 0;
    std::atomic<bool> overlapped{false};

    /// Each admission holds the shared lock until the other has entered. An exclusive lock
    /// would keep the second admission out until the first returns.
    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        auto admission = context->lockSwarmMode();
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
        (void)iterator;
    };

    std::thread first(run_admission);
    std::thread second(run_admission);
    first.join();
    second.join();

    EXPECT_TRUE(overlapped.load());
}

TEST(StorageObjectStorageSwarm, StopBeforeSendDoesNotRequestTask)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable ready;
    bool at_send = false;
    bool allow_send = false;
    std::atomic<int> sends{0};

    /// Same order as the `TCPHandler` callback: the admission lock covers only the check and the send.
    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        {
            std::unique_lock lock(mutex);
            at_send = true;
            ready.notify_all();
            ready.wait(lock, [&] { return allow_send; });
        }

        auto admission = context->lockSwarmMode();
        if (!context->isSwarmModeEnabled())
            return std::make_shared<DB::ClusterFunctionReadTaskResponse>();

        ++sends;
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
        entered = ready.wait_for(lock, std::chrono::seconds(5), [&] { return at_send; });
    }
    if (!entered)
    {
        {
            std::lock_guard lock(mutex);
            allow_send = true;
        }
        ready.notify_all();
        worker.join();
        context->startSwarmMode();
        FAIL() << "Task callback did not reach the send";
    }

    EXPECT_TRUE(context->stopSwarmMode());

    {
        std::lock_guard lock(mutex);
        allow_send = true;
    }
    ready.notify_all();
    worker.join();
    context->startSwarmMode();

    ASSERT_FALSE(error);
    EXPECT_EQ(sends.load(), 0);
}

TEST(StorageObjectStorageSwarm, TaskResponseReadDoesNotBlockStop)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable ready;
    bool in_receive = false;
    bool finish_read = false;

    DB::ClusterFunctionReadTaskCallback callback = [&]()
    {
        {
            auto admission = context->lockSwarmMode();
            if (!context->isSwarmModeEnabled())
                return std::make_shared<DB::ClusterFunctionReadTaskResponse>();
        }

        std::unique_lock lock(mutex);
        in_receive = true;
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
        entered = ready.wait_for(lock, std::chrono::seconds(5), [&] { return in_receive; });
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
        FAIL() << "Task callback did not reach the response read";
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
