#pragma once

#include <Common/PODArray.h>
#include <Common/Stopwatch.h>
#include <Processors/Formats/Impl/Parquet/ReadCommon.h>

#include <condition_variable>
#include <functional>
#include <optional>
#include <span>

namespace DB
{
class ReadBuffer;
class SeekableReadBuffer;
}

namespace DB
{
class CachedInMemoryReadBufferFromFile;
}

namespace DB::Parquet
{

class PrefetchHandle;

class Prefetcher
{
private:
    struct RequestState;
    struct Task;

public:
    void init(ReadBuffer * reader_, const ReadOptions & options, FormatParserSharedResourcesPtr parser_shared_resources_);

    /// Waits for in-progress reads to complete, cancels queued reads that haven't started yet.
    ~Prefetcher();

    /// Not thread safe.
    /// All ranges must be registered before any reading happens (except direct readSync).
    /// Ranges are allowed to overlap a little, but this decreases the effectiveness of range
    /// coalescing, and the overlap might be read from file multiple times.
    /// (We use overlap to simplify bloom filter header reading a little.)
    /// If likely_to_be_used is true, Prefetcher will be more eager to piggy-back this range when
    /// reading other ranges.
    PrefetchHandle registerRange(size_t offset, size_t length, bool likely_to_be_used);

    /// Called at most once, after all registerRange calls and before all enqueue/getRangeData calls.
    void finalizeRanges();

    /// Replace a requested range with a set of disjoint smaller ranges contained within it.
    /// `subranges` must be sorted.
    std::vector<PrefetchHandle> splitRange(
        PrefetchHandle request, const std::vector<std::pair</*global_offset*/ size_t, /*length*/ size_t>> & subranges, bool likely_to_be_used);

    /// Kicks off background tasks to prefetch these range, if needed (if not already started, and
    /// prefetching is enabled, and handle is valid).
    /// Adds the range's memory usage to MemoryUsageDiff. Remembers memory_usage->stage so that
    /// PrefetchHandle::reset can later subtract from MemoryUsageDiff correctly.
    void startPrefetch(const std::vector<PrefetchHandle *> & requests_to_start, MemoryUsageDiff * diff);

    /// If prefetched, returns prefetched data.
    /// If prefetch in progress, waits for it to complete.
    /// If prefetch not started, reads the data right here.
    /// The returned pointer is valid as long as the PrefetchHandle is alive.
    std::span<const char> getRangeData(const PrefetchHandle & request);

    /// Pass-through read from the underlying ReadBuffer.
    /// `on_progress(m)` reports that the first `m` bytes of `to` have been filled, i.e. a count
    /// cumulative over the whole call, per `SeekableReadBuffer::readBigAt`'s contract. `ReadBufferFromS3`,
    /// `ReadWriteBufferFromHTTP` and `CachedOnDiskReadBufferFromFile` report progress mid-transfer;
    /// `ReadBufferFromS3` restarts the count near zero on each retry attempt, so the value can go
    /// backwards and `publishBytesReady`'s monotonic guard absorbs that. Transports that never call
    /// it (local `pread`, Azure, HDFS) make readiness equal completion.
    void readSync(char * to, size_t n, size_t offset, const std::function<void(size_t /*bytes filled so far*/)> & on_progress = {});

    size_t getFileSize() const { return file_size; }

