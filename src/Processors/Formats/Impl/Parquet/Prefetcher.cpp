#include <Processors/Formats/Impl/Parquet/Prefetcher.h>
#include <IO/CachedInMemoryReadBufferFromFile.h>
#include <algorithm>

#include <Formats/FormatParserSharedResources.h>
#include <IO/copyData.h>
#include <IO/SeekableReadBuffer.h>
#include <IO/WithFileSize.h>
#include <IO/WriteBufferFromVector.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>

#include <shared_mutex>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int INCORRECT_DATA;
    extern const int LOGICAL_ERROR;
}

namespace ProfileEvents
{
    extern const Event ParquetFetchWaitTimeMicroseconds;
    extern const Event ParquetPrefetcherReadRandomRead;
    extern const Event ParquetPrefetcherReadSeekAndRead;
    extern const Event ParquetPrefetcherReadEntireFile;
    extern const Event ParquetPartialReadsServed;
    extern const Event ParquetReadTasks;
    extern const Event ParquetReadTaskBytes;
    extern const Event ParquetReadFirstByteMicroseconds;
    extern const Event ParquetReadTransferMicroseconds;
}

namespace DB::Parquet
{

void Prefetcher::init(ReadBuffer * reader_, const ReadOptions & options, FormatParserSharedResourcesPtr parser_shared_resources_)
{
    min_bytes_for_seek = options.min_bytes_for_seek;
    bytes_per_read_task = options.bytes_per_read_task;
    gap_bytes = options.coalesce_gap_bytes ? std::min(min_bytes_for_seek, options.coalesce_gap_bytes) : min_bytes_for_seek;
    /// Values in (0, 1) would ask a coalesced read to span fewer bytes than the ranges it serves,
    /// which is impossible; they'd disable coalescing entirely in a way that looks like a typo for
    /// "disabled" (0). Reject them instead of silently reading every range on its own.
    if (!(options.max_read_amplification == 0 || options.max_read_amplification >= 1))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "input_format_parquet_max_read_amplification must be 0 (disabled) or >= 1, got {}", options.max_read_amplification);
    max_read_amplification = options.max_read_amplification;
    read_amplification_floor_bytes = options.read_amplification_floor_bytes;
    parser_shared_resources = parser_shared_resources_;
    determineReadModeAndFileSize(reader_, options);
    /// Only the page-cache buffer can say whether a range is already in memory; every other source
    /// leaves `cache_probe` null and the coalescer behaves as before.
    cache_probe = dynamic_cast<CachedInMemoryReadBufferFromFile *>(reader);
    range_sets.resize(1);
}

Prefetcher::~Prefetcher()
{
    shutdown->shutdown();

    /// Assert that all PrefetchHandle-s were destroyed.
    chassert(std::all_of(requests.begin(), requests.end(), [](const RequestState & req)
    {
        return req.state.load(std::memory_order_relaxed) == RequestState::State::Cancelled;
    }));
    /// Every `bytes_in_flight` increment (`scheduleTask`) must be matched by exactly one decrement
    /// (`runTask` completion or `decreaseTaskRefcount` for a task dropped while `Scheduled`); by the
    /// time all PrefetchHandle-s are gone (checked above) and `shutdown->shutdown()` has waited out
    /// any still-running tasks, none should be left in flight.
    chassert(bytes_in_flight.load(std::memory_order_relaxed) == 0);
    chassert(bytes_executing.load(std::memory_order_relaxed) == 0);
}

Prefetcher::ReadStats Prefetcher::readStats() const
{
    std::lock_guard lock(stats_mutex);
    return ReadStats{
        .rtt_us = stat_rtt_us,
        .bandwidth_bytes_per_us = stat_bandwidth_bytes_per_us,
        .bandwidth_peak_bytes_per_us = stat_bandwidth_peak_bytes_per_us,
        .samples = stat_samples};
}

size_t Prefetcher::requestLength(const PrefetchHandle & handle) const
{
    return handle ? handle.request->length : 0;
}

size_t Prefetcher::targetBytesInFlight(size_t concurrency) const
{
    ReadStats stats = readStats();
    /// Clamp the fitted bandwidth*rtt product before casting to `size_t` and multiplying by
    /// `concurrency`: a single bad sample (e.g. a near-zero transfer time producing a huge bandwidth
    /// sample) could otherwise blow up the EWMA into a value that overflows or produces a target far
    /// beyond anything sane to keep in flight.
    constexpr double max_product_bytes = 1024.0 * 1024 * 1024; // 1 GiB, per-stream ceiling
    double product = std::min(stats.bandwidth_bytes_per_us * stats.rtt_us, max_product_bytes);
    size_t target = static_cast<size_t>(product) * concurrency * 2;
    return std::max(target, 4 * bytes_per_read_task);
}

