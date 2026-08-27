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

    std::atomic<size_t> num_streams{0};
    std::atomic<size_t> active_prefetch_readers{0};
    ThreadPoolCallbackRunnerFast parsing_runner;
    ThreadPoolCallbackRunnerFast io_runner;

    /// IInputFormat implementation may put arbitrary state here.
    std::shared_ptr<void> opaque;

    FormatParserSharedResources(const Settings & settings, size_t num_streams_);

    static FormatParserSharedResourcesPtr singleThreaded(const Settings & settings);

    void finishStream();

    /// Caps how many files prefetch ahead at the same time (input_format_parquet_max_active_files).
    /// With many files sharing one IO pool, spreading it evenly leaves every file with too few reads
    /// in flight to cover the storage's response time, and none of them finish early. Letting a few
    /// files run at full depth and admitting the rest as those drain reads the same bytes with the
    /// same bandwidth, but frees each file's read-ahead buffers sooner.
    /// A reader that doesn't hold a slot still reads the row group it must deliver next, so a query
    /// cannot stall on this.
    bool tryAcquirePrefetchSlot(size_t max_active);
    void releasePrefetchSlot();

    size_t getParsingThreadsPerReader() const;
    size_t getIOThreadsPerReader() const;

    void initOnce(std::function<void()> f);

private:
    /// For lazily initializing the fields above.
    std::once_flag init_flag;
    std::exception_ptr init_exception;
};
}
