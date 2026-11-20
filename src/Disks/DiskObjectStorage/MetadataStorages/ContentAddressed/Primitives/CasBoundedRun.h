#pragma once
#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <Common/threadPoolCallbackRunner.h>
#include <base/scope_guard.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace DB::Cas
{

/// The threads one tool run fans its requests out over, `concurrency` at most. Created once per run, so a run of many
/// batches starts its threads once.
inline std::unique_ptr<ThreadPool> makeBoundedPool(size_t concurrency)
{
    const size_t threads = std::max<size_t>(concurrency, 1);
    return std::make_unique<ThreadPool>(CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive,
        CurrentMetrics::LocalThreadScheduled, /*max_threads*/ threads, /*max_free_threads*/ threads, /*queue_size*/ 0,
        /*shutdown_on_exception*/ false);
}

/// Runs `job(i)` for every `i` below `count` on `pool`, under `thread_name`, at most the pool's thread count at a time;
/// the calling thread only submits and joins, and runs the jobs itself when the pool has one thread or `count` is 1.
/// Once a job throws, the jobs not started yet are skipped. Every started job is joined before the first failure in
/// index order is rethrown, so nothing a job references outlives the call.
/// A job type with a member `beforeSubmit(i)` has it called on the calling thread before job `i` is submitted; a seam for
/// tests, which no production job has.
template <typename Job>
concept HasBeforeSubmit = requires(const Job & job, size_t i) { job.beforeSubmit(i); };

template <typename Job>
void runBounded(ThreadPool & pool, ThreadName thread_name, size_t count, const Job & job)
{
    if (pool.getMaxThreads() <= 1 || count <= 1)
    {
        for (size_t i = 0; i < count; ++i)
            job(i);
        return;
    }
    ThreadPoolCallbackRunnerLocal<void> runner(pool, thread_name);
    /// Declared before the guard that joins the jobs, so it outlives them when a submission throws.
    std::atomic<bool> failed{false};
    std::vector<std::shared_ptr<ThreadPoolCallbackRunnerLocal<void>::Task>> handles;
    handles.reserve(count);
    SCOPE_EXIT_SAFE({ ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles); });
    for (size_t i = 0; i < count && !failed.load(); ++i)
    {
        if constexpr (HasBeforeSubmit<Job>)
            job.beforeSubmit(i);
        handles.emplace_back(runner.enqueueAndGiveOwnership([&job, &failed, i]
        {
            if (failed.load())
                return;
            try
            {
                job(i);
            }
            catch (...)
            {
                failed.store(true);
                throw;
            }
        }));
    }
    ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinishAndRethrowFirstError(handles);
}

}
