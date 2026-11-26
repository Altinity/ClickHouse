#include <gtest/gtest.h>

#include <Common/tests/gtest_global_context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

/// These tests call `Context::runWithSwarmTaskAdmission`, the same function the `TCPHandler`
/// read-task callback uses around `sendReadTaskRequest`.

TEST(StorageObjectStorageSwarm, ConcurrentTaskAdmissionsDoNotSerialize)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable both_inside;
    size_t inside = 0;
    std::atomic<bool> overlapped{false};

    auto send = [&]()
    {
        std::unique_lock lock(mutex);
        ++inside;
        both_inside.notify_all();
        if (both_inside.wait_for(lock, std::chrono::seconds(5), [&] { return inside >= 2; }))
            overlapped = true;
    };

    std::thread first([&] { EXPECT_TRUE(context->runWithSwarmTaskAdmission(/*server_cancelled=*/false, send)); });
    std::thread second([&] { EXPECT_TRUE(context->runWithSwarmTaskAdmission(/*server_cancelled=*/false, send)); });
    first.join();
    second.join();

    EXPECT_TRUE(overlapped.load());
}

TEST(StorageObjectStorageSwarm, StopBeforeSendDoesNotRequestTask)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();
    ASSERT_TRUE(context->stopSwarmMode());

    std::atomic<int> sends{0};
    EXPECT_FALSE(context->runWithSwarmTaskAdmission(/*server_cancelled=*/false, [&] { ++sends; }));

    context->startSwarmMode();
    EXPECT_FALSE(context->runWithSwarmTaskAdmission(/*server_cancelled=*/true, [&] { ++sends; }));
    context->startSwarmMode();

    EXPECT_EQ(sends.load(), 0);
}

TEST(StorageObjectStorageSwarm, InFlightSendBlocksStopAndNoRequestFollows)
{
    auto context = DB::Context::createCopy(::getContext().context);
    context->startSwarmMode();

    std::mutex mutex;
    std::condition_variable ready;
    bool in_send = false;
    bool finish_send = false;
    std::atomic<int> sends{0};

    std::thread worker([&]()
    {
        EXPECT_TRUE(context->runWithSwarmTaskAdmission(/*server_cancelled=*/false, [&]()
        {
            ++sends;
            std::unique_lock lock(mutex);
            in_send = true;
            ready.notify_all();
            ready.wait(lock, [&] { return finish_send; });
        }));
    });

    bool entered = false;
    {
        std::unique_lock lock(mutex);
        entered = ready.wait_for(lock, std::chrono::seconds(5), [&] { return in_send; });
    }
    if (!entered)
    {
        {
            std::lock_guard lock(mutex);
            finish_send = true;
        }
        ready.notify_all();
        worker.join();
        context->startSwarmMode();
        FAIL() << "Task send did not start";
    }

    auto stop_result = std::async(std::launch::async, [&] { return context->stopSwarmMode(); });
    EXPECT_EQ(stop_result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);

    {
        std::lock_guard lock(mutex);
        finish_send = true;
    }
    ready.notify_all();

    const auto stop_status = stop_result.wait_for(std::chrono::seconds(5));
    worker.join();
    ASSERT_EQ(stop_status, std::future_status::ready);
    EXPECT_TRUE(stop_result.get());

    EXPECT_FALSE(context->runWithSwarmTaskAdmission(/*server_cancelled=*/false, [&] { ++sends; }));
    context->startSwarmMode();
    EXPECT_EQ(sends.load(), 1);
}
