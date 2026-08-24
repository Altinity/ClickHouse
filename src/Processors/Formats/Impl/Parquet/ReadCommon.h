#pragma once

#include <Common/threadPoolCallbackRunner.h>
#include <Formats/FormatSettings.h>

#include <algorithm>
#include <vector>

namespace DB
{
struct FormatParserSharedResources;
using FormatParserSharedResourcesPtr = std::shared_ptr<FormatParserSharedResources>;
struct FormatFilterInfo;
using FormatFilterInfoPtr = std::shared_ptr<FormatFilterInfo>;
}

namespace DB::Parquet
{

struct ReadOptions
{
    FormatSettings format;

    bool seekable_read = true;

    bool schema_inference_force_nullable = false;
    bool schema_inference_force_not_nullable = false;

    /// Not implemented.
    /// Use dictionary filter if dictionary page is smaller than this (and all values in the column
    /// chunk are dictionary-encoded). This takes precedence over bloom filter. 0 to disable.
    size_t dictionary_filter_limit_bytes = 0;

    size_t min_bytes_for_seek = 64 << 10;
    size_t bytes_per_read_task = 4 << 20;

    /// Don't use bloom filter for `x IN (...)` if the set `(...)` is has more than this many
    /// elements. There's no point using bloom filter for big sets because false positive
    /// probability becomes very high. E.g. if bloom filter has 1% false positive probability,
    /// searching for 100 elements would have 63% false positive probability.
    size_t bloom_filter_max_set_size = 100;

    /// Hint (in bytes) for how much of the file tail to read when fetching the parquet footer
    /// (FileMetaData). 0 = unknown -> use the default speculative 64 KiB read. A data lake that
    /// already knows per-file stats (e.g. Iceberg: column count, row-group count from split_offsets,
    /// and per-column bound sizes) can set this via estimateParquetFooterSize so the footer is
    /// captured in a single read for wide / many-row-group files. Only affects read count, never
    /// correctness: an undershoot falls back to a second read, an overshoot reads a slightly larger
    /// (already-clamped) tail.
    size_t footer_metadata_size_hint = 0;

    /// Cumulative start offsets of the object's S3 multipart-upload parts (part i covers
    /// [multipart_part_offsets[i], multipart_part_offsets[i+1])). Learned from GetObjectAttributes
    /// (see ObjectStorageIdentityCache) and used to align coalesced read tasks to part boundaries so
    /// a single read never straddles two parts. Empty = unknown / single-part -> no alignment.
    /// EXPERIMENTAL: used to measure the effect of part-boundary-aligned reads. Takes precedence over
    /// read_alignment_stride when non-empty.
    std::vector<size_t> multipart_part_offsets;

    /// Fixed boundary grid (bytes) for read alignment when the real per-file part layout is unknown:
    /// no coalesced read straddles a multiple of this. 0 = off. EXPERIMENTAL.
    size_t read_alignment_stride = 0;

    /// Anti-fragmentation guard: don't cut a read at an alignment boundary if the resulting aligned
    /// segment would be smaller than this many bytes (allow the straddle instead of a tiny read).
    size_t read_alignment_min_bytes = 0;

    /// Split a single read that straddles a part boundary into per-part segments read in parallel,
    /// instead of one straddling GET. Measured on same-region AWS S3: a boundary-crossing ranged GET
    /// pays ~one extra RTT mid-stream, so two aligned reads fetched concurrently are ~2.5x faster.
    /// Boundaries come from multipart_part_offsets (preferred) or read_alignment_stride. Also enables
    /// the speculative-parallel footer read (last 2 MiB + the rest, fired concurrently). 0 = off.
    /// EXPERIMENTAL. Only helps latency-bound / critical-path reads; hidden by high concurrency.
    bool split_reads_across_boundaries = false;

    /// Anti-amplification guard for coalescing: don't bridge a gap between two wanted ranges if the
    /// resulting read task would be less than this fraction "wanted" (i.e. mostly filler bytes).
    /// Coalescing normally reads through gaps shorter than min_bytes_for_seek to save a seek, but many
    /// such sub-threshold gaps can accumulate into a task that is almost entirely unwanted bytes -
    /// e.g. reading a tiny, scattered (RLE/near-constant) column drags in the neighbouring columns and
    /// amplifies a few KB into hundreds of MB. This caps that: a task can never be more than
    /// 1/read_min_fill_ratio times its wanted bytes. 0 = off (pure gap-size coalescing). Gaps below a
    /// small absolute floor are always bridged regardless, so dense reads are unaffected. EXPERIMENTAL.
    double read_min_fill_ratio = 0;