void Prefetcher::updateReadStats(const Task * task, uint64_t total_us, bool transport_progress)
{
    uint64_t first_byte_us = task->first_byte_us.load(std::memory_order_relaxed);
    /// Only trust `first_byte_us` as a genuine time-to-first-byte boundary if the transport actually
    /// reported progress mid-transfer (`transport_progress`). Otherwise the only `publishBytesReady`
    /// call for this task was the unconditional completion call at the end of `readSync` (a
    /// transport that never reports progress, or one that happened to deliver everything in a single
    /// burst) -- there's no meaningful separate transfer phase to measure in that case, and treating
    /// the near-zero gap between "first byte" and "total" as a transfer duration would produce
    /// absurd bandwidth samples (bytes / ~1 microsecond).
    uint64_t rtt_sample_us = transport_progress ? first_byte_us : total_us;
    uint64_t transfer_us = (transport_progress && total_us > first_byte_us) ? (total_us - first_byte_us) : 0;

    ProfileEvents::increment(ProfileEvents::ParquetReadFirstByteMicroseconds, rtt_sample_us);
    ProfileEvents::increment(ProfileEvents::ParquetReadTransferMicroseconds, transfer_us);

    constexpr double alpha = 0.2;
    {
        std::lock_guard lock(stats_mutex);
        stat_rtt_us = stat_rtt_us * (1 - alpha) + static_cast<double>(rtt_sample_us) * alpha;
        if (transport_progress && transfer_us > 0)
        {
            double bandwidth_sample = static_cast<double>(task->length) / static_cast<double>(transfer_us);
            stat_bandwidth_bytes_per_us = stat_bandwidth_bytes_per_us * (1 - alpha) + bandwidth_sample * alpha;
            stat_bandwidth_peak_bytes_per_us = std::max(stat_bandwidth_peak_bytes_per_us, stat_bandwidth_bytes_per_us);
        }
        ++stat_samples;
    }

}


void Prefetcher::determineReadModeAndFileSize(ReadBuffer * reader_, const ReadOptions & options)
{
    if (options.seekable_read)
    {
        bool has_file_size = isBufferWithFileSize(*reader_);
        auto * seekable = dynamic_cast<SeekableReadBuffer *>(reader_);
        if (has_file_size && seekable)
        {
            if (seekable->supportsReadAt())
            {
                reader = seekable;
                read_mode = ReadMode::RandomRead;
            }
            else if (seekable->checkIfActuallySeekable())
            {
                reader = seekable;
                read_mode = ReadMode::SeekAndRead;
            }

            if (reader)
                file_size = getFileSizeFromReadBuffer(*seekable);
        }
    }

    if (!reader)
    {
        /// Avoid loading the whole file if it's clearly not a parquet file.
        constexpr std::string_view expected_prefix = "PAR1";
        if (!reader_->eof() && reader_->available() >= expected_prefix.size() &&
            memcmp(reader_->position(), expected_prefix.data(), expected_prefix.size()) != 0)
        {
            throw Exception(ErrorCodes::INCORRECT_DATA, "Not a Parquet file (wrong magic bytes at the start)");
        }

        WriteBufferFromVector<PaddedPODArray<char>> out(entire_file);
        copyData(*reader_, out);
        out.finalize();

        read_mode = ReadMode::EntireFileIsInMemory;
        file_size = entire_file.size();
    }
}

void Prefetcher::readSync(char * to, size_t n, size_t offset, const std::function<void(size_t)> & on_progress)
{
    if (offset > file_size || n > file_size - offset)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "File read out of bounds: offset {}, length {}, file size {}", offset, n, file_size);

    size_t nread = 0;
    switch (read_mode)
    {
        case ReadMode::RandomRead:
        {
            /// `readBigAt`'s progress callback reports bytes copied so far, cumulative over the
            /// call (`ReadBufferFromS3`, `ReadWriteBufferFromHTTP`, `CachedOnDiskReadBufferFromFile`).
            /// One exception: `ReadBufferFromS3::readBigAt` restarts the count near zero on every
            /// retry attempt, so it isn't monotonic across attempts. `publishBytesReady`'s guard
            /// against non-increasing values absorbs that. Not every transport calls the callback at
            /// all (local `pread`, Azure, HDFS don't), in which case readiness equals completion.
            std::function<bool(size_t)> progress;
            if (on_progress)
            {
                /// Returning `false` means "don't stop the read" (see `copyFromIStreamWithProgressCallback`).
                progress = [&](size_t copied)
                {
                    on_progress(copied);
                    return false;
                };
            }
            nread = reader->readBigAt(to, n, offset, progress);
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadRandomRead);
            break;
        }
        case ReadMode::SeekAndRead:
        {
            std::lock_guard lock(read_mutex);
            // Seeking to a position above a previous setReadUntilPosition() confuses some of the
            // ReadBuffer implementations.
            reader->setReadUntilEnd();
            reader->seek(offset, SEEK_SET);
            reader->setReadUntilPosition(offset + n);
            nread = reader->readBig(to, n);
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadSeekAndRead);
            break;
        }
        case ReadMode::EntireFileIsInMemory:
            memcpy(to, entire_file.data() + offset, n);
            nread = n;
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadEntireFile);
            break;
    }
    if (nread != n)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Unexpected eof: offset {}, length {}, bytes read {}, expected file size {}", offset, n, nread, file_size);
    if (on_progress)
        on_progress(n);
}

