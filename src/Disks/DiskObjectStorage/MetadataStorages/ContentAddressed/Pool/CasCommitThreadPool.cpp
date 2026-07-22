#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasCommitThreadPool.h>
#include <Common/CurrentMetrics.h>
#include <Common/Exception.h>
#include <Common/ThreadPool.h>
#include <memory>
#include <mutex>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

/// The pool object itself, plus the mutex guarding construction/destruction -- mirroring
/// `StaticThreadPool`'s `initialize`/`get`/`shutdown` (`IO/SharedThreadPools.cpp`), but kept private to
/// this translation unit rather than reusing that class: this pool is a single, CAS-owned singleton,
/// not one more entry in the generic `APPLY_FOR_STATIC_THREAD_POOLS` registry shared by unrelated
/// subsystems (IO, backups, MergeTree...).
///
/// Deliberately fails loud on both ends: `get()` throws if `initialize()` hasn't run yet, and
/// `initialize()` throws if called twice. There is no lazy self-initializing fallback with a
/// hardcoded default -- that would make `cas_commit_pool_size` a silent no-op for any caller that
/// reaches the pool before `programs/server/Server.cpp` wires it up.
class CasCommitPoolHolder
{
public:
    ThreadPool & get()
    {
        std::lock_guard lock(mutex);
        if (!instance)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "CAS commit thread pool is not initialized");
        return *instance;
    }

    void initialize(size_t max_threads, size_t max_free_threads, size_t queue_size)
    {
        std::lock_guard lock(mutex);
        /// Argument validation runs BEFORE the double-init check on purpose: a zero-thread pool is a
        /// misconfiguration regardless of init state, and validating first keeps this check testable
        /// (a unit test can assert the throw without first poisoning the already-initialized
        /// process-wide singleton -- the throw here leaves `instance` untouched). A zero-thread pool
        /// constructs fine but can never run a scheduled callback, so `commit()`'s
        /// `waitForAllToFinish` would block FOREVER on every nonempty commit. Fail-closed (CLAUDE.md:
        /// no silent fallback) -- reject rather than clamp, so the misconfiguration surfaces at startup.
        if (max_threads == 0)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
                "CAS commit thread pool size must be at least 1 (got 0); a zero-thread pool can never "
                "run a scheduled commit callback and would deadlock every commit");
        if (instance)
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "CAS commit thread pool is initialized twice");

        instance = std::make_unique<ThreadPool>(
            CurrentMetrics::LocalThread,
            CurrentMetrics::LocalThreadActive,
            CurrentMetrics::LocalThreadScheduled,
            max_threads,
            max_free_threads,
            queue_size,
            /* shutdown_on_exception= */ false);
    }

    void shutdown()
    {
        std::lock_guard lock(mutex);
        instance.reset();
    }

private:
    std::mutex mutex;
    std::unique_ptr<ThreadPool> instance;
};

CasCommitPoolHolder & holder()
{
    static CasCommitPoolHolder instance;
    return instance;
}

}

ThreadPool & getCasCommitThreadPool()
{
    return holder().get();
}

void initializeCasCommitThreadPool(size_t max_threads, size_t max_free_threads, size_t queue_size)
{
    holder().initialize(max_threads, max_free_threads, queue_size);
}

void shutdownCasCommitThreadPool()
{
    holder().shutdown();
}

}
