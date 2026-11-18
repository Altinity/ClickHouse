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

/// Runs `job(i)` for every `i` below `count`, at most `concurrency` at a time, on threads of its own; the calling thread
/// only submits and joins. Once a job throws, the jobs not started yet are skipped. Every started job is joined before
/// the first failure in index order is rethrown, so nothing a job references outlives the call.
/// A job type with a member `beforeSubmit(i)` has it called on the calling thread before job `i` is submitted; a seam for
/// tests, which no production job has.
template <typename Job>
concept HasBeforeSubmit = requires(const Job & job, size_t i) { job.beforeSubmit(i); };

template <typename Job>
void runBounded(size_t concurrency, size_t count, const Job & job)
{
    if (concurrency <= 1 || count <= 1)
    {
        for (size_t i = 0; i < count; ++i)
            job(i);
        return;
    }
    const size_t threads = std::min(concurrency, count);
    ThreadPool pool(CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
        /*max_threads*/ threads, /*max_free_threads*/ threads, /*queue_size*/ 0, /*shutdown_on_exception*/ false);
    ThreadPoolCallbackRunnerLocal<void> runner(pool, ThreadName::UNKNOWN);
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