    /// Hedged reads (tail-latency mitigation): if a read a consumer is blocked on hasn't completed
    /// within hedged_read_threshold_ms, issue a duplicate read and take whichever returns first.
    /// 0 = off. Only reads no larger than hedged_read_max_bytes are hedged (latency, not throughput),
    /// and at most hedged_read_max_inflight hedges run concurrently (cost cap). EXPERIMENTAL.
    size_t hedged_read_threshold_ms = 0;
    size_t hedged_read_ttfb_threshold_ms = 0;
    size_t hedged_read_max_bytes = 0;
    size_t hedged_read_max_inflight = 0;
};

/// Estimate the serialized size of a parquet FileMetaData footer, to size the initial tail read.
///  - num_columns: number of leaf columns,
///  - num_row_groups: number of row groups,
///  - bounds_bytes: total bytes of the per-column min+max bounds for one file (e.g. summed from an
///    Iceberg manifest's lower_bounds + upper_bounds); these repeat per row group in the footer.
/// The result is clamped to a sane speculative-read range, so it is always safe to use directly as
/// ReadOptions::footer_metadata_size_hint.
inline size_t estimateParquetFooterSize(size_t num_columns, size_t num_row_groups, size_t bounds_bytes)
{
    /// Rough per-structure sizes of thrift-compact FileMetaData (see the constants' rationale in the
    /// design notes). Overestimating only costs a marginally larger tail read; underestimating just
    /// triggers the reader's existing second-read fallback.
    constexpr size_t fixed_overhead = 4096;      /// schema + file-level key/value metadata
    constexpr size_t per_row_group = 64;         /// RowGroupMetaData wrapper
    /// Thrift-compact ColumnMetaData per chunk (offsets, sizes, encodings, short path). Measured
    /// ~51 B/chunk on a 437-column / 8-row-group / 1 GiB file (large offsets -> ~5 B varints);
    /// 64 leaves headroom for longer column names. The old 112 overshot the whole footer ~2.2x
    /// because this term dominates wide/few-stats footers (num_columns * this * num_row_groups).
    constexpr size_t per_column_chunk = 64;      /// ColumnMetaData fixed fields (offsets, sizes, ...)
    constexpr size_t floor_size = 64ul << 10;    /// never smaller than the default speculative read
    constexpr size_t cap_size = 16ul << 20;      /// don't speculatively read an enormous tail

    size_t est = fixed_overhead
        + num_row_groups * (per_row_group + num_columns * per_column_chunk + bounds_bytes);
    est += est / 4; /// ~1.25x safety for column names and Iceberg-vs-parquet truncation-length skew
    return std::clamp(est, floor_size, cap_size);
}

struct SharedResourcesExt
{
    size_t total_memory_low_watermark = 0;
    size_t total_memory_high_watermark = 0;

    struct Limits
    {
        size_t memory_low_watermark;
        size_t memory_high_watermark;
        size_t parsing_threads;
    };

    static Limits getLimitsPerReader(const FormatParserSharedResources & parser_shared_resources, double memory_fraction, double thread_fraction);
};


/// Each column chunk goes through some subsequence of these stages, in order.
///
/// The scheduling of all this work (in ReadManager) is pretty complicated.
/// Some of the tasks apply to column chunk (e.g. reading bloom filter), some apply to part of
/// a column chunk ("column subchunk" we call it). Some stages need some per-row-group work after
/// finishing all per-column tasks (e.g. apply KeyCondition after reading bloom filters for all
/// columns).
///
/// Here's a slightly simplified dependency graph:
/// https://github.com/ClickHouse/ClickHouse/pull/82789#discussion_r2292203372
/// (if you need to edit this diagram, load this into excalidraw:
///  https://pastila.nl/?cafebabe/5f32c6546f4797c537707535c515f2c3#Fp02Ps7p2hRahC0B5cK+TQ== )
///
/// An important role of this enum is to separately control parallelism of different stages.
/// E.g. typically column index is small, and we can read it in lots of columns and row groups
/// in parallel (especially useful if we're reading over network and are latency-bound).
/// But main data is often big enough that we can't afford enough memory to read many row groups in
/// parallel. We'd like the parallelism to automatically scale based on memory usage.
/// But also we don't want to get into a situation where e.g. most of the memory budget is used by
/// column indexes and there's not enough left to read main data for a few row groups in parallel.
/// To solve these two problems at once, we do memory accounting separately for each stage, with
/// separate memory budget for each stage (see ReadManager::Stage).
/// Memory is attributed to the stage that allocated it. E.g. ReadManager::read() (Deliver stage)
/// may release a column that was allocated by PrewhereData stage, reducing PrewhereData's memory
/// usage and potentially kicking off more PrewhereData read tasks.
enum class ReadStage
{
    NotStarted = 0,

