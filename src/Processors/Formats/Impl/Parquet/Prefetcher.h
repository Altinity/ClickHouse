#pragma once

#include <Common/PODArray.h>
#include <Processors/Formats/Impl/Parquet/ReadCommon.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <span>

namespace DB
{
class ReadBuffer;
class SeekableReadBuffer;
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

    /// Pass-through read from the underlying ReadBuffer. When `notify_first_byte` is set (only on the
    /// primary read path), notifies its `first_byte` when the underlying read reports its first bytes,
    /// so TTFB-triggered hedging can tell a stalled connection from a slow-but-progressing transfer.
    void readSync(char * to, size_t n, size_t offset, Task * notify_first_byte = nullptr);

    /// Read several (to, n, offset) ranges concurrently: submits all but the first to io_runner and
    /// runs the first inline, then waits for the rest. Falls back to sequential readSync when there's
    /// no thread pool or a single range. The first exception (if any) is rethrown.
    /// DEADLOCK SAFETY: must be called from a NON-io_runner thread (footer read at open, or the
    /// inline consumer path in getRangeData). Calling it from a scheduled io_runner task could
    /// deadlock (a pool thread waiting on pool tasks) - see ThreadPoolCallbackRunnerFast notes.
    void readSyncParallel(const std::vector<std::tuple<char *, size_t, size_t>> & reads);

    /// Whether split_reads_across_boundaries is enabled (used by the footer read).
    bool splitReadsEnabled() const { return split_reads_across_boundaries; }

    /// Retain a tail chunk of the file (the bytes already read to parse the footer) so that any
    /// range subsequently registered and fully contained in it - notably the Column Index and
    /// Offset Index, which live just below the FileMetaData footer - is served from this in-memory
    /// copy instead of issuing another read. No-op for EntireFileIsInMemory (nothing to save) and
    /// when the chunk is empty. `data` points at `length` bytes representing file offsets
    /// [file_offset, file_offset + length).
    void retainTail(const char * data, size_t length, size_t file_offset);

    size_t getFileSize() const { return file_size; }

    /// Average completed-read throughput (bytes/sec) since init, or 0 if not enough has been read to
    /// estimate. Used for read back-pressure (input_format_parquet_prefetch_bandwidth_hide_seconds).
    double averageThroughputBytesPerSec() const;

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
        /// Notified when the state changes from Running to Done or Exception (by the primary read, or
        /// by a hedge read - whichever finishes first).
        CompletionNotification completion;
        /// Notified when the primary read receives its first byte (from the readBigAt progress
        /// callback), and unconditionally when the primary read finishes - so a waiter is always
        /// released even if the read produced no progress callback. Used by TTFB-triggered hedging
        /// (hedged_read_ttfb_threshold_ms): a hedge fires only if no first byte arrives in time.
        CompletionNotification first_byte;
        std::exception_ptr exception;

        /// Hedging (tail-latency mitigation): a second read of the same range, raced against the
        /// primary. Whichever finishes first CAS-wins `hedge_winner` (1 = primary, 2 = hedge) and
        /// notifies `completion`; getRangeData then returns the winner's buffer. `hedge_buf` holds
        /// the hedge read; `hedge_started` guards launching it at most once.
        PaddedPODArray<char> hedge_buf;
        std::atomic<int> hedge_winner {0};
        std::atomic<bool> hedge_started {false};
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
    PaddedPODArray<char> entire_file;

    size_t file_size{};
    size_t min_bytes_for_seek{};
    size_t bytes_per_read_task{};

    /// Cumulative start offsets of the object's S3 multipart-upload parts, used to keep coalesced
    /// read tasks within a single part. Empty = no alignment. Set once in init(), read-only after.
    /// Takes precedence over read_alignment_stride when non-empty.
    std::vector<size_t> multipart_part_offsets;

    /// Fixed boundary grid (bytes) used for read alignment when multipart_part_offsets is empty.
    /// 0 = off. And the anti-fragmentation min aligned-segment size. Set in init(), read-only after.
    size_t read_alignment_stride = 0;
    size_t read_alignment_min_bytes = 0;

    /// Split straddling reads into per-boundary segments read in parallel (see ReadOptions). Set in
    /// init(), read-only after.
    bool split_reads_across_boundaries = false;

    /// Anti-amplification: minimum "wanted / span" fraction for a coalesced task (see ReadOptions).
    /// 0 = off. Set in init(), read-only after.
    double read_min_fill_ratio = 0;