PrefetchHandle Prefetcher::registerRange(size_t offset, size_t length, bool likely_to_be_used)
{
    chassert(!ranges_finalized.load(std::memory_order_relaxed));
    if (offset > file_size || length > file_size - offset)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Range out of bounds: offset {}, length {}, file size {}", offset, length, file_size);
    RequestState & req = requests.emplace_back();
    req.length = length;
    req.allow_incidental_read.store(likely_to_be_used || length < min_bytes_for_seek, std::memory_order_relaxed);
    range_sets[0].ranges.push_back(RangeState {.request = &req, .start = offset, .end = offset + length});
    return PrefetchHandle(&req);
}

void Prefetcher::finalizeRanges()
{
    bool already_finalized = ranges_finalized.exchange(true);  /// NOLINT(clang-analyzer-deadcode.DeadStores)
    chassert(!already_finalized);
    auto & ranges = range_sets[0].ranges;
    std::sort(ranges.begin(), ranges.end(), [](const RangeState & a, const RangeState & b)
        {
            return a.start < b.start;
        });
    for (size_t i = 0; i < ranges.size(); ++i)
    {
        RequestState * req = ranges[i].request;
        const auto s = req->state.load(std::memory_order_relaxed);
        if (s == RequestState::State::HasRange)
            req->range_idx = i;
        else
            chassert(s == RequestState::State::Cancelled);
    }
}

void Prefetcher::startPrefetch(const std::vector<PrefetchHandle *> & requests_to_start, MemoryUsageDiff * diff)
{
    chassert(ranges_finalized.load(std::memory_order_relaxed));

    /// Allow the requested ranges can be coalesced with each other even if they're longer than
    /// min_bytes_for_seek.
    for (const PrefetchHandle * handle : requests_to_start)
        if (*handle)
            handle->request->allow_incidental_read.store(true, std::memory_order_relaxed);

    for (PrefetchHandle * handle : requests_to_start)
    {
        if (!*handle)
            continue;
        RequestState * req = handle->request;
        chassert(req);
        pickRangesAndCreateTaskIfNotExists(req, *handle, /*splitting=*/ false, 0, 0, std::unique_lock(mutex));
        chassert(req->state.load(std::memory_order_relaxed) == RequestState::State::HasTask);
        const Task * task = req->task;

        if (!handle->memory)
        {
            size_t memory_usage = static_cast<size_t>(static_cast<double>(req->length) * task->memory_amplification);
            handle->memory = MemoryUsageToken(memory_usage, diff);
        }
    }
}

std::vector<PrefetchHandle> Prefetcher::splitRange(
    PrefetchHandle request, const std::vector<std::pair</*global_offset*/ size_t, /*length*/ size_t>> & subranges, bool likely_to_be_used)
{
    chassert(ranges_finalized.load(std::memory_order_relaxed));
    chassert(std::is_sorted(subranges.begin(), subranges.end()));
    chassert(!subranges.empty());
    chassert(!request.memory); // prefetch not requested

    RequestState * parent_req = request.request;
    std::vector<PrefetchHandle> out_handles;

    {
        std::unique_lock lock(mutex);

        /// Allocate RequestState-s.
        out_handles.reserve(subranges.size());
        for (size_t i = 0; i < subranges.size(); ++i)
            out_handles.push_back(PrefetchHandle(&requests.emplace_back()));

        if (parent_req->state.load(std::memory_order_relaxed) == RequestState::State::HasRange)
        {
            auto & ranges = range_sets[parent_req->range_set_idx].ranges;
            const auto & range = ranges.at(parent_req->range_idx);

            size_t subrange_start = UINT64_MAX;
            size_t subrange_end = 0;
            for (const auto & [start, length] : subranges)
            {
                if (start < range.start || length > range.end - start)
                    throw Exception(ErrorCodes::INCORRECT_DATA, "Subrange out of bounds: [{}, {}) not in [{}, {})", start, start + length, range.start, range.end);
                subrange_start = std::min(subrange_start, start);
                subrange_end = std::max(subrange_end, start + length);
            }

            /// If the request is already short, don't split it, and try to coalesce with other ranges.
            if (range.length() < min_bytes_for_seek)
            {
                pickRangesAndCreateTaskIfNotExists(parent_req, request, /*splitting=*/ true, subrange_start, subrange_end, std::move(lock));
            }
            else
            {
                /// Normal case: actually split the range.
                ///
                /// We put the split ranges into a new universe instead of inserting into the middle
                /// of the existing RangeSet. This allows us to use a sorted array instead of a slow
                /// tree (e.g. std::map), but introduces a limitation: ranges produced by a split can only
                /// be coalesced among each other, not with other ranges (non-split ranges or ranges from
                /// other splits). (I just guessed that this would be a better tradeoff, didn't benchmark it.)
                size_t new_range_set_idx = range_sets.size();
                auto & new_ranges = range_sets.emplace_back().ranges;
                new_ranges.reserve(subranges.size());
                for (size_t i = 0; i < subranges.size(); ++i)
                {
                    const auto [start, length] = subranges[i];
                    RequestState * req = out_handles[i].request;
                    req->state.store(RequestState::State::HasRange, std::memory_order_relaxed);
                    req->allow_incidental_read.store(likely_to_be_used || length < min_bytes_for_seek);
                    req->range_set_idx = new_range_set_idx;
                    req->range_idx = i;
                    req->length = length;

                    RangeState & r = new_ranges.emplace_back();
                    r.start = start;
                    r.end = start + length;
                    r.request = req;
                }

                request.reset(/*diff=*/ nullptr);
                return out_handles;
            }
        }
    } // unlock mutex

    chassert(parent_req->state.load(std::memory_order_relaxed) == RequestState::State::HasTask);
    Task * task = parent_req->task;
    task->refcount.fetch_add(subranges.size());

    for (size_t i = 0; i < subranges.size(); ++i)
    {
        RequestState * req = out_handles[i].request;
        req->state.store(RequestState::State::HasTask, std::memory_order_relaxed);
        req->task = task;
        req->length = subranges[i].second;
        req->task_offset = subranges[i].first - task->offset;
    }

    request.reset(/*diff=*/ nullptr);
    return out_handles;
}

