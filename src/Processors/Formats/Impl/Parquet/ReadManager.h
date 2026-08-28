#pragma once

#include <Processors/Formats/Impl/Parquet/Reader.h>

namespace DB::Parquet
{

struct AtomicBitSet
{
    std::vector<std::atomic<UInt64>> a;

    void resize(size_t bits);

    bool set(size_t i, std::memory_order memory_order)
    {
        UInt64 mask = 1ull << (i & 63);
        UInt64 x = a[i >> 6].fetch_or(mask, memory_order);
        return (x & mask) == 0;
    }
    bool unset(size_t i, std::memory_order memory_order)
    {
        UInt64 mask = 1ull << (i & 63);
        UInt64 x = a[i >> 6].fetch_and(~mask, memory_order);
        return (x & mask) != 0;
    }

    std::optional<size_t> findFirst();
};

// I'd like to talk to the manager.
class ReadManager
{
public:
    Reader reader;

    /// To initialize ReadManager:
    ///  1. call manager.reader.prefetcher.init
    ///  2. call manager.reader.init
    ///  3. call manager.init
    /// (I'm trying this style because the usual pattern of passing-through lots of arguments through
    /// layers of constructors seems bad. This seems better but still not great, hopefully there's an
    /// even better way.)
    void init(FormatParserSharedResourcesPtr parser_shared_resources_, const std::optional<std::vector<size_t>> & buckets_to_read_);

    ~ReadManager();

    struct ReadResult
    {
        Chunk chunk;
        BlockMissingValues block_missing_values;
        size_t virtual_bytes_read = 0;
    };

    /// Not thread safe.
    ReadResult read();

    void cancel() noexcept;

private:
    using RowGroup = Reader::RowGroup;
    using RowSubgroup = Reader::RowSubgroup;
    using ColumnChunk = Reader::ColumnChunk;
    using ColumnSubchunk = Reader::ColumnSubchunk;
    using PrimitiveColumnInfo = Reader::PrimitiveColumnInfo;
    using OutputColumnState = Reader::OutputColumnState;

    struct Task
    {
        ReadStage stage{};
        size_t step_idx = 0; /// 0 = main step, (>=1) = prewhere steps
        size_t row_group_idx{};
        size_t row_subgroup_idx = UINT64_MAX;
        size_t column_idx = UINT64_MAX;
        size_t cost_estimate_bytes = 0;

        struct Comparator
        {
            bool operator()(const Task & x, const Task & y) const
            {
                return std::make_tuple(x.row_group_idx, x.row_subgroup_idx) > std::make_tuple(y.row_group_idx, y.row_subgroup_idx);
            }
        };
    };

    struct Stage
    {
        /// Tasks that are either in thread pool's queue or executing.
        std::atomic<size_t> batches_in_progress {0};

        /// Share of the parsing thread pool for this stage, independent of the memory pools.
        double thread_target_fraction = 1;

        /// We take advantage of the fact that each <row group, stage> pair can have at most one group
        /// of tasks in flight at a time. E.g. we create tasks to read columns in subgroup n, then
        /// wait for all of them to complete, then create tasks to read columns in subgroup n+1, etc.
        /// So each pair <row group, stage> is a sequence of groups of tasks, and their scheduling
        /// doesn't need any mutexes.
        AtomicBitSet schedulable_row_groups;
        std::vector<std::vector<Task>> row_group_tasks_to_schedule;
    };

    FormatParserSharedResourcesPtr parser_shared_resources;

    std::shared_ptr<ShutdownHelper> shutdown = std::make_shared<ShutdownHelper>();

    std::array<Stage, size_t(ReadStage::Deallocated)> stages;
    /// First row group that hasn't reached Deallocated stage.
    std::atomic<size_t> first_incomplete_row_group {0};

    /// See `MemoryPool`. Signed because deallocations can be flushed before the matching allocation
    /// on another thread.
    std::array<std::atomic<ssize_t>, NUM_MEMORY_POOLS> pool_usage {};
    std::array<double, NUM_MEMORY_POOLS> pool_fraction {};

    /// Bytes of delivered chunks that are still held by the pipeline (not yet consumed/dropped).
    /// Charged to the `Decoded` pool in addition to `pool_usage`, via `ChunkMemoryInfo` attached to
    /// each delivered `Chunk`. Kept separate (not folded into `pool_usage`) because it's decremented
    /// by chunk destructors running on arbitrary threads outside of `flushMemoryUsageDiff`, and
    /// because it must outlive `ReadManager` for chunks that are still alive after the reader is
    /// destroyed -- hence the `shared_ptr`.
    std::shared_ptr<std::atomic<ssize_t>> delivered_bytes = std::make_shared<std::atomic<ssize_t>>(0);

    SharedResourcesExt::Limits poolLimits(MemoryPool pool) const;