    /// Hedged reads (tail-latency mitigation). Set in init(), read-only after. hedges_inflight is the
    /// live count, bounded by hedged_read_max_inflight.
    size_t hedged_read_threshold_ms = 0;
    size_t hedged_read_ttfb_threshold_ms = 0;
    size_t hedged_read_max_bytes = 0;
    size_t hedged_read_max_inflight = 0;
    std::atomic<size_t> hedges_inflight {0};

    /// Tail chunk retained by retainTail() to serve fully-contained ranges (Column/Offset Index)
    /// without a second read. Written once before any prefetching, read-only afterwards.
    /// [retained_tail_start, retained_tail_end) are file offsets; empty range == nothing retained.
    PaddedPODArray<char> retained_tail;
    size_t retained_tail_start = 0;
    size_t retained_tail_end = 0;

    /// Total bytes read by completed tasks, and when reading started, for throughput estimation.
    std::atomic<size_t> total_bytes_read{0};
    std::chrono::steady_clock::time_point read_start_time{};

    std::shared_ptr<ShutdownHelper> shutdown = std::make_shared<ShutdownHelper>();

    /// Locked when creating a Task.
    std::mutex mutex;

    /// Arenas.
    std::deque<RequestState> requests;
    std::deque<Task> tasks;
    std::deque<RangeSet> range_sets;

    std::atomic<bool> ranges_finalized {false};

    /// (One mutex for all tasks because it's not used frequently.)
    std::mutex exception_mutex;

    void determineReadModeAndFileSize(ReadBuffer * reader_, const ReadOptions & options);
    /// Creates and starts a Task covering this request and possibly other nearby ranges.
    ///
    /// If splitting, the request is being cancelled and replaced by a smaller range
    /// (splitAndPrefetchRange), and only subrange [subrange_start, subrange_end) needs to be read.
    void pickRangesAndCreateTaskIfNotExists(RequestState *, const PrefetchHandle &, bool splitting, size_t start_offset, size_t end_offset, std::unique_lock<std::mutex> lock);
    static void decreaseTaskRefcount(Task * task, size_t amount);
    void scheduleTask(Task * task);
    /// allow_split: whether a boundary-straddling read may be split into parallel segments (segments
    /// run on getIOThreadPool, a separate pool - deadlock-safe from any caller). Set true ONLY on the
    /// inline getRangeData path, where a consumer is actively blocked (prefetch didn't cover this read
    /// in time - i.e. ramp-up / prefetch behind). Prefetched (scheduled) reads pass false: no one is
    /// waiting on them, so splitting would only add GETs. This makes splitting self-limiting - it
    /// happens early / when starved and stops once prefetch keeps the pipeline full.
    Task::State runTask(Task * task, bool allow_split = false);
    /// File-offset split points strictly inside (offset, offset+length), from multipart_part_offsets
    /// (preferred) or read_alignment_stride. Empty = the range fits within one part / no boundaries.
    std::vector<size_t> splitPointsForRange(size_t offset, size_t length) const;
    /// Whether the byte range [lo, hi) contains a part boundary strictly inside - i.e. reading it
    /// would straddle a part. Used to stop coalescing from bridging a gap across a boundary.
    bool gapCrossesBoundary(size_t lo, size_t hi) const;
    /// Anti-amplification: whether extending a task (currently covering [start_offset, end_offset) with
    /// `wanted` bytes of actual ranges) to include range r would drop the task's wanted/span fraction
    /// below read_min_fill_ratio (only for gaps above a small floor). Used to stop coalescing from
    /// amplifying a tiny scattered column into a huge mostly-filler read.
    bool fillRatioWouldBreak(size_t wanted, size_t start_offset, size_t end_offset, const RangeState & r) const;
    [[noreturn]] void rethrowException(Task * task);

    /// Issue a duplicate (hedged) read of `task`'s range when the primary is slow, to cut tail
    /// latency. Phase A: the read runs synchronously on the calling (already-blocked) consumer
    /// thread into task->hedge_buf; on success sets hedge_winner=2 and notifies so sharers wake.
    /// Returns true if a usable hedge result was produced. At most once per task (hedge_started
    /// guard), bounded by hedged_read_max_inflight, and only for real (non-cached) remote reads.
    /// NOTE: a fully async race (Phase B) would need decreaseTaskRefcount to defer freeing
    /// hedge_buf until an in-flight hedge finishes (the refcount/Deallocated path assumes one
    /// reader) - deferred to avoid a use-after-free on hedge_buf.
    bool hedgeReadSync(Task * task);
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
