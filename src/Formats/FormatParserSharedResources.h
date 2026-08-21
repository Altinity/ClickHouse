#pragma once

#include <Common/threadPoolCallbackRunner.h>
#include <Interpreters/Context_fwd.h>

namespace DB
{

struct Settings;

struct FormatParserSharedResources;
using FormatParserSharedResourcesPtr = std::shared_ptr<FormatParserSharedResources>;

/// When reading many files in one query, e.g. `SELECT ... FROM file('part{00..99}.parquet')`,
/// we want the file readers to share some resource limits, e.g. number of threads.
/// This struct is shared among such group of readers (IInputFormat instances).
///
/// All nontrivial parts of this struct are lazily initialized by the IInputFormat implementation,
/// because most implementations don't use most of this struct.
struct FormatParserSharedResources
{
    const size_t max_parsing_threads = 0;
    const size_t max_io_threads = 0;

    /// How many readers a single stream may keep concurrently active (e.g. via
    /// `object_storage_max_files_to_prefetch` priming several files' readers ahead of the one
    /// currently being consumed). Per-reader budgets (see Parquet's `getLimitsPerReader`) divide by
    /// `num_streams * max_concurrent_readers_per_stream`, not just `num_streams`, so that priming
    /// more files ahead doesn't let each of them claim a whole stream's share of memory/threads as
    /// if it were the only reader alive for that stream. Defaults to 1 (today's behaviour: exactly
    /// one active reader per stream) so this is a no-op unless a caller opts into more.
    const size_t max_concurrent_readers_per_stream = 1;

    std::atomic<size_t> num_streams{0};
    ThreadPoolCallbackRunnerFast parsing_runner;
    ThreadPoolCallbackRunnerFast io_runner;

    /// IInputFormat implementation may put arbitrary state here.
    std::shared_ptr<void> opaque;

    FormatParserSharedResources(const Settings & settings, size_t num_streams_);

    static FormatParserSharedResourcesPtr singleThreaded(const Settings & settings);

    void finishStream();

    size_t getParsingThreadsPerReader() const;
    size_t getIOThreadsPerReader() const;

    void initOnce(std::function<void()> f);

private:
    /// For lazily initializing the fields above.
    std::once_flag init_flag;
    std::exception_ptr init_exception;
};
}
