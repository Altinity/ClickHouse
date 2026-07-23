#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.h>

#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>

#include <memory>
#include <mutex>

namespace CurrentMetrics
{
    extern const Metric CasBlobUploadPoolThreads;
    extern const Metric CasBlobUploadPoolThreadsActive;
    extern const Metric CasBlobUploadPoolThreadsScheduled;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
}

namespace Cas
{

namespace
{
    std::mutex pool_mutex;
    std::unique_ptr<ThreadPool> pool_instance;
}

void initializeBlobUploadPool(size_t size)
{
    if (size == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "content_addressed_blob_upload_pool_size must not be 0");

    std::lock_guard lock(pool_mutex);
    if (pool_instance)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "The CAS blob upload pool is initialized twice");

    pool_instance = std::make_unique<ThreadPool>(
        CurrentMetrics::CasBlobUploadPoolThreads,
        CurrentMetrics::CasBlobUploadPoolThreadsActive,
        CurrentMetrics::CasBlobUploadPoolThreadsScheduled,
        size);
}

ThreadPool & blobUploadPool()
{
    std::lock_guard lock(pool_mutex);
    if (!pool_instance)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "The CAS blob upload pool is not initialized");

    return *pool_instance;
}

void shutdownBlobUploadPool() noexcept
{
    std::lock_guard lock(pool_mutex);
    pool_instance.reset();
}

bool blobUploadPoolInitializedForTest()
{
    std::lock_guard lock(pool_mutex);
    return pool_instance != nullptr;
}

}
}