void Prefetcher::pickRangesAndCreateTaskIfNotExists(RequestState * initial_req, const PrefetchHandle &, bool splitting, size_t start_offset, size_t end_offset, std::unique_lock<std::mutex> lock)
{
    chassert(lock.owns_lock());

    /// Re-check state after locking mutex.
    switch (initial_req->state.load(std::memory_order_acquire))
    {
        case RequestState::State::Cancelled: // impossible, we hold a PrefetchHandle
            chassert(false);
            break;
        case RequestState::State::HasRange:
            break;
        case RequestState::State::HasTask:
            /// Another thread created a task while we were locking the mutex.
            return;
    }
    size_t range_set_idx = initial_req->range_set_idx;
    size_t range_idx = initial_req->range_idx;
    auto & ranges = range_sets.at(range_set_idx).ranges;
    chassert(ranges.at(range_idx).request == initial_req);
    if (!splitting)
    {
        start_offset = ranges[range_idx].start;
        end_offset = ranges[range_idx].end;
    }

    /// Try to extend the task's range in both directions to cover more request ranges, as long
    /// as gaps between them are shorter than gap_bytes and the task doesn't exceed
    /// max_read_amplification.

    size_t start_idx = range_idx;
    size_t end_idx = range_idx + 1;
    size_t total_length_of_covered_ranges = end_offset - start_offset;
    /// Bytes inside the task's span that are gaps the source can serve from its in-memory cache.
    /// Reading through them costs a memcpy, not a request, so they are excluded from the read
    /// amplification the cap bounds -- otherwise a cached block sitting between two needed ranges
    /// splits the read in two and each half pays a full round trip for the uncached parts.
    size_t cached_gap_bytes = 0;

    /// Go left.
    size_t initial_offset = start_offset;
    for (size_t idx = range_idx; idx > 0; --idx)
    {
        const RangeState & r = ranges[idx - 1];
        /// The gap this merge would read through, and whether the cache already holds it.
        const size_t gap = r.end < start_offset ? start_offset - r.end : 0;
        const bool gap_cached = gap != 0 && gapIsCached(r.end, gap);
        const size_t free_bytes = cached_gap_bytes + (gap_cached ? gap : 0);
        const size_t span = std::max(end_offset, r.end) - std::min(start_offset, r.start);
        if ((r.end + gap_bytes <= start_offset && !gap_cached) || // gap too long to read through
            r.start + bytes_per_read_task <= initial_offset || // task not too big
            exceedsAmplification(span - std::min(span, free_bytes), total_length_of_covered_ranges + r.length()) ||
            !r.request->allow_incidental_read.load(std::memory_order_relaxed)) // range wants to be coalesced
            break;

        const auto s = r.request->state.load(std::memory_order_relaxed);
        if (s == RequestState::State::HasRange)
        {
            /// Include this range in the task.
            cached_gap_bytes = free_bytes;
            start_idx = idx - 1;
            total_length_of_covered_ranges += r.length();
            start_offset = std::min(start_offset, r.start);
            /// A range found to the left may extend past the current end (e.g. when ranges
            /// share the same start offset but have different lengths, and the sort placed
            /// the longer range first). We must extend end_offset to cover it.
            end_offset = std::max(end_offset, r.end);
        }
        else if (s != RequestState::State::Cancelled)
        {
            /// Range already has a task. No need to scan further, the other task already did that.
            chassert(s == RequestState::State::HasTask);
            break;
        }
        else
        {
            /// Keep going past a cancelled range, but don't update start_idx/start_offset until we
            /// hit a non-cancelled range.
        }
    }

    /// Go right.
    initial_offset = end_offset;
    for (size_t idx = range_idx + 1; idx < ranges.size(); ++idx)
    {
        const RangeState & r = ranges[end_idx];
        const size_t gap = end_offset < r.start ? r.start - end_offset : 0;
        const bool gap_cached = gap != 0 && gapIsCached(end_offset, gap);
        const size_t free_bytes = cached_gap_bytes + (gap_cached ? gap : 0);
        const size_t span = std::max(end_offset, r.end) - std::min(start_offset, r.start);
        if ((end_offset + gap_bytes <= r.start && !gap_cached) ||
            initial_offset + bytes_per_read_task <= r.end ||
            exceedsAmplification(span - std::min(span, free_bytes), total_length_of_covered_ranges + r.length()) ||
            !r.request->allow_incidental_read.load(std::memory_order_relaxed))
            break;

        const auto s = r.request->state.load(std::memory_order_relaxed);
        if (s == RequestState::State::HasRange)
        {
            cached_gap_bytes = free_bytes;
            end_idx = idx + 1;
            total_length_of_covered_ranges += r.length();
            end_offset = std::max(end_offset, r.end);
            /// (This currently doesn't do anything because ranges are sorted by `start`, but why not.)
            start_offset = std::min(start_offset, r.start);
        }
        else if (s != RequestState::State::Cancelled)
        {
            chassert(s == RequestState::State::HasTask);
            break;
        }
    }

    /// Create task.
    Task & task = tasks.emplace_back();
    task.owner = this;
    task.offset = start_offset;
    task.length = end_offset - task.offset;
    ProfileEvents::increment(ProfileEvents::ParquetReadTasks);
    ProfileEvents::increment(ProfileEvents::ParquetReadTaskBytes, task.length);
    task.memory_amplification = 1. * static_cast<double>(task.length) / static_cast<double>(total_length_of_covered_ranges);
    size_t initial_refcount = end_idx - start_idx + 1;
    task.refcount.store(initial_refcount);

    size_t actual_refcount = 0;
    for (size_t idx = start_idx; idx < end_idx; ++idx)
    {
        const RangeState & range = ranges[idx];
        RequestState * req = range.request;
        req->task = &task;
        req->task_offset = range.start - task.offset;

        RequestState::State s = RequestState::State::HasRange;
        if (req->state.compare_exchange_strong(s, RequestState::State::HasTask))
            actual_refcount += 1;
        else
            chassert(s == RequestState::State::Cancelled);
    }

    chassert(actual_refcount > 0);
    decreaseTaskRefcount(&task, initial_refcount - actual_refcount);

    lock.unlock();

    scheduleTask(&task);
}

