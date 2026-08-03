#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBlobUploadPool.h>

#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>

#include <algorithm>
#include <memory>
#include <mutex>

namespace CurrentMetrics
{
    extern const Metric CASBlobUploadPoolThreads;
    extern const Metric CASBlobUploadPoolThreadsActive;
    extern const Metric CASBlobUploadPoolThreadsScheduled;
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
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "cas_blob_upload_pool_size must not be 0");

    std::lock_guard lock(pool_mutex);
    if (pool_instance)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "The CAS blob upload pool is initialized twice");

    pool_instance = std::make_unique<ThreadPool>(
        CurrentMetrics::CASBlobUploadPoolThreads,
        CurrentMetrics::CASBlobUploadPoolThreadsActive,
        CurrentMetrics::CASBlobUploadPoolThreadsScheduled,
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

ByteWeightedSemaphore::ByteWeightedSemaphore(uint64_t capacity_bytes_)
    : capacity_bytes(capacity_bytes_)
{
}

void ByteWeightedSemaphore::acquire(uint64_t weight)
{
    /// The held hook is copied under the lock and invoked after unlocking, so a rendezvous inside it
    /// never blocks the internal mutex (which would deadlock every other acquire/release).
    std::function<void(uint64_t)> hook_copy;
    {
        std::unique_lock lock(mutex);

        const bool overweight = weight > capacity_bytes;
        if (overweight)
        {
            /// EXCLUSIVE path. Count ourselves as a queued/running exclusive immediately so new NORMAL
            /// admissions are held off (the fairness gate below), which is what makes the drain reachable.
            ++waiting_exclusive;
            bool waited = false;
            while (exclusive_active || in_flight != 0)
            {
                /// Bounded by liveness, not time: `in_flight` only decreases while we wait (new normals
                /// are blocked by `waiting_exclusive > 0`), and any active exclusive eventually releases,
                /// so this wait terminates. It never waits on the unreachable `weight <= capacity`.
                if (!waited && wait_hook)
                    wait_hook();
                waited = true;
                cv.wait(lock);
            }
            --waiting_exclusive;
            exclusive_active = true;
            ++active_holders;
            ++stats.exclusive_grants;
            /// The exclusive body's bytes are NOT added to `in_flight`: it runs alone, so there is no
            /// aggregate to bound -- the invariant for an overweight blob is exclusivity, not the cap.
        }
        else
        {
            bool waited = false;
            while (exclusive_active || waiting_exclusive != 0 || in_flight + weight > capacity_bytes)
            {
                if (!waited && wait_hook)
                    wait_hook();
                waited = true;
                cv.wait(lock);
            }
            in_flight += weight;
            ++active_holders;
            stats.peak_in_flight = std::max(stats.peak_in_flight, in_flight);
        }

        stats.peak_holders = std::max(stats.peak_holders, static_cast<uint32_t>(active_holders));
        /// Exclusivity witness: an exclusive holder must be the ONLY holder. The predicates above make
        /// this hold by construction; the flag catches a regression that broke them.
        if (exclusive_active && active_holders != 1)
            stats.co_hold_violation = true;

        hook_copy = held_hook;
    }

    if (hook_copy)
        hook_copy(weight);
}

void ByteWeightedSemaphore::release(uint64_t weight) noexcept
{
    {
        std::lock_guard lock(mutex);
        /// Fail loud (debug/sanitizer builds) on an unpaired release. The sole production caller
        /// (`ByteWeightedSemaphoreLock`) pairs acquire/release exactly; a trip here means a wiring
        /// regression or a second caller underflowing these unsigned counters, which would otherwise
        /// silently wedge every subsequent admission with no signal.
        chassert(active_holders > 0);
        if (weight > capacity_bytes)
            exclusive_active = false;
        else
        {
            chassert(in_flight >= weight);
            in_flight -= weight;
        }
        --active_holders;
    }
    cv.notify_all();
}

ByteWeightedSemaphore::StatsForTest ByteWeightedSemaphore::statsForTest() const
{
    std::lock_guard lock(mutex);
    return stats;
}

void ByteWeightedSemaphore::resetStatsForTest()
{
    std::lock_guard lock(mutex);
    stats = StatsForTest{};
}

void ByteWeightedSemaphore::setHeldHookForTest(std::function<void(uint64_t)> hook)
{
    std::lock_guard lock(mutex);
    held_hook = std::move(hook);
}

void ByteWeightedSemaphore::setWaitHookForTest(std::function<void()> hook)
{
    std::lock_guard lock(mutex);
    wait_hook = std::move(hook);
}

namespace
{
    /// 64 MiB per pool slot -- the chosen default per-task budget, not a blob-body cap (see
    /// `initializeCondemnedUploadAdmission`'s header comment).
    constexpr uint64_t kCondemnedUploadPerTaskBudgetBytes = 64ULL << 20;

    std::mutex admission_mutex;
    std::unique_ptr<ByteWeightedSemaphore> admission_instance;
}

void initializeCondemnedUploadAdmission(uint64_t configured_capacity_bytes, size_t pool_size)
{
    const uint64_t capacity = configured_capacity_bytes != 0
        ? configured_capacity_bytes
        : static_cast<uint64_t>(pool_size) * kCondemnedUploadPerTaskBudgetBytes;

    if (capacity == 0)
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "The CAS condemned-upload memory cap resolves to 0 bytes "
            "(cas_condemned_upload_memory_bytes and the derived pool_size * 64 MiB are both 0)");

    std::lock_guard lock(admission_mutex);
    if (admission_instance)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "The CAS condemned-upload admission is initialized twice");

    admission_instance = std::make_unique<ByteWeightedSemaphore>(capacity);
}

ByteWeightedSemaphore & condemnedUploadAdmission()
{
    std::lock_guard lock(admission_mutex);
    if (!admission_instance)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "The CAS condemned-upload admission is not initialized");

    return *admission_instance;
}

void shutdownCondemnedUploadAdmission() noexcept
{
    std::lock_guard lock(admission_mutex);
    admission_instance.reset();
}

bool condemnedUploadAdmissionInitializedForTest()
{
    std::lock_guard lock(admission_mutex);
    return admission_instance != nullptr;
}

}
}