    /// Fitted round-trip time and bandwidth of source reads, EWMA (alpha 0.2) over tasks read from
    /// the source. Tasks served from the cache are excluded (both the single-cell zero-copy path and
    /// the multi-cell memcpy-assembled path of `readBigAtRetainCells`): they either have no wire
    /// transfer at all, or a fast local memcpy that isn't representative of the source's bandwidth.
    /// Used to size the number of concurrently in-flight reads (see `targetBytesInFlight`).
    struct ReadStats
    {
        double rtt_us = 50'000;            // prior: 50 ms
        double bandwidth_bytes_per_us = 64; // prior: ~64 MB/s per stream
        /// Highest per-stream bandwidth seen so far; 0 before any sample.
        double bandwidth_peak_bytes_per_us = 0;
        size_t samples = 0;
    };
    /// Snapshot of the fitted stats; takes `stats_mutex` (shared with the rare per-task update).
    ReadStats readStats() const;
    /// Sum of `length` of tasks a thread is actually reading right now: counts from the
    /// `Scheduled` -> `Running` CAS in `runTask` until that call finishes the read. Tasks handed to
    /// the IO pool but not yet picked up by one of its threads are *not* counted -- they occupy queue
    /// space, not the storage's pipe, and counting them made the read-ahead target a limit on queue
    /// length rather than on read depth (the pool executes `io_threads` reads at a time no matter how
    /// many are queued behind them).
    size_t bytesInFlight() const { return bytes_executing.load(std::memory_order_relaxed); }
    /// Sum of `length` of tasks that are `Scheduled` or `Running`, i.e. including the ones still
    /// waiting for an IO thread. Diagnostics only: no budget is derived from it.
    size_t bytesQueued() const { return bytes_in_flight.load(std::memory_order_relaxed); }
    /// Length of the range a handle pins, 0 for an empty handle. Doesn't touch the handle's task or
    /// any other shared state, so the read-path planner can use it to size reads it hasn't started.
    size_t requestLength(const PrefetchHandle & handle) const;
    /// How many bytes we'd like to have in flight at once, given `concurrency` concurrent readers:
    /// bandwidth * round-trip time is the amount of data one stream keeps "in the pipe"; multiplying
    /// by `concurrency` and by a headroom factor of 2 keeps all streams busy despite jitter. Floored
    /// at 4 read tasks so we don't undershoot before any samples have been collected.
    size_t targetBytesInFlight(size_t concurrency) const;

private:
    friend class PrefetchHandle;

    /// Corresponds to PrefetchHandle.
    struct RequestState
    {
        /// State transitions:
        ///
        /// HasRange -> HasTask
        ///       |      |
        ///       v      v
        ///       Cancelled
        ///
        /// Transition to HasTask happen with `mutex` locked, after assigning `task` and `task_offset`.
        enum class State
        {
            HasRange,
            HasTask,
            Cancelled, // PrefetchHandle was reset
        };

        std::atomic<State> state {State::HasRange};

        /// Whether this range can be piggy-backed to nearby other reads.
        std::atomic<bool> allow_incidental_read {true};

        Task * task = nullptr; // if HasTask
        size_t range_set_idx = 0;
        size_t range_idx = UINT64_MAX;
        size_t length = 0;
        size_t task_offset = 0;
    };

    /// Range that the user wants, before coalescing. Overlapping ranges are allowed, but are not
    /// handled optimally and should be avoided when possible.
    struct RangeState
    {
        RequestState * request;

        size_t start;
        size_t end;

        size_t length() const { return end - start; }
    };

    /// A range to read from file. May cover multiple request ranges.
    /// Tasks' ranges may overlap (if requested ranges overlap).
    struct Task
    {
        enum class State : UInt8
        {
            Scheduled,
            Running,
            Done,
            Exception,
            /// This range is no longer needed, `buf` can be deallocated.
            /// Task may still be running; in this case, the runner will deallocate `buf` when done.
            Deallocated,
        };

        /// Back-pointer to the owning Prefetcher, needed by `decreaseTaskRefcount` (static, called
        /// from `PrefetchHandle::reset` which doesn't otherwise have a Prefetcher to reach) to
        /// subtract from `bytes_in_flight` when a `Scheduled` task is dropped before anyone runs it.
        Prefetcher * owner = nullptr;

        size_t offset{};
        size_t length{};
        double memory_amplification = 1;

        /// TODO [parquet]: If the range is long, it may make sense to have multiple subtasks reading parts of
        ///       the range in parallel (into subranges of one buffer). E.g. if there's a big column
        ///       chunk with no offset index, and we're reading over network.
        PaddedPODArray<char> buf;

        /// When the underlying read buffer supports zero-copy cached reads, and the Task's range
        /// happens to fit in one retained cache cell, we reference that cell here and don't use `buf`.
        /// Lightweight mirror of SeekableReadBuffer::CachedRegion to avoid the heavy include.
        struct CachedReadRegion
        {
            std::shared_ptr<void> handle;
            const char * data = nullptr;
            size_t size = 0;
            size_t file_offset = 0;
        };
        std::optional<CachedReadRegion> cached_region;

        std::atomic<State> state {State::Scheduled};
        /// How many RequestState-s in HasTask state point to this Task.
        std::atomic<size_t> refcount {};
        /// Notified when the state changes from Running to Done or Exception.
        CompletionNotification completion;
        /// Bytes of `buf` (or `cached_region`) that have landed, counted from `offset`. Monotonic.
        /// Ranges inside a task are sorted by offset and object storage streams a range request in
        /// order, so a request whose end is <= `bytes_ready` can be served before the task finishes.
        /// Only ever written by the one thread executing `runTask` for this task (see
        /// `publishBytesReady`), so the read-modify-write there isn't itself racing with another writer.
        std::atomic<size_t> bytes_ready {0};
        /// Number of threads currently blocked in `waitForBytes` for this task. `publishBytesReady`
        /// only bothers taking `ready_mutex` and notifying `ready_cv` when this is nonzero; each
        /// waiter still re-checks its own `need` against `bytes_ready` after waking (see `waitForBytes`).
        std::atomic<size_t> waiters {0};
        std::exception_ptr exception;