void Prefetcher::decreaseTaskRefcount(Task * task, size_t amount)
{
    size_t c = task->refcount.fetch_sub(amount, std::memory_order_acq_rel);
    chassert(c >= amount);
    if (c != amount)
        return;

    Task::State prev_state = task->state.exchange(Task::State::Deallocated);
    if (prev_state != Task::State::Running)
    {
        task->buf = {};
        task->cached_region.reset();
    }

    /// If the task was still `Scheduled`, it was dropped before any thread got to run it (the
    /// `Scheduled` -> `Running` CAS at the top of `runTask` will now fail and return early without
    /// touching `bytes_in_flight`): `scheduleTask` already added its bytes, and nobody else will
    /// subtract them, so we must do it here. If the previous state was `Running`, `runTask` is still
    /// executing (or about to) and will subtract them itself when it finishes; if it was `Done` or
    /// `Exception`, `runTask` already did.
    if (prev_state == Task::State::Scheduled)
        task->owner->bytes_in_flight.fetch_sub(task->length, std::memory_order_relaxed);

    /// This path only runs when no `PrefetchHandle` references the task any more, so nobody can be
    /// blocked in `waitForBytes` for it.
    chassert(task->waiters.load() == 0);
}

void Prefetcher::publishBytesReady(Task * task, size_t bytes_ready)
{
    /// Only ever called on the one thread executing `runTask` for this task (the `Scheduled` ->
    /// `Running` CAS and the progress callback both run there), so this read-modify-write of
    /// `bytes_ready` doesn't race with another writer.
    ///
    /// `bytes_ready` is cumulative over the `readBigAt` call, but isn't necessarily increasing from
    /// call to call: `ReadBufferFromS3` restarts the count near zero on every retry attempt inside
    /// `readBigAt` (see `readSync`). This guard against non-increasing values is what makes that safe.
    size_t prev = task->bytes_ready.load(std::memory_order_relaxed);
    if (bytes_ready <= prev)
        return;
    if (prev == 0)
        /// `max(1, ...)`: keep 0 available as an unambiguous "never set" sentinel even if the clock
        /// reads back an elapsed time of 0 (sub-microsecond first byte).
        task->first_byte_us.store(std::max<uint64_t>(1, task->stopwatch.elapsedMicroseconds()), std::memory_order_relaxed);
    /// The store here and the `waiters` load below must not be reordered with each other (nor with
    /// the paired increment-then-load in `waitForBytes`), or a waiter could go to sleep just after
    /// we've already published enough bytes and just before it increments `waiters`, and never get
    /// woken by this call. Plain acquire/release on two different locations isn't enough to prevent
    /// that (a `store-release` here and a `load-acquire` there can still each be reordered ahead of
    /// an unrelated atomic on the same thread); `seq_cst` on both sides is. Worst case if we get it
    /// wrong is a missed wakeup, not corruption: the waiter still wakes up (late) from the
    /// unconditional `notify_all` when the task leaves `Running` in `runTask`.
    task->bytes_ready.store(bytes_ready, std::memory_order_seq_cst);
    if (task->waiters.load(std::memory_order_seq_cst) != 0)
    {
        std::lock_guard lock(ready_mutex);
        ready_cv.notify_all();
    }
}