    BloomFilterHeader,
    BloomFilterBlocksOrDictionary,
    ColumnIndexAndOffsetIndex,

    OffsetIndex,
    /// Issues the compressed data-page reads (startPrefetch) but does not decode. Charged to its own
    /// memory budget so many row groups can have their reads in flight (deep prefetch) while only a
    /// few are decoded at once (ColumnData). Decouples fetch depth from decode-ahead depth.
    ColumnDataPrefetch,
    ColumnData,

    Deliver,

    Deallocated,
};


/// We track approximate current memory usage per ReadStage that allocated the memory (*).
/// This struct aggregates how much memory was allocated by some operation.
/// ReadManager then uses it to update per-stage memory usage std::atomic counters.
/// (We do this instead of updating the std::atomics directly to reduce contention on the atomics.
///  I haven't checked whether this makes a difference.)
///
/// (*) This is to have a separate memory limit on each stage to automatically get higher parallelism
/// for stages that use little memory (e.g. prefetch small bloom filters and indexes for lots of row
/// groups in parallel, but read large column data for few row groups to not run out of memory).
/// TODO [parquet]: Try using thread-locals instead of manually error-pronely passing this everywhere.
struct MemoryUsageDiff
{
    ReadStage cur_stage;
    std::array<ssize_t, size_t(ReadStage::Deallocated)> by_stage {};
    /// Bit mask saying which ReadStage-s may have new tasks that can be scheduled to thread pool.
    UInt64 stages_to_schedule = 0;
    bool finalized = false;

    explicit MemoryUsageDiff(ReadStage cur_stage_) : cur_stage(cur_stage_) {}
    MemoryUsageDiff() = delete;
    MemoryUsageDiff(const MemoryUsageDiff &) = delete;
    MemoryUsageDiff & operator=(const MemoryUsageDiff &) = delete;

    ~MemoryUsageDiff()
    {
        chassert(finalized || std::uncaught_exceptions() > 0);
    }

    void allocated(size_t amount)
    {
        chassert(cur_stage > ReadStage::NotStarted);
        chassert(cur_stage < ReadStage::Deliver);
        chassert(!finalized);
        by_stage.at(size_t(cur_stage)) += ssize_t(amount);
    }
    void deallocated(size_t amount, ReadStage stage)
    {
        chassert(!finalized);
        by_stage.at(size_t(stage)) -= ssize_t(amount);
    }

    void scheduleAllStages()
    {
        stages_to_schedule = ~0ul;
    }
    void scheduleStage(ReadStage stage)
    {
        stages_to_schedule |= 1ul << size_t(stage);
    }
};

/// Remembers the ReadStage and size of a memory allocation.
/// Not RAII, you have to call reset to update the stat.
class MemoryUsageToken
{
public:
    MemoryUsageToken() = default;
    MemoryUsageToken(size_t val_, MemoryUsageDiff * diff)
        : alloc_stage(diff->cur_stage), val(val_)
    {
        diff->allocated(val);
    }
    MemoryUsageToken(MemoryUsageToken && rhs) noexcept
    {
        *this = std::move(rhs);
    }
    MemoryUsageToken & operator=(MemoryUsageToken && rhs) noexcept
    {
        chassert(!val);
        alloc_stage = std::exchange(rhs.alloc_stage, ReadStage::Deallocated);
        val = std::exchange(rhs.val, 0);
        return *this;
    }

    explicit operator bool() const { return alloc_stage != ReadStage::Deallocated; }

    void reset(MemoryUsageDiff * diff)
    {
        if (val)
            diff->deallocated(val, alloc_stage);
        val = 0;
        alloc_stage = ReadStage::Deallocated;
    }
    void add(size_t amount, MemoryUsageDiff * diff)
    {
        chassert(diff->cur_stage == alloc_stage);
        diff->allocated(amount);
        val += amount;
    }

    /// How much memory this token currently charges.
    size_t charged() const { return val; }

private:
    ReadStage alloc_stage = ReadStage::Deallocated;
    size_t val = 0;
};


#ifdef OS_LINUX

class CompletionNotification
{
private:
    enum State : UInt32
    {
        EMPTY,
        WAITING,
        NOTIFIED,
    };

    std::atomic<UInt32> val {0};

public:
    bool check() const;
    void wait();
    /// Wait up to timeout_ms. Returns true if notified, false on timeout.
    bool wait_for(UInt64 timeout_ms);
    void notify();
};

#else

class CompletionNotification
{
private:
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    std::atomic<bool> notified {false};

public:
    bool check() const;
    void wait();
    /// Wait up to timeout_ms. Returns true if notified, false on timeout.
    bool wait_for(UInt64 timeout_ms);
    void notify();
};

#endif

}