        /// Restarted in `runTask` right before the source read starts. Only ever touched by the one
        /// thread executing `runTask` for this task (and by `publishBytesReady`, which is called
        /// only from that same thread), so no synchronization is needed for the timer itself.
        Stopwatch stopwatch;
        /// Microseconds from `stopwatch`'s restart to the first `publishBytesReady` call for this
        /// task (`bytes_ready` going from 0 to nonzero), floored to 1 so 0 stays available as an
        /// unambiguous "never set" sentinel. Left at 0 only for tasks served from the cache (no
        /// `publishBytesReady` call at all in the single-cell zero-copy case; the multi-cell case
        /// does call it, but at the very end, and is excluded from stats by `served_from_cache`
        /// regardless). For the buffered (`readSync`) path this is always eventually set, even when
        /// the transport never reports progress mid-transfer (local `pread`, Azure, HDFS): `readSync`
        /// makes one unconditional completion call at the end either way. Whether that lone call is a
        /// genuine first byte or just the completion marker is `transport_progress`'s job to tell
        /// apart, not this field's. Same single-writer-thread reasoning as `stopwatch`; atomic only so
        /// a debugger/future reader doesn't need to reason about tearing.
        std::atomic<uint64_t> first_byte_us {0};
    };

    enum class ReadMode
    {
        /// The normal mode: use reader->readBigAt, no read_mutex.
        RandomRead,
        /// Slow mode: use reader->seek and reader->next with read_mutex.
        SeekAndRead,
        /// The whole file was read into `entire_file`, no further reading required.
        EntireFileIsInMemory,
    };

    struct RangeSet
    {
        /// Pre-registered ranges. Sorted and immutable after finalizeRanges().
        std::vector<RangeState> ranges;
    };

    FormatParserSharedResourcesPtr parser_shared_resources;

    std::mutex read_mutex;
    ReadMode read_mode{};
    SeekableReadBuffer * reader = nullptr;
    /// Non-null only when `reader` is the userspace page cache buffer, which can answer "is this range
    /// cached" without touching its own read position. Owned by `reader`, valid for its lifetime.
    CachedInMemoryReadBufferFromFile * cache_probe = nullptr;
    PaddedPODArray<char> entire_file;

    size_t file_size{};
    size_t min_bytes_for_seek{};
    size_t bytes_per_read_task{};
    /// min(min_bytes_for_seek, options.coalesce_gap_bytes), or min_bytes_for_seek if the setting is 0.
    size_t gap_bytes{};
    double max_read_amplification = 0;
    /// See `exceedsAmplification`.
    size_t read_amplification_floor_bytes = 0;

    std::shared_ptr<ShutdownHelper> shutdown = std::make_shared<ShutdownHelper>();

    /// Locked when creating a Task.
    std::mutex mutex;

    /// Arenas.
    std::deque<RequestState> requests;
    std::deque<Task> tasks;
    std::deque<RangeSet> range_sets;

    std::atomic<bool> ranges_finalized {false};

    /// Sum of `length` of tasks that are `Scheduled` or `Running`: incremented in `scheduleTask`,
    /// decremented exactly once per task, either at the end of `runTask` (success or exception) or,
    /// if the task is dropped before any thread runs it, in `decreaseTaskRefcount`.
    std::atomic<size_t> bytes_in_flight {0};

    /// Sum of `length` of tasks between the `Scheduled` -> `Running` CAS in `runTask` and the end of
    /// the read there. Always <= `bytes_in_flight`; this is what `bytesInFlight` reports and what the
    /// read-ahead target is compared against. A task dropped while still `Scheduled` never enters
    /// this counter, so `decreaseTaskRefcount` has nothing to undo here.
    std::atomic<size_t> bytes_executing {0};

