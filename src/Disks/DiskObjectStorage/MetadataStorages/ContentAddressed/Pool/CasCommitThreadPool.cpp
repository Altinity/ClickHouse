#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasCommitThreadPool.h>
#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <memory>
#include <mutex>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace DB::Cas
{

namespace
{

/// The compiled-in default used only when nobody (server startup, a test) has explicitly sized the
/// pool yet -- kept in sync with `cas_commit_pool_size`'s default (`Core/ServerSettings.cpp`), but not
/// derived from it: this file must not depend on `ServerSettings` (a unit test constructing this pool
/// has no server settings object at all).
constexpr size_t default_max_threads = 100;

/// The pool object itself, plus the `std::call_once` guard that makes both entry points below
/// (an explicit server-startup size, or a lazy self-initializing first access) race-free and
/// idempotent -- mirroring `StaticThreadPool`'s `initialize`/`initializeWithDefaultSettingsIfNotInitialized`
/// pair (`IO/SharedThreadPools.h`), but kept private to this translation unit rather than reusing that
/// class: this pool is a single, CAS-owned singleton, not one more entry in the generic
/// `APPLY_FOR_STATIC_THREAD_POOLS` registry shared by unrelated subsystems (IO, backups, MergeTree...).
class CasCommitPoolHolder
{
public:
    ThreadPool & get()
    {
        std::call_once(init_flag, [this] { initializeImpl(default_max_threads, 0, 10000); });
        return *instance;
    }

    void initialize(size_t max_threads, size_t max_free_threads, size_t queue_size)
    {
        std::call_once(init_flag, [this, max_threads, max_free_threads, queue_size]
        {
            initializeImpl(max_threads, max_free_threads, queue_size);
        });
    }

private:
    void initializeImpl(size_t max_threads, size_t max_free_threads, size_t queue_size)
    {
        instance = std::make_unique<ThreadPool>(
            CurrentMetrics::LocalThread,
            CurrentMetrics::LocalThreadActive,
            CurrentMetrics::LocalThreadScheduled,
            max_threads,
            max_free_threads,
            queue_size,
            /* shutdown_on_exception= */ false);
    }

    std::once_flag init_flag;
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

}
