#include <gtest/gtest.h>

#include <Interpreters/ClusterFunctionReadTask.h>
#include <Storages/ObjectStorage/StorageObjectStorageSource.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/tests/gtest_global_context.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace DB::ErrorCodes
{
    extern const int CANNOT_UNPACK_ARCHIVE;
    extern const int LOGICAL_ERROR;
}

namespace
{
class UnknownSizeObjectStorage final : public DB::IObjectStorage
{
public:
    std::string getName() const override { return "UnknownSize"; }
    DB::ObjectStorageType getType() const override { return DB::ObjectStorageType::Web; }
    std::string getCommonKeyPrefix() const override { return ""; }
    std::string getDescription() const override { return "unknown size test storage"; }
    bool exists(const DB::StoredObject &) const override { return true; }

    DB::ObjectMetadata getObjectMetadata(const std::string &, bool) const override { return makeMetadata(); }
    DB::ObjectMetadata getObjectMetadata(const DB::RelativePathWithMetadata &, bool) const override { return makeMetadata(); }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string &, bool) const override { return makeMetadata(); }
    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const DB::RelativePathWithMetadata &, bool) const override { return makeMetadata(); }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject &,
        const DB::ReadSettings &,
        std::optional<size_t>,
        bool,
        bool) const override
    {
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "The archive reader should not read an object of unknown size");
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject &,
        DB::WriteMode,
        std::optional<DB::ObjectAttributes>,
        size_t,
        const DB::WriteSettings &) override
    {
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Unexpected write");
    }

    bool isRemote() const override { return true; }
    void removeObjectIfExists(const DB::StoredObject &) override {}
    void removeObjectsIfExist(const DB::StoredObjects &) override {}

    void copyObject(
        const DB::StoredObject &,
        const DB::StoredObject &,
        const DB::ReadSettings &,
        const DB::WriteSettings &,
        std::optional<DB::ObjectAttributes>) override
    {
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Unexpected copy");
    }

    void shutdown() override {}
    void startup() override {}
    std::string getObjectsNamespace() const override { return ""; }
    DB::ObjectStorageKeyGeneratorPtr createKeyGenerator() const override { return nullptr; }

private:
    static DB::ObjectMetadata makeMetadata()
    {
        DB::ObjectMetadata metadata;
        metadata.is_size_known = false;
        metadata.size_bytes = 0;
        return metadata;
    }
};
}

TEST(StorageObjectStorageArchive, DistributedArchiveRejectsUnknownSize)
{
    bool sent_task = false;
    DB::ClusterFunctionReadTaskCallback callback = [&sent_task]
    {
        if (sent_task)
            return std::make_shared<DB::ClusterFunctionReadTaskResponse>();

        sent_task = true;
        return std::make_shared<DB::ClusterFunctionReadTaskResponse>("archive.zip :: value.tsv");
    };

    auto object_storage = std::make_shared<UnknownSizeObjectStorage>();
    auto context = DB::Context::createCopy(::getContext().context);
    DB::StorageObjectStorageSource::ReadTaskIterator iterator(
        callback,
        /*max_threads_count=*/1,
        /*is_archive_=*/true,
        object_storage,
        /*table_location_=*/"",
        context);

    try
    {
        (void)iterator.next(/*processor=*/0);
        FAIL() << "Expected CANNOT_UNPACK_ARCHIVE";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CANNOT_UNPACK_ARCHIVE);
        EXPECT_NE(std::string_view(e.message()).find("size is unknown"), std::string_view::npos);
    }
}

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