Prefetcher::Task::State Prefetcher::waitForBytes(Task * task, size_t need)
{
    std::unique_lock lock(ready_mutex);
    /// See the seq_cst comment in `publishBytesReady`: the increment here and the `bytes_ready` load
    /// below must not be reordered with each other, or with the store-then-load pair there.
    task->waiters.fetch_add(1, std::memory_order_seq_cst);
    Task::State s;
    while (true)
    {
        s = task->state.load(std::memory_order_acquire);
        if (s != Task::State::Running)
            break;
        if (task->bytes_ready.load(std::memory_order_seq_cst) >= need)
            break;
        ready_cv.wait(lock);
    }
    task->waiters.fetch_sub(1, std::memory_order_seq_cst);
    return s;
}

void Prefetcher::publishCacheReadThroughPolicy() const
{
    if (!cache_probe)
        return;

    /// Reading through an island of cached blocks buys one fewer round trip and costs the island's
    /// bytes. Which side wins is not a property of the storage but of how this reader is using it:
    /// with few reads in flight the pipe is idle and round trips are the wall clock, so bytes are
    /// nearly free; with many reads in flight the link is the constraint, latency is already hidden by
    /// the concurrency, and every extra byte displaces a useful one. The IO pool size is the reader's
    /// own measure of that, and the cache cannot see it -- hence this hand-off.
    ///
    /// The measured shape on S3 (see the read-path report): read-through is a large win at 4 and 16
    /// concurrent reads and a loss at 64, which `4 / concurrency` tracks -- full allowance up to 4,
    /// a quarter at 16, a sixteenth at 64. Clamped to a floor so a very deep pool still merges an
    /// island that is trivially small next to the request.
    const size_t concurrency = std::max<size_t>(1, parser_shared_resources
        ? parser_shared_resources->io_threads.load(std::memory_order_relaxed) : 1);
    constexpr double latency_bound_concurrency = 4.0;
    const double share = std::clamp(latency_bound_concurrency / static_cast<double>(concurrency), 0.05, 1.0);
    cache_probe->setReadThroughWastePermille(static_cast<size_t>(share * 1000));
}

bool Prefetcher::gapIsCached(size_t offset, size_t length) const
{
    /// `readBigAt`-family only, and only worth probing for gaps a task could actually absorb: the
    /// probe is a hash lookup per cache block, so bounding it by `bytes_per_read_task` keeps the cost
    /// proportional to the read it may enable.
    if (!cache_probe || read_mode != ReadMode::RandomRead || length == 0 || length > bytes_per_read_task)
        return false;
    return cache_probe->isBigRangeCached(offset, length);
}

void Prefetcher::scheduleTask(Task * task)
{
    publishCacheReadThroughPolicy();

    /// The calling thread (pickRangesAndCreateTaskIfNotExists) still holds, via the `PrefetchHandle`
    /// it was passed, a reference that keeps `refcount` >= 1 until that handle is later reset by its
    /// owner -- which can't happen before this call returns, since the owner is the caller further
    /// up the same call stack. So `refcount` can't have dropped to zero and raced ahead of the
    /// `fetch_add` below via `decreaseTaskRefcount` in this window between the lock being released
    /// and `scheduleTask` running.
    chassert(task->refcount.load(std::memory_order_relaxed) > 0);

    /// Matched by exactly one `fetch_sub`: either at the end of `runTask` (this task is guaranteed
    /// to reach `runTask` exactly once, whether scheduled onto `io_runner` here or run synchronously
    /// from `getRangeData`), or in `decreaseTaskRefcount` if the task is dropped before that happens.
    bytes_in_flight.fetch_add(task->length, std::memory_order_relaxed);

    if (parser_shared_resources && !parser_shared_resources->io_runner.isDisabled())
        parser_shared_resources->io_runner([this, task, _shutdown = shutdown]
            {
                std::shared_lock shutdown_lock(*_shutdown, std::try_to_lock);
                if (!shutdown_lock.owns_lock())
                    return;
                runTask(task);
            });
}