    /// Protects the ReadStats accumulators below. Updated at most once per completed task (rare),
    /// so a mutex is simpler than lock-free fixed-point atomics.
    mutable std::mutex stats_mutex;
    double stat_rtt_us = 50'000;
    double stat_bandwidth_bytes_per_us = 64;
    /// Highest per-stream bandwidth this reader has seen. Together with the current value it estimates
    /// how saturated the link is: when many streams share a saturated pipe, each one's measured
    /// bandwidth falls, and bytes added to one stream are taken from the others.
    double stat_bandwidth_peak_bytes_per_us = 0;
    size_t stat_samples = 0;
    /// Folds one task's timing into the EWMAs above and into the profile events. Called at the end
    /// of `runTask` for tasks read from the source (see call site for the exact conditions).
    void updateReadStats(const Task * task, uint64_t total_us, bool transport_progress);

    /// (One mutex for all tasks because it's not used frequently.)
    std::mutex exception_mutex;

    /// For partial-readiness waits (see `Task::bytes_ready`). One pair for all tasks: waits are rare
    /// (decode outran the read) and short.
    std::mutex ready_mutex;
    std::condition_variable ready_cv;

    /// Blocks until `task->bytes_ready >= need` or the task left the Running state. Returns the
    /// task state observed last.
    Task::State waitForBytes(Task * task, size_t need);
    /// Called from the read's progress callback and at completion.
    void publishBytesReady(Task * task, size_t bytes_ready);

    void determineReadModeAndFileSize(ReadBuffer * reader_, const ReadOptions & options);
    /// Creates and starts a Task covering this request and possibly other nearby ranges.
    ///
    /// If splitting, the request is being cancelled and replaced by a smaller range
    /// (splitAndPrefetchRange), and only subrange [subrange_start, subrange_end) needs to be read.
    void pickRangesAndCreateTaskIfNotExists(RequestState *, const PrefetchHandle &, bool splitting, size_t start_offset, size_t end_offset, std::unique_lock<std::mutex> lock);
    /// Tell the source's cache how much of a read it may waste on already-cached bytes, from this
    /// reader's IO concurrency. Cheap (one atomic store); called as tasks are scheduled.
    void publishCacheReadThroughPolicy() const;

    /// Whether [offset, offset + length) is fully present in the source's in-memory cache. False
    /// unless the source is the userspace page cache (`CachedInMemoryReadBufferFromFile`). Advisory:
    /// used only to decide whether a gap is cheap to read through, never for correctness.
    bool gapIsCached(size_t offset, size_t length) const;

    /// True if a task spanning `span` bytes to serve `useful` bytes would exceed max_read_amplification.
    ///
    /// The bound is a ratio, which says nothing about how much is actually wasted: on a file whose
    /// column chunks are a few KB, five needed columns sit inside ~25 KB and the ratio looks terrible
    /// while the waste is a rounding error next to one request. Measured on a 17.5k-file Iceberg table:
    /// the ratio bound alone split each file's read three to four ways, trading 74 MiB (13% of the
    /// bytes) for 6.5k extra requests and 40% more wall time, and turning it off matched the base
    /// reader exactly. So a read whose absolute waste is below one round trip's worth of bytes is never
    /// worth splitting, whatever its ratio; above that the ratio still governs, which is what keeps a
    /// row group's worth of unrelated data out of a read that needs 50 KiB of it.
    /// The floor is `input_format_parquet_read_amplification_floor_bytes`; `0` applies the ratio to
    /// every read, which is what happens without the setting.
    bool exceedsAmplification(size_t span, size_t useful) const
    {
        if (max_read_amplification <= 0 || span <= useful)
            return false;
        if (span - useful <= read_amplification_floor_bytes)
            return false;
        return static_cast<double>(span) > max_read_amplification * static_cast<double>(useful);
    }
    static void decreaseTaskRefcount(Task * task, size_t amount);
    void scheduleTask(Task * task);
    Task::State runTask(Task * task);
    [[noreturn]] void rethrowException(Task * task);
};

/// Pins a pre-registered range that we may want to read.
/// Call reset to mark the range as no longer needed and subtract its memory usage from MemoryUsageDiff.
/// All handles must be destroyed before Prefetcher is destroyed.
class PrefetchHandle
{
public:
    PrefetchHandle() = default;
    PrefetchHandle(PrefetchHandle &&) noexcept;
    PrefetchHandle & operator=(PrefetchHandle &&) noexcept;

    /// Doesn't record deallocated memory in MemoryUsageDiff. Should only be called on shutdown,
    /// otherwise use reset(diff).
    ~PrefetchHandle();

    explicit operator bool() const { return request != nullptr; }

    void reset(MemoryUsageDiff * diff);

private:
    friend class Prefetcher;
    using RequestState = Prefetcher::RequestState;

    RequestState * request = nullptr;
    MemoryUsageToken memory;

    explicit PrefetchHandle(RequestState * request_);
};

}