    /// One unit of read issue: the prefetch handles a <row group, subgroup, step, stage> needs,
    /// issued together. Planned in delivery order; issued by `pumpIssueQueue` under the
    /// bytes-in-flight target. The handles are pointers into `Reader::row_groups`, which outlives
    /// the queue; see `dropQueuedReads` for how they're kept from outliving the ColumnChunk they
    /// point into.
    struct PlannedRead
    {
        ReadStage stage{};           /// stage whose budget the bytes are charged to (`poolOf(stage)`)
        size_t row_group_idx = 0;
        size_t row_subgroup_idx = UINT64_MAX; /// UINT64_MAX for row-group-level (index) reads
        size_t step_idx = 0;
        std::vector<PrefetchHandle *> handles;
        size_t bytes = 0;            /// sum of the handles' request lengths
    };

    /// Protects `issue_queue` and the issuing of the reads in it: a thread holds it while it calls
    /// `Prefetcher::startPrefetch` for an entry it just popped, so that `dropQueuedReads` (called
    /// before a row group's `ColumnChunk`s are cleared) can be sure that no one is touching that row
    /// group's handles through the queue any more.
    std::mutex issue_mutex;
    std::deque<PlannedRead> issue_queue;
    /// Row groups whose data-page reads for the first step have been planned (`enqueueRowGroupPageReads`
    /// covers all subgroups at once, so it must happen only once per row group).
    AtomicBitSet page_reads_planned;

    /// Bytes we want the Prefetcher to have in flight: the fitted bandwidth*rtt*concurrency target,
    /// floored by `input_format_parquet_min_bytes_in_flight`.
    size_t bytesInFlightTarget() const;
    /// Whether this read must be issued even when the budgets are full, because the reader can't make
    /// progress without it. See the definition for exactly which reads those are.
    bool isPrivilegedRead(const PlannedRead & planned) const;
    /// Issue queued reads in order while `prefetcher.bytesInFlight() + planned.bytes` stays under the
    /// target and the read's memory pool has room. Reads that the reader cannot make progress without
    /// are issued regardless of both bounds -- those are the index entries and, of the first incomplete
    /// row group, the page reads of the subgroup at `read_ptr` (see `isPrivilegedRead`), not everything
    /// belonging to that row group. Charges the bytes to `poolOf(planned.stage)` via `diff`. Never calls
    /// `flushMemoryUsageDiff` (the caller owns the diff), so it can be called from it.
    void pumpIssueQueue(MemoryUsageDiff & diff);
    void enqueueRowGroupIndexReads(size_t row_group_idx);
    void enqueueRowGroupPageReads(size_t row_group_idx, size_t step_idx);
    /// Start the reads planned for this <stage, row group, subgroup, step> and take them out of the
    /// queue: the demand path calls this when the pump hasn't got to them yet, bypassing the budget.
    /// Every stage that starts or resets a handle the planner may have queued must do this first: the
    /// handles are not protected against being started by two threads at once, and a stage that resets
    /// one would leave the queue holding an entry the pump could then try to issue.
    void takeQueuedReads(ReadStage stage, size_t row_group_idx, size_t row_subgroup_idx, size_t step_idx, MemoryUsageDiff & diff);
    /// Forget everything planned for this row group. Must be called before clearing its ColumnChunks:
    /// entries may point into `ColumnChunk::data_pages`, whose buffer clearing frees.
    void dropQueuedReads(size_t row_group_idx);
    /// For assertions only: is anything still queued for this <stage, row group, subgroup, step>?
    bool hasQueuedReads(ReadStage stage, size_t row_group_idx, size_t row_subgroup_idx, size_t step_idx);

    std::mutex delivery_mutex;
    std::priority_queue<Task, std::vector<Task>, Task::Comparator> delivery_queue;
    std::condition_variable delivery_cv;
    std::exception_ptr exception;
    /// Nullopt means that ReadManager reads all row groups
    std::optional<std::unordered_set<UInt64>> row_groups_to_read;

    void scheduleTask(Task task, bool is_first_in_group, MemoryUsageDiff & diff, std::vector<Task> & out_tasks);
    void runTask(Task task, bool last_in_batch, MemoryUsageDiff & diff);
    void runBatchOfTasks(const std::vector<Task> & tasks) noexcept;
    void scheduleTasksIfNeeded(ReadStage stage_idx);
    void finishRowGroupStage(size_t row_group_idx, ReadStage stage, MemoryUsageDiff & diff);
    void finishRowSubgroupStage(size_t row_group_idx, size_t row_subgroup_idx, ReadStage stage, size_t step_idx, MemoryUsageDiff & diff);
    /// Free some memory ColumnChunk that's not needed after decoding is done in all row sugroups.
    /// Call sites should be careful to not call it from multiple threads in parallel.
    void clearColumnChunk(ColumnChunk & column, MemoryUsageDiff & diff);
    void clearRowSubgroup(RowSubgroup & row_subgroup, MemoryUsageDiff & diff);
    void setTasksToSchedule(size_t row_group_idx, ReadStage stage, std::vector<Task> add_tasks, MemoryUsageDiff & diff);
    void addTasksToReadColumns(size_t row_group_idx, size_t row_subgroup_idx, ReadStage stage, size_t step_idx, MemoryUsageDiff & diff);
    void advanceDeliveryPtrIfNeeded(size_t row_group_idx, MemoryUsageDiff & diff);
    void flushMemoryUsageDiff(MemoryUsageDiff && diff);
    std::string collectDeadlockDiagnostics();
};

}