std::span<const char> Prefetcher::getRangeData(const PrefetchHandle & request)
{
    const RequestState * req = request.request;
    chassert(req->state == RequestState::State::HasTask);
    Task * task = req->task;
    Task::State s = task->state.load(std::memory_order_acquire);
    const size_t need = req->task_offset + req->length;
    if (s == Task::State::Scheduled || s == Task::State::Running)
    {
        Stopwatch wait_time;

        if (s == Task::State::Scheduled)
        {
            s = runTask(task);
            chassert(s != Task::State::Scheduled);
        }

        if (s == Task::State::Running) // (not `else`, the runTask above may return Running)
        {
            s = waitForBytes(task, need);
            if (s == Task::State::Running)
                ProfileEvents::increment(ProfileEvents::ParquetPartialReadsServed);
        }

        ProfileEvents::increment(ProfileEvents::ParquetFetchWaitTimeMicroseconds, wait_time.elapsedMicroseconds());
    }
    if (s == Task::State::Exception)
        rethrowException(task);
    chassert(s == Task::State::Done || (s == Task::State::Running && task->bytes_ready.load(std::memory_order_acquire) >= need));

    if (s == Task::State::Done && task->cached_region.has_value())
    {
        /// Zero-copy path: serve data directly from cache cells.
        size_t req_file_offset = task->offset + req->task_offset;

        const auto & r = task->cached_region.value();
        chassert(r.file_offset <= req_file_offset);
        chassert(r.file_offset + r.size >= req_file_offset + req->length);

        size_t offset_in_region = req_file_offset - r.file_offset;

        return std::span(r.data + offset_in_region, req->length);
    }

    chassert(req->task_offset + req->length <= task->buf.size());
    return std::span(task->buf.data() + req->task_offset, req->length);
}

Prefetcher::Task::State Prefetcher::runTask(Task * task)
{
    auto s = Task::State::Scheduled;
    if (!task->state.compare_exchange_strong(s, Task::State::Running))
        return s;

    /// From here on this task occupies a reader thread, so its bytes are what the storage is actually
    /// working on -- the quantity the read-ahead target is about. Matched by the `fetch_sub` at the
    /// end of this function; the CAS above succeeds for exactly one caller, so the pair is exact, and
    /// a task dropped while still `Scheduled` never touches this counter.
    bytes_executing.fetch_add(task->length, std::memory_order_relaxed);

    task->stopwatch.restart();

    auto final_state = Task::State::Done;
    /// Set in the `supportsReadAtRetainCells` branch below (both the single-cell zero-copy case and
    /// the multi-cell memcpy-assembled case): a task served from the cache has no meaningful
    /// round-trip-time/bandwidth to measure (no wire transfer, or a fast local memcpy that would
    /// otherwise pollute the fitted stats), so it's excluded from `updateReadStats` regardless of
    /// whether it happened to land in one cell (no `cached_region`-based exclusion needed) or many.
    bool served_from_cache = false;
    /// Set from the buffered (`readSync`) branch's progress callback: true only if the transport
    /// invoked it with a partial count at least once. Distinguishes a genuine mid-transfer progress
    /// report from the single synthetic completion call that `readSync` always makes at the end (see
    /// its call to `on_progress(n)`), which transports that never report progress (local `pread`,
    /// Azure, HDFS) rely on as their only callback. Without this, that synthetic call could look like
    /// "first byte arrived a few nanoseconds before completion", turning a normal read into a
    /// bandwidth sample of `length / ~1 microsecond`.
    bool transport_progress = false;
    try
    {
        /// When the reader supports zero-copy cached reads, get retained cache cells
        /// instead of allocating a buffer and copying data into it.
        if (read_mode == ReadMode::RandomRead && reader->supportsReadAtRetainCells() && task->length > 0)
        {
            served_from_cache = true;
            auto cached_regions = reader->readBigAtRetainCells(task->length, task->offset);
            chassert(!cached_regions.empty());

            if (cached_regions.size() == 1)
            {
                /// We got lucky and the Task's range is all in one cache cell. Zero-copy it.
                auto & cr = cached_regions[0];
                task->cached_region = Task::CachedReadRegion{
                    .handle = std::move(cr.handle),
                    .data = cr.data,
                    .size = cr.size,
                    .file_offset = cr.file_offset,
                };
            }
            else
            {
                /// If the data spans multiple cache blocks, pre-assemble it into task->buf now
                /// (on the single-threaded producer side) to avoid a data race in getRangeData,
                /// where multiple consumer threads could try to lazily populate task->buf concurrently.
                if (cached_regions.size() > 1)
                {
                    task->buf.resize(task->length);
                    size_t copied = 0;
                    for (const auto & region : cached_regions)
                    {
                        memcpy(task->buf.data() + copied, region.data, region.size);
                        copied += region.size;
                    }
                    chassert(copied == task->length);
                }
            }

            /// Only publish for the buffered (multi-cell) case: `task->buf` holds real data there, so a
            /// waiter reading `task->buf.data() + task_offset` right after seeing `bytes_ready` is
            /// safe. For the single-cell zero-copy case `task->buf` is never filled (`cached_region`
            /// is used instead), and `getRangeData` only reads from `cached_region` once `state` is
            /// `Done` -- so publishing readiness here, before the state CAS below, would let a waiter
            /// observe `Running` with enough `bytes_ready` and fall through to the (empty) `buf`
            /// return. The unconditional `notify_all` after the state CAS still wakes waiters on the
            /// zero-copy path once the task is actually `Done`.
            if (!task->cached_region.has_value())
                publishBytesReady(task, task->length);
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadRandomRead);
        }
        else
        {
            task->buf.resize(task->length);
            readSync(task->buf.data(), task->length, task->offset,
                [this, task, &transport_progress](size_t copied)
                {
                    if (copied < task->length)
                        transport_progress = true;
                    publishBytesReady(task, copied);
                });
        }
    }
    catch (...)
    {
        final_state = Task::State::Exception;
        std::lock_guard lock(exception_mutex);
        task->exception = std::current_exception();
    }

    uint64_t total_us = task->stopwatch.elapsedMicroseconds();

    /// Matches the `fetch_add` in `scheduleTask`. Exactly one of {here, `decreaseTaskRefcount`}
    /// subtracts this task's bytes, since we only get here once (the CAS above succeeds for exactly
    /// one caller) and the early return above (CAS failed) skips this.
    bytes_in_flight.fetch_sub(task->length, std::memory_order_relaxed);
    /// Matches the `fetch_add` after the `Scheduled` -> `Running` CAS above.
    bytes_executing.fetch_sub(task->length, std::memory_order_relaxed);

    /// Fold this task's timing into the fitted round-trip-time/bandwidth stats, but only for reads
    /// that actually went over the wire and can be timed meaningfully: not on exception, only for
    /// `RandomRead` (the mode `readBigAt`'s progress callback is wired up for), and excluding tasks
    /// served from the cache (`served_from_cache`, set above -- no wire transfer to time, or a fast
    /// local memcpy that isn't representative of the source's bandwidth). Computed now (`bytes_ready`
    /// isn't touched by the state CAS or the deallocation below) but the update itself is deferred
    /// past that CAS and the `notify_all` so waiters aren't held up by the stats mutex or the
    /// profile-event increments.
    const bool should_update_stats = final_state != Task::State::Exception && read_mode == ReadMode::RandomRead
        && !served_from_cache && task->bytes_ready.load(std::memory_order_relaxed) > 0;

    s = Task::State::Running;
    if (task->state.compare_exchange_strong(s, final_state))
    {
        s = final_state;
    }
    else
    {
        chassert(s == Task::State::Deallocated);
        task->buf = {};
        task->cached_region.reset();
    }

    {
        /// Wake partial waiters too: the task is Done, Exception or Deallocated now.
        std::lock_guard lock(ready_mutex);
        ready_cv.notify_all();
    }

    task->completion.notify();

    if (should_update_stats)
        updateReadStats(task, total_us, transport_progress);

    return s;
}

void Prefetcher::rethrowException(Task * task)
{
    std::lock_guard lock(exception_mutex);
    /// Each waiter gets a private copy so callers can safely mutate it (addMessage())
    std::rethrow_exception(copyMutableException(task->exception));
}

PrefetchHandle::PrefetchHandle(RequestState * request_) : request(request_) {}

PrefetchHandle::PrefetchHandle(PrefetchHandle && rhs) noexcept
{
    *this = std::move(rhs);
}

PrefetchHandle & PrefetchHandle::operator=(PrefetchHandle && rhs) noexcept
{
    // Shouldn't assign to nonempty handles because deallocation wouldn't be recorded in MemoryUsageDiff.
    chassert(!memory);

    reset(nullptr);
    request = std::exchange(rhs.request, nullptr);
    return *this;
}

PrefetchHandle::~PrefetchHandle()
{
    reset(nullptr);
}

void PrefetchHandle::reset(MemoryUsageDiff * diff)
{
    if (!request)
        return;

    if (diff)
        memory.reset(diff);

    if (request->state.exchange(RequestState::State::Cancelled) == RequestState::State::HasTask)
        Prefetcher::decreaseTaskRefcount(request->task, 1);

    request = nullptr;
}

}
