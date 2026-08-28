#include <Processors/Formats/Impl/Parquet/ReadManager.h>

#include <Processors/Formats/Impl/Parquet/ChunkMemoryInfo.h>
#include <Common/BitHelpers.h>
#include <Common/Logger.h>
#include <Common/ProfileEvents.h>
#include <Columns/ColumnsCommon.h>
#include <Formats/FormatFilterInfo.h>
#include <Formats/FormatParserSharedResources.h>
#include <Processors/Formats/IInputFormat.h>
#include <Common/logger_useful.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int QUERY_WAS_CANCELLED;
}

namespace ProfileEvents
{
    extern const Event ParquetDecodingTasks;
    extern const Event ParquetDecodingTaskBatches;
    extern const Event ParquetReadRowGroups;
    extern const Event ParquetPrunedRowGroups;
    extern const Event ParquetPlannedReads;
    extern const Event ParquetIssueQueueStalls;
}

namespace DB::Parquet
{

void AtomicBitSet::resize(size_t bits)
{
    a = std::vector<std::atomic<UInt64>>((bits + 63) / 64);
}

std::optional<size_t> AtomicBitSet::findFirst()
{
    for (size_t i = 0; i < a.size(); ++i)
    {
        UInt64 x = a[i].load(std::memory_order_relaxed);
        if (x)
            return (i << 6) + getTrailingZeroBitsUnsafe(x);
    }
    return std::nullopt;
}

void ReadManager::init(FormatParserSharedResourcesPtr parser_shared_resources_, const std::optional<std::vector<size_t>> & buckets_to_read_)
{
    parser_shared_resources = parser_shared_resources_;

    if (reader.file_metadata.schema.empty())
        reader.file_metadata = Reader::readFileMetaData(reader.prefetcher);

    if (buckets_to_read_)
    {
        row_groups_to_read = std::unordered_set<UInt64>{};
        for (auto rg : *buckets_to_read_)
            row_groups_to_read->insert(rg);
    }
    reader.prefilterAndInitRowGroups(row_groups_to_read);
    reader.preparePrewhere();

    ProfileEvents::increment(ProfileEvents::ParquetReadRowGroups, reader.row_groups.size());
    ProfileEvents::increment(ProfileEvents::ParquetPrunedRowGroups, reader.file_metadata.row_groups.size() - reader.row_groups.size());

    size_t num_row_groups = reader.row_groups.size();
    page_reads_planned.resize(num_row_groups);
    for (size_t i = size_t(ReadStage::NotStarted) + 1; i < size_t(ReadStage::Deliver); ++i)
    {
        stages[i].schedulable_row_groups.resize(num_row_groups);
        stages[i].row_group_tasks_to_schedule.resize(num_row_groups);
    }

    /// Per-stage thread budgets (sum to 1) so no stage starves the others of parallelism.
    /// Decode holds large columns -> bounded memory but most threads (only CPU-bound stage);
    /// index/bloom/prefetch only issue async reads -> fixed small shares. decode_thread_fraction
    /// is decode's thread share (issuers split the rest). Memory is budgeted separately, by pool
    /// (see `MemoryPool` / `pool_fraction` below), not per stage.
    const double decode_thread_fraction = reader.options.format.parquet.decode_thread_fraction;
    if (!(decode_thread_fraction >= 0 && decode_thread_fraction <= 1))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "input_format_parquet_decode_thread_fraction must be in [0, 1], got {}", decode_thread_fraction);

    auto set_thread_fraction = [&](ReadStage s, double thread_fraction)
    {
        stages[size_t(s)].thread_target_fraction = thread_fraction;
    };
    const double issuer_thread_fraction = (1.0 - decode_thread_fraction) / 5.0;      // five read-issuing stages split the rest
    set_thread_fraction(ReadStage::NotStarted, 0);
    set_thread_fraction(ReadStage::BloomFilterHeader, issuer_thread_fraction);
    set_thread_fraction(ReadStage::BloomFilterBlocksOrDictionary, issuer_thread_fraction);
    set_thread_fraction(ReadStage::ColumnIndexAndOffsetIndex, issuer_thread_fraction);
    set_thread_fraction(ReadStage::OffsetIndex, issuer_thread_fraction);
    set_thread_fraction(ReadStage::ColumnDataPrefetch, issuer_thread_fraction);
    set_thread_fraction(ReadStage::ColumnData, decode_thread_fraction);
    set_thread_fraction(ReadStage::Deliver, 0);

    /// Normalize (defensive: the fractions already sum to 1).
    double thread_sum = 0;
    for (const Stage & stage : stages)
        thread_sum += stage.thread_target_fraction;
    for (Stage & stage : stages)
        stage.thread_target_fraction /= thread_sum;

    /// Memory is budgeted by lifetime, not by stage: see `MemoryPool`. Metadata (bloom filters,
    /// indexes, dictionary pages) gets a fixed small share; the rest splits between compressed
    /// data pages in flight (bounds read-ahead depth) and decoded columns (including chunks
    /// already delivered to the pipeline but not yet consumed).
    const double compressed_fraction = reader.options.format.parquet.compressed_memory_fraction;
    if (!(compressed_fraction > 0 && compressed_fraction < 0.95))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "input_format_parquet_compressed_memory_fraction must be in (0, 0.95), got {}", compressed_fraction);
    pool_fraction[size_t(MemoryPool::Metadata)] = 0.05;
    pool_fraction[size_t(MemoryPool::Compressed)] = compressed_fraction;
    pool_fraction[size_t(MemoryPool::Decoded)] = 1.0 - 0.05 - compressed_fraction;

    /// Plan the index reads (bloom filter headers, column indexes, offset indexes, dictionary pages)
    /// of *every* row group up front, in delivery order. These are the reads that serialize row
    /// groups if they're issued stage by stage, one row group at a time: each is only a few KB, but
    /// each costs a round trip. `pumpIssueQueue` below issues as many of them as the bytes-in-flight
    /// target allows, and the rest follow as earlier reads land.
    for (size_t i = 0; i < reader.row_groups.size(); ++i)
        enqueueRowGroupIndexReads(i);

    /// The NotStarted stage completed for all row groups, transition to next stage.
    MemoryUsageDiff diff(ReadStage::NotStarted);
    for (size_t i = 0; i < reader.row_groups.size(); ++i)
        finishRowGroupStage(i, ReadStage::NotStarted, diff);
    pumpIssueQueue(diff);
    flushMemoryUsageDiff(std::move(diff));
}

ReadManager::~ReadManager()
{
    shutdown->shutdown();
}

SharedResourcesExt::Limits ReadManager::poolLimits(MemoryPool pool) const
{
    /// Thread fraction is per stage, not per pool; callers that need it read Stage::thread_target_fraction.
    return SharedResourcesExt::getLimitsPerReader(*parser_shared_resources, pool_fraction[size_t(pool)], /*thread_fraction=*/ 1.0);
}

void ReadManager::cancel() noexcept
{
    {
        std::lock_guard lock(delivery_mutex);
        if (exception)
            return;
        exception = std::make_exception_ptr(Exception(ErrorCodes::QUERY_WAS_CANCELLED, "Cancelled"));
    }
    delivery_cv.notify_all();
}

void ReadManager::finishRowGroupStage(size_t row_group_idx, ReadStage stage, MemoryUsageDiff & diff)
{
    RowGroup & row_group = reader.row_groups[row_group_idx];

    /// Finish the stage.
    if (stage == ReadStage::BloomFilterBlocksOrDictionary)
    {
        if (!reader.applyBloomAndDictionaryFilters(row_group))
            stage = ReadStage::Deliver; // skip the row group
        for (auto & c : row_group.columns)
        {
            c.bloom_filter_header_prefetch.reset(&diff);
            c.bloom_filter_data_prefetch.reset(&diff);
            for (auto & b : c.bloom_filter_blocks)
                b.prefetch.reset(&diff);
            c.bloom_filter_blocks.clear();
        }
    }

    /// Determine what stage to transition to and which columns are involved.
    std::vector<Task> add_tasks;
    while (true) // loop over skipped stages
    {
        chassert(stage < ReadStage::Deallocated);
        stage = ReadStage(int(stage) + 1);

        /// Start the new stage.
        switch (stage)
        {
            case ReadStage::NotStarted:
            case ReadStage::ColumnDataPrefetch:
            case ReadStage::ColumnData:
            case ReadStage::Deliver:
                chassert(false);
                break;

            case ReadStage::BloomFilterHeader:
                for (size_t i = 0; i < row_group.columns.size(); ++i)
                    if (row_group.columns[i].bloom_filter_header_prefetch)
                        add_tasks.push_back(Task {
                            .stage = ReadStage::BloomFilterHeader,
                            .row_group_idx = row_group_idx, .column_idx = i});
                break;
            case ReadStage::BloomFilterBlocksOrDictionary:
                for (size_t i = 0; i < row_group.columns.size(); ++i)
                {
                    const auto & c = row_group.columns[i];
                    if (!c.bloom_filter_blocks.empty() || c.use_dictionary_filter)
                        add_tasks.push_back(Task {
                            .stage = ReadStage::BloomFilterBlocksOrDictionary,
                            .row_group_idx = row_group_idx, .column_idx = i});
                }
                break;
            case ReadStage::ColumnIndexAndOffsetIndex:
                for (size_t i = 0; i < row_group.columns.size(); ++i)
                    if (row_group.columns[i].use_column_index)
                        add_tasks.push_back(Task {
                            .stage = ReadStage::ColumnIndexAndOffsetIndex,
                            .row_group_idx = row_group_idx, .column_idx = i});
                break;
            case ReadStage::OffsetIndex: // (first of the per-row-subgroup stages)
                reader.intersectColumnIndexResultsAndInitSubgroups(row_group);
                if (!row_group.subgroups.empty())
                {
                    /// The offset indexes of this row group are decoded and its subgroups are known,
                    /// so the byte ranges of the data pages every subgroup needs are known too: plan
                    /// them all now (in subgroup order) instead of one subgroup at a time. Nothing
                    /// has been scheduled for this row group yet (the tasks below are only queued;
                    /// `flushMemoryUsageDiff` schedules them), so we're the only thread touching
                    /// these column chunks.
                    /// Only the columns whose offset index is already decoded (the ones with a
                    /// column-index condition) can be planned here; the rest are planned by the
                    /// second hook, in `finishRowSubgroupStage`, once the per-subgroup `OffsetIndex`
                    /// stage has decoded them. Deliberately without setting `page_reads_planned`:
                    /// that hook must still run for the remaining columns, and re-walking the ones
                    /// planned here is a no-op because `determinePagesToPrefetch` has already
                    /// advanced their page cursor to the end.
                    size_t first_step = reader.steps.empty() ? 0 : 1;
                    enqueueRowGroupPageReads(row_group_idx, first_step);
                    pumpIssueQueue(diff);

                    row_group.stage.store(ReadStage::ColumnData);
                    row_group.stage_tasks_remaining.store(row_group.subgroups.size(), std::memory_order_relaxed);
                    /// Start the first subgroup.
                    finishRowSubgroupStage(row_group_idx, /*row_subgroup_idx=*/ 0, ReadStage::NotStarted, /*step_idx=*/ 0, diff);
                    return;
                }
                /// The whole row group was filtered out.
                stage = ReadStage::Deliver;
                break;
            case ReadStage::Deallocated:
                /// We should be careful which row_group fields we access here. Other threads may
                /// still be mutating subgroups or columns. In particular, if `!subgroups.empty()`,
                /// clearColumnChunk is called by finishRowSubgroupStage (after all subgroups are read),
                /// which can run in parallel with finishRowGroupStage (after all subgroups are delivered).
                /// It may be tempting to do things like `row_group.subgroups.clear()`, but we can't,
                /// not without adding some mutexes.
                if (row_group.subgroups.empty())
                {
                    /// Before clearing: the queue may still hold this row group's index reads (e.g.
                    /// the row group was filtered out by its bloom filter, so the column index reads
                    /// planned at init were never needed).
                    dropQueuedReads(row_group_idx);
                    for (auto & c : row_group.columns)
                        clearColumnChunk(c, diff);
                }
                break;
        }

        if (!add_tasks.empty() || stage == ReadStage::Deallocated)
            break;

        /// Nothing needs to be done for this stage, skip to next stage.
    }

    row_group.stage.store(stage);
    row_group.stage_tasks_remaining.store(add_tasks.size(), std::memory_order_relaxed);

    if (stage == ReadStage::Deallocated)
    {
        size_t i = first_incomplete_row_group.load();
        while (i < reader.row_groups.size() && reader.row_groups[i].stage.load() == ReadStage::Deallocated)
        {
            if (first_incomplete_row_group.compare_exchange_weak(i, i + 1))
            {
                diff.scheduleAllStages();

                /// Notify read() if everything is done or if it's relying on
                /// first_incomplete_row_group to deliver chunks in order.
                if (i + 1 == reader.row_groups.size() || reader.options.format.parquet.preserve_order)
                {
                    {
                        /// Lock and unlock to avoid race condition on condition variable.
                        /// (Otherwise the notify_all() may happen after read() saw the old
                        ///  first_incomplete_row_group value but before it started waiting
                        ///  on delivery_cv.)
                        std::lock_guard lock(delivery_mutex);
                    }
                    delivery_cv.notify_all();
                }
            }
        }
    }

    if (!add_tasks.empty())
        setTasksToSchedule(row_group_idx, stage, std::move(add_tasks), diff);
}

void ReadManager::setTasksToSchedule(size_t row_group_idx, ReadStage stage, std::vector<Task> add_tasks, MemoryUsageDiff & diff)
{
    LOG_TEST(getLogger("ParquetReadManager"), "setTasksToSchedule: row_group_idx={}, stage={}, add_tasks={}", row_group_idx, static_cast<Int32>(stage), add_tasks.size());
    for (const auto & task : add_tasks)
    {
        LOG_TEST(getLogger("ParquetReadManager"), "setTasksToSchedule: {} {} {} {} {}", task.column_idx, task.row_group_idx, task.cost_estimate_bytes, static_cast<Int32>(task.stage), task.step_idx);
    }
    chassert(!add_tasks.empty());
    Stage & stage_state = stages.at(size_t(stage));
    auto & tasks = stage_state.row_group_tasks_to_schedule.at(row_group_idx);
    chassert(tasks.empty());
    tasks = std::move(add_tasks);
    bool changed = stage_state.schedulable_row_groups.set(row_group_idx, std::memory_order_release);  /// NOLINT(clang-analyzer-deadcode.DeadStores)
    auto first_row_group = stage_state.schedulable_row_groups.findFirst();
    if (first_row_group)
        LOG_TEST(getLogger("ParquetReadManager"), "setTasksToSchedule: check row group is set: {}", *first_row_group);
    else
        LOG_TEST(getLogger("ParquetReadManager"), "setTasksToSchedule: no row groups to set");
    chassert(changed);
    diff.scheduleStage(stage);
}

void ReadManager::addTasksToReadColumns(size_t row_group_idx, size_t row_subgroup_idx, ReadStage stage, size_t step_idx, MemoryUsageDiff & diff)
{
    RowGroup & row_group = reader.row_groups[row_group_idx];
    RowSubgroup & row_subgroup = row_group.subgroups[row_subgroup_idx];
    std::vector<Task> add_tasks;

    while (true) // offset index, then data
    {
        bool is_offset_index = stage == ReadStage::OffsetIndex;

        for (size_t i = 0; i < reader.primitive_columns.size(); ++i)
        {
            if (reader.primitive_columns[i].first_step_to_calculate != step_idx)
                continue;

            ColumnChunk & c = row_group.columns.at(i);
            if (is_offset_index)
            {
                if (c.offset_index_prefetch && c.offset_index.page_locations.empty())
                {
                    LOG_TEST(getLogger("ParquetReadManager"), "addTasksToReadColumns: added OffsetIndex: i={} step_idx={} row_group_idx={} row_subgroup_idx={}", i, step_idx, row_group_idx, row_subgroup_idx);

                    /// If offset index for this column wasn't read by previous stages, make a task
                    /// to read it before reading data.
                    add_tasks.push_back(Task {
                        .stage = ReadStage::OffsetIndex,
                        .step_idx = step_idx,
                        .row_group_idx = row_group_idx,
                        .row_subgroup_idx = row_subgroup_idx,
                        .column_idx = i});
                }
                else
                {
                    LOG_TEST(getLogger("ParquetReadManager"), "addTasksToReadColumns: not added due locations empty i={} step_idx={} row_group_idx={} row_subgroup_idx={}", i, step_idx, row_group_idx, row_subgroup_idx);
                }
            }
            else
            {
                /// `stage` is ColumnDataPrefetch (issue reads) or ColumnData (decode).
                LOG_TEST(getLogger("ParquetReadManager"), "addTasksToReadColumns: added {}: i={} step_idx={} row_group_idx={} row_subgroup_idx={}", magic_enum::enum_name(stage), i, step_idx, row_group_idx, row_subgroup_idx);
                add_tasks.push_back(Task {
                    .stage = stage,
                    .step_idx = step_idx,
                    .row_group_idx = row_group_idx,
                    .row_subgroup_idx = row_subgroup_idx,
                    .column_idx = i});
            }
        }

        if (add_tasks.empty() && is_offset_index)
        {
            /// Don't need to read offset index, move on to the next stage (ColumnDataPrefetch).
            stage = ReadStage::ColumnDataPrefetch;
            continue;
        }

        if (add_tasks.empty())
            /// If we don't need to read any columns, add a task that will just call finishRowGroupStage().
            /// (Why go through the task queue instead of skipping the stage at this function's call site?
            ///  Because (a) less code this way, (b) to make memory usage limiting for PREWHERE filter mask
            ///  (RowSubgroup.filter.memory) work correctly when PREWHERE expression doesn't use any
            ///  columns (note: the expression may still be nontrivial, e.g. `rand()%2=0`).)
            add_tasks.push_back(Task {
                .stage = stage,
                .step_idx = step_idx,
                .row_group_idx = row_group_idx,
                .row_subgroup_idx = row_subgroup_idx,
                .column_idx = UINT64_MAX});

        row_subgroup.stage.exchange(stage, std::memory_order_relaxed);  /// NOLINT(clang-analyzer-deadcode.DeadStores)
        row_subgroup.stage_tasks_remaining.store(add_tasks.size(), std::memory_order_relaxed);
        setTasksToSchedule(row_group_idx, stage, std::move(add_tasks), diff);

        break;
    }
}

void ReadManager::finishRowSubgroupStage(size_t row_group_idx, size_t row_subgroup_idx, ReadStage stage, size_t step_idx, MemoryUsageDiff & diff)
{
    RowGroup & row_group = reader.row_groups[row_group_idx];
    RowSubgroup & row_subgroup = row_group.subgroups[row_subgroup_idx];
    std::optional<size_t> advanced_ptr;

    LOG_TEST(getLogger("ParquetReadManager"), "finishRowSubgroupStage: rg={} sg={} stage={} step={} rows_pass={} rows_total={}",
              row_group_idx, row_subgroup_idx, magic_enum::enum_name(stage), step_idx,
              row_subgroup.filter.rows_pass, row_subgroup.filter.rows_total);

    switch (stage)
    {
        case ReadStage::NotStarted:
        {
            /// 1 if there are prewhere steps, 0 otherwise
            size_t first_step = reader.steps.empty() ? 0 : 1;
            if (first_step < reader.steps.size() + 1)
            {
                addTasksToReadColumns(row_group_idx, row_subgroup_idx, ReadStage::OffsetIndex, first_step, diff);
                return;
            }
            /// No steps, go directly to step 0
            addTasksToReadColumns(row_group_idx, row_subgroup_idx, ReadStage::OffsetIndex, 0, diff);
            return;
        }
        case ReadStage::ColumnData:
        {
            if (row_subgroup.filter.rows_pass == 0)
                break;
            if (step_idx > 0 && step_idx <= reader.steps.size())
            {
                reader.applyPrewhere(row_subgroup, row_group, step_idx);

                size_t next_step = (step_idx < reader.steps.size()) ? step_idx + 1 : 0;
                if (next_step > 0)
                {
                    /// More prewhere steps to process.
                    addTasksToReadColumns(row_group_idx, row_subgroup_idx, ReadStage::OffsetIndex, next_step, diff);
                    return;
                }
                else
                {
                    /// All prewhere steps done, move to step 0
                    addTasksToReadColumns(row_group_idx, row_subgroup_idx, ReadStage::OffsetIndex, 0, diff);
                    return;
                }
            }
            else if (step_idx == 0)
            {
                /// Main step finished. Move to Deliver.
                LOG_TEST(getLogger("ParquetReadManager"), "finishRowSubgroupStage: rg={} sg={} main step finished, moving to Deliver, advancing read_ptr",
                          row_group_idx, row_subgroup_idx);
                row_subgroup.stage.store(ReadStage::Deliver, std::memory_order::relaxed);

                /// Must add to delivery_queue before advancing read_ptr to deliver subgroups in order.
                {
                    std::lock_guard lock(delivery_mutex);
                    delivery_queue.push(Task {.stage = ReadStage::Deliver, .row_group_idx = row_group_idx, .row_subgroup_idx = row_subgroup_idx});
                    LOG_TEST(getLogger("ParquetReadManager"), "finishRowSubgroupStage: rg={} sg={} added to delivery_queue, size={}",
                              row_group_idx, row_subgroup_idx, delivery_queue.size());
                }

                row_group.read_ptr.store(row_subgroup_idx + 1);
                advanced_ptr = row_subgroup_idx + 1;
                LOG_TEST(getLogger("ParquetReadManager"), "finishRowSubgroupStage: rg={} sg={} advanced read_ptr -> {}",
                          row_group_idx, row_subgroup_idx, row_subgroup_idx + 1);
                delivery_cv.notify_one();
                break;
            }
            else
            {
                chassert(false);
                break;
            }
        }
        case ReadStage::Deliver:
        {
            row_subgroup.stage.store(ReadStage::Deallocated);
            clearRowSubgroup(row_subgroup, diff);
            advanceDeliveryPtrIfNeeded(row_group_idx, diff);
            return;
        }
        case ReadStage::BloomFilterHeader:
        case ReadStage::BloomFilterBlocksOrDictionary:
        case ReadStage::ColumnIndexAndOffsetIndex:
        case ReadStage::OffsetIndex:
        {
            /// The offset indexes of this step's columns are decoded now. For the first step that
            /// means the page ranges of *every* subgroup of this row group are known: their filters
            /// come from the column index only, and PREWHERE (which is what narrows them further)
            /// runs after this step. So plan all of them, once per row group, instead of one
            /// subgroup at a time. Later steps must wait for their own PREWHERE result, so they keep
            /// planning per subgroup (through `scheduleTask`), as before.
            /// Subgroups of one row group are read strictly one at a time, so no other thread is
            /// looking at these column chunks.
            const size_t first_step = reader.steps.empty() ? 0 : 1;
            if (step_idx == first_step && page_reads_planned.set(row_group_idx, std::memory_order_relaxed))
            {
                enqueueRowGroupPageReads(row_group_idx, step_idx);
                pumpIssueQueue(diff);
            }

            /// Prerequisites read; issue the compressed data-page reads (but don't decode yet).
            addTasksToReadColumns(row_group_idx, row_subgroup_idx, ReadStage::ColumnDataPrefetch, step_idx, diff);
            return;
        }
        case ReadStage::ColumnDataPrefetch:
        {
            /// Data-page reads issued (in flight in the Prefetcher's io pool); now decode.
            addTasksToReadColumns(row_group_idx, row_subgroup_idx, ReadStage::ColumnData, step_idx, diff);
            return;
        }
        case ReadStage::Deallocated:
            chassert(false);
            break;
    }

    /// Start reading the next row subgroup if ready.
    /// Skip subgroups that were fully filtered out by prewhere.
    size_t main_ptr = row_group.read_ptr.load();
    LOG_TEST(getLogger("ParquetReadManager"), "finishRowSubgroupStage: rg={} starting next subgroup, read_ptr={} subgroups={} delivery_ptr={}",
              row_group_idx, main_ptr, row_group.subgroups.size(), row_group.delivery_ptr.load());

    /// Start next subgroup to read (sequential: one subgroup at a time).
    while (main_ptr < row_group.subgroups.size())
    {
        RowSubgroup & next_subgroup = row_group.subgroups[main_ptr];
        ReadStage next_subgroup_stage = next_subgroup.stage.load();
        if (!next_subgroup.stage.compare_exchange_strong(
                next_subgroup_stage, ReadStage::OffsetIndex))
            break;

        if (next_subgroup.filter.rows_pass > 0)
        {
            size_t first_step = reader.steps.empty() ? 0 : 1;
            addTasksToReadColumns(row_group_idx, main_ptr, ReadStage::OffsetIndex, first_step, diff);
            break;
        }
        else
        {
            row_group.read_ptr.store(main_ptr + 1);
            main_ptr += 1;
            advanced_ptr = main_ptr;
            next_subgroup.stage.store(ReadStage::Deallocated);
            clearRowSubgroup(next_subgroup, diff);
        }
    }

    if (advanced_ptr.has_value())
    {
        advanceDeliveryPtrIfNeeded(row_group_idx, diff);

        if (*advanced_ptr == row_group.subgroups.size())
        {
            /// If we've read (not necessarily delivered) all subgroups, we can deallocate things
            /// like dictionary page and offset index. Clear all columns (including PREWHERE-only),
            /// since we scheduled ColumnData prefetches for all of them and must release the memory.
            /// Every subgroup went through ColumnDataPrefetch, which takes its planned reads out of
            /// the queue, so there should be nothing left for this row group; drop anyway, because
            /// clearing frees the `data_pages` some queue entries point into.
            dropQueuedReads(row_group_idx);
            for (size_t i = 0; i < reader.primitive_columns.size(); ++i)
                clearColumnChunk(row_group.columns.at(i), diff);
        }
    }
}

void ReadManager::advanceDeliveryPtrIfNeeded(size_t row_group_idx, MemoryUsageDiff & diff)
{
    RowGroup & row_group = reader.row_groups[row_group_idx];
    size_t delivery_ptr = row_group.delivery_ptr.load();
    size_t initial_delivery_ptr = delivery_ptr;

    LOG_TEST(getLogger("ParquetReadManager"), "advanceDeliveryPtrIfNeeded: rg={} initial_delivery_ptr={} subgroups={} read_ptr={}",
              row_group_idx, delivery_ptr, row_group.subgroups.size(), row_group.read_ptr.load());

    while (delivery_ptr < row_group.subgroups.size() &&
           row_group.subgroups[delivery_ptr].stage.load() == ReadStage::Deallocated)
    {
        if (!row_group.delivery_ptr.compare_exchange_weak(delivery_ptr, delivery_ptr + 1))
            continue;
        size_t old_delivery_ptr = delivery_ptr;
        delivery_ptr += 1;

        if (delivery_ptr == row_group.subgroups.size()) // only if *this thread* incremented it
            finishRowGroupStage(row_group_idx, ReadStage::Deliver, diff);
        else if (first_incomplete_row_group.load() == row_group_idx)
             diff.scheduleAllStages();

        LOG_TEST(getLogger("ParquetReadManager"), "advanceDeliveryPtrIfNeeded: rg={} advanced delivery_ptr {} -> {}",
                  row_group_idx, old_delivery_ptr, delivery_ptr);
    }

    if (delivery_ptr > initial_delivery_ptr)
    {
        LOG_TEST(getLogger("ParquetReadManager"), "advanceDeliveryPtrIfNeeded: rg={} final delivery_ptr={} read_ptr={}",
                  row_group_idx, row_group.delivery_ptr.load(), row_group.read_ptr.load());
    }
}

void ReadManager::enqueueRowGroupIndexReads(size_t row_group_idx)
{
    /// `input_format_parquet_min_bytes_in_flight = 0` turns read-ahead planning off: nothing is
    /// queued, so `pumpIssueQueue` has nothing to issue and every read is started by the stage that
    /// needs it, as it was before the issue controller existed.
    if (reader.options.format.parquet.min_bytes_in_flight == 0)
        return;

    RowGroup & row_group = reader.row_groups[row_group_idx];
    std::vector<PlannedRead> planned;

    auto plan = [&](ReadStage stage, std::vector<PrefetchHandle *> handles)
    {
        std::erase_if(handles, [](const PrefetchHandle * h) { return !*h; });
        if (handles.empty())
            return;
        size_t bytes = 0;
        for (const PrefetchHandle * h : handles)
            bytes += reader.prefetcher.requestLength(*h);
        planned.push_back(PlannedRead {
            .stage = stage, .row_group_idx = row_group_idx, .row_subgroup_idx = UINT64_MAX,
            .step_idx = 0, .handles = std::move(handles), .bytes = bytes});
    };

    std::vector<PrefetchHandle *> handles;
    handles.reserve(row_group.columns.size() * 2);

    for (auto & c : row_group.columns)
        handles.push_back(&c.bloom_filter_header_prefetch);
    plan(ReadStage::BloomFilterHeader, std::move(handles));

    /// Both indexes of a column are read by one task (the `ColumnIndexAndOffsetIndex` stage reads
    /// them for columns with a column-index condition), and the offset index of every other column
    /// is read by the per-subgroup `OffsetIndex` stage. Plan them all here: the offset index is what
    /// the data-page reads need, and reading it one row group at a time is what limits read depth.
    handles = {};
    for (auto & c : row_group.columns)
    {
        handles.push_back(&c.column_index_prefetch);
        handles.push_back(&c.offset_index_prefetch);
    }
    plan(ReadStage::ColumnIndexAndOffsetIndex, std::move(handles));

    /// Bloom filter blocks aren't planned here: which blocks are needed is only known after the
    /// header is decoded.
    handles = {};
    for (auto & c : row_group.columns)
        if (c.use_dictionary_filter)
            handles.push_back(&c.dictionary_page_prefetch);
    plan(ReadStage::BloomFilterBlocksOrDictionary, std::move(handles));

    if (planned.empty())
        return;
    std::lock_guard lock(issue_mutex);
    for (PlannedRead & p : planned)
        issue_queue.push_back(std::move(p));
}

void ReadManager::enqueueRowGroupPageReads(size_t row_group_idx, size_t step_idx)
{
    /// See `enqueueRowGroupIndexReads`: 0 disables read-ahead planning.
    if (reader.options.format.parquet.min_bytes_in_flight == 0)
        return;

    RowGroup & row_group = reader.row_groups[row_group_idx];
    std::vector<PlannedRead> planned;

    for (size_t subgroup_idx = 0; subgroup_idx < row_group.subgroups.size(); ++subgroup_idx)
    {
        RowSubgroup & row_subgroup = row_group.subgroups[subgroup_idx];
        if (row_subgroup.filter.rows_pass == 0)
            continue;

        std::vector<PrefetchHandle *> handles;
        for (size_t i = 0; i < reader.primitive_columns.size(); ++i)
        {
            if (reader.primitive_columns[i].first_step_to_calculate != step_idx)
                continue;
            ColumnChunk & column = row_group.columns.at(i);

            /// Skip a column whose offset index hasn't been decoded yet: `determinePagesToPrefetch`
            /// needs `offset_index.page_locations` to split the column chunk into page ranges, and
            /// this column will get its offset index read by the per-subgroup `OffsetIndex` stage,
            /// which then issues its pages the usual way (`scheduleTask`).
            if (column.offset_index_prefetch && column.offset_index.page_locations.empty())
                continue;

            reader.determinePagesToPrefetch(column, row_subgroup, row_group, handles);

            /// The dictionary page and the whole-column-chunk read (the latter only when there's no
            /// offset index to read pages individually with) are one read for the whole column
            /// chunk, not one per subgroup: plan them with the first subgroup that needs the column,
            /// and tell the per-subgroup path in `scheduleTask` to leave them to us.
            if (!column.dictionary_and_whole_chunk_planned)
            {
                bool pushed = false;
                if (!column.dictionary.isInitialized() && column.dictionary_page_prefetch)
                {
                    handles.push_back(&column.dictionary_page_prefetch);
                    pushed = true;
                }
                if (column.data_pages.empty() && column.data_pages_prefetch)
                {
                    handles.push_back(&column.data_pages_prefetch);
                    pushed = true;
                }
                column.dictionary_and_whole_chunk_planned = pushed;
            }
        }

        std::erase_if(handles, [](const PrefetchHandle * h) { return !*h; });
        if (handles.empty())
            continue;
        size_t bytes = 0;
        for (const PrefetchHandle * h : handles)
            bytes += reader.prefetcher.requestLength(*h);
        planned.push_back(PlannedRead {
            .stage = ReadStage::ColumnDataPrefetch, .row_group_idx = row_group_idx,
            .row_subgroup_idx = subgroup_idx, .step_idx = step_idx,
            .handles = std::move(handles), .bytes = bytes});
    }

    if (planned.empty())
        return;
    std::lock_guard lock(issue_mutex);
    for (PlannedRead & p : planned)
        issue_queue.push_back(std::move(p));
}

void ReadManager::takeQueuedReads(ReadStage stage, size_t row_group_idx, size_t row_subgroup_idx, size_t step_idx, MemoryUsageDiff & diff)
{
    std::lock_guard lock(issue_mutex);
    for (auto it = issue_queue.begin(); it != issue_queue.end();)
    {
        if (it->stage == stage && it->row_group_idx == row_group_idx && it->row_subgroup_idx == row_subgroup_idx && it->step_idx == step_idx)
        {
            /// Issue first, erase after, as in `pumpIssueQueue`: if `startPrefetch` throws, the entry
            /// stays in the queue, so its handles are still reachable instead of being lost with the
            /// popped entry -- nobody would have started them and nobody could reach them any more.
            /// The bytes are charged to the stage the planner queued the entry under, exactly as the
            /// pump charges them, so which of the two issued the read doesn't change the accounting.
            const ReadStage saved_stage = std::exchange(diff.cur_stage, it->stage);
            reader.prefetcher.startPrefetch(it->handles, &diff);
            diff.cur_stage = saved_stage;
            it = issue_queue.erase(it);
        }
        else
            ++it;
    }
}

void ReadManager::dropQueuedReads(size_t row_group_idx)
{
    std::lock_guard lock(issue_mutex);
    std::erase_if(issue_queue, [&](const PlannedRead & p) { return p.row_group_idx == row_group_idx; });
}

bool ReadManager::hasQueuedReads(ReadStage stage, size_t row_group_idx, size_t row_subgroup_idx, size_t step_idx)
{
    std::lock_guard lock(issue_mutex);
    return std::any_of(issue_queue.begin(), issue_queue.end(), [&](const PlannedRead & p)
    {
        return p.stage == stage && p.row_group_idx == row_group_idx &&
            p.row_subgroup_idx == row_subgroup_idx && p.step_idx == step_idx;
    });
}

size_t ReadManager::bytesInFlightTarget() const
{
    /// `io_threads` is the size of the pool that actually executes the reads (0 before it's created).
    size_t io_threads = std::max<size_t>(1, parser_shared_resources->io_threads.load(std::memory_order_relaxed));
    return std::max(
        reader.prefetcher.targetBytesInFlight(io_threads),
        reader.options.format.parquet.min_bytes_in_flight);
}

bool ReadManager::isPrivilegedRead(const PlannedRead & planned) const
{
    /// Only the reads that the reader cannot make progress without bypass the budgets, and only for
    /// the row group that has to be delivered next:
    ///  * its index reads (`row_subgroup_idx == UINT64_MAX`), which are small and are the
    ///    prerequisite of everything else in the row group;
    ///  * the data pages of the one subgroup it is about to read (`read_ptr`).
    /// The other subgroups' pages (which `enqueueRowGroupPageReads` plans all at once) obey both the
    /// bytes-in-flight target and the memory pool cap: read-ahead must not be able to put a whole row
    /// group's compressed data in flight past `input_format_parquet_memory_high_watermark`. Bypassing
    /// isn't needed for them anyway -- the demand path (`takeQueuedReads` in `scheduleTask`) takes a
    /// subgroup's planned reads out of the queue and starts them itself when the subgroup is admitted,
    /// so no subgroup can ever be stuck waiting for the pump.
    if (planned.row_group_idx != first_incomplete_row_group.load(std::memory_order_relaxed))
        return false;
    if (planned.row_subgroup_idx == UINT64_MAX)
        return true;
    return planned.row_subgroup_idx == reader.row_groups[planned.row_group_idx].read_ptr.load();
}

void ReadManager::pumpIssueQueue(MemoryUsageDiff & diff)
{
    {
        /// Cheap early-out: this runs on every `flushMemoryUsageDiff`. Also the whole of the
        /// disabled case (`input_format_parquet_min_bytes_in_flight = 0`), where nothing is planned.
        std::lock_guard lock(issue_mutex);
        if (issue_queue.empty())
            return;
    }

    const size_t target = bytesInFlightTarget();
    bool blocked = false;

    while (true)
    {
        /// Held across `startPrefetch` so that `dropQueuedReads` can rely on the queue being the only
        /// way this function reaches a row group's handles.
        std::lock_guard lock(issue_mutex);

        auto it = issue_queue.begin();
        if (it == issue_queue.end())
            break;

        if (!isPrivilegedRead(*it))
        {
            const MemoryPool pool = poolOf(it->stage);
            /// `pool_usage` doesn't include what this diff charged and hasn't flushed yet, so add it.
            ssize_t pool_pending = 0;
            for (size_t i = 0; i < diff.by_stage.size(); ++i)
                if (i != size_t(ReadStage::Deliver) && poolOf(ReadStage(i)) == pool)
                    pool_pending += diff.by_stage[i];
            size_t pool_usage_now = size_t(std::max<ssize_t>(0,
                pool_usage[size_t(pool)].load(std::memory_order_relaxed) + pool_pending));

            /// `bytes` is the sum of the requested ranges' lengths, while the Prefetcher may coalesce
            /// them into tasks that also span the gaps in between (and may serve some of them from
            /// already-read ranges), so the bytes actually in flight for an entry can differ from
            /// `bytes` in either direction -- the effective read depth is usually a bit deeper than
            /// this target nominally allows. That's fine: the target is a fitted goal, not a limit
            /// anything depends on.
            /// The pool cap below is the one that must hold, and it holds only approximately for the
            /// same reason: `PlannedRead::bytes` sums the requested lengths, while the tokens actually
            /// charged to the pool are `request->length` times the task's `memory_amplification` (the
            /// coalesced span divided by the useful bytes in it). So one entry can overshoot the cap by
            /// up to its own size times that amplification, which `input_format_parquet_max_read_amplification`
            /// bounds; the overshoot is at most one entry's worth because the next iteration sees the
            /// real usage.
            if (reader.prefetcher.bytesInFlight() + it->bytes > target ||
                pool_usage_now + it->bytes > poolLimits(pool).memory_high_watermark)
            {
                blocked = true;
                /// A privileged entry isn't necessarily at the front of the queue -- a later row
                /// group's index reads are planned before an earlier row group's page reads -- so
                /// look for one before giving up.
                it = std::find_if(issue_queue.begin(), issue_queue.end(),
                    [&](const PlannedRead & p) { return isPrivilegedRead(p); });
                if (it == issue_queue.end())
                    break;
            }
        }

        /// Issue first, erase after: if `startPrefetch` throws, the entry stays in the queue, so its
        /// handles are still reachable (for `dropQueuedReads`, and for a later pump to retry) instead
        /// of being lost together with the popped entry. `issue_mutex` is held throughout, so `it`
        /// stays valid across the call.
        const ReadStage saved_stage = std::exchange(diff.cur_stage, it->stage);
        reader.prefetcher.startPrefetch(it->handles, &diff);
        diff.cur_stage = saved_stage;
        issue_queue.erase(it);
        ProfileEvents::increment(ProfileEvents::ParquetPlannedReads);
    }

    /// One event per pump that ran into a full budget (not one per entry it looked at), so the count
    /// reads as "times read-ahead had to wait", not as a function of how deep the queue happens to be.
    /// Counted even if the pump then issued a privileged entry: read-ahead was still blocked, which is
    /// what the event is about.
    if (blocked)
        ProfileEvents::increment(ProfileEvents::ParquetIssueQueueStalls);
}

static bool checkTaskSchedulingLimits(size_t memory_usage, size_t added_memory, size_t batches_in_progress, size_t added_tasks, const SharedResourcesExt::Limits & limits)
{
    if (added_tasks == 0)
    {
        return memory_usage < limits.memory_low_watermark ||
            (memory_usage <= limits.memory_high_watermark && batches_in_progress < limits.parsing_threads);
    }
    else
    {
        /// If we're going to pay the cost of adding tasks to the queue, prefer to add many at once.
        return added_memory < limits.memory_low_watermark ||
               (memory_usage + added_memory <= limits.memory_high_watermark &&
                added_tasks < limits.parsing_threads);
    }
}

void ReadManager::flushMemoryUsageDiff(MemoryUsageDiff && diff)
{
    chassert(!diff.finalized);
    diff.finalized = true;

    /// Stages to call scheduleTasksIfNeeded for, decided below. Collected into a bitmask (instead
    /// of calling scheduleTasksIfNeeded eagerly per stage) for two reasons: (1) `pool_usage` should
    /// reflect the whole diff before we make any scheduling decision, and (2) a pool is shared by
    /// several stages (see `poolOf(ReadStage)`), so freeing memory charged to one stage can unblock a *different*
    /// stage on the same pool -- we want to wake all of them exactly once, not just the one whose
    /// own by_stage went negative.
    UInt64 stages_to_schedule = diff.stages_to_schedule;

    for (size_t i = 0; i < diff.by_stage.size(); ++i)
    {
        ssize_t d = diff.by_stage[i];
        if (i == size_t(ReadStage::Deliver))
        {
            chassert(d == 0);
            continue;
        }
        MemoryPool pool = poolOf(ReadStage(i));
        if (d != 0)
        {
            pool_usage[size_t(pool)].fetch_add(d, std::memory_order_relaxed);
        }

        bool already_scheduled = (stages_to_schedule & (1ull << i)) != 0;
        if (!already_scheduled && d < 0)
        {
            const auto & stage = stages[i];
            auto limits = poolLimits(pool);
            limits.parsing_threads = SharedResourcesExt::getLimitsPerReader(*parser_shared_resources, 1.0, stage.thread_target_fraction).parsing_threads;
            size_t memory_usage = size_t(std::max<ssize_t>(0, pool_usage[size_t(pool)].load(std::memory_order_relaxed)));
            if (pool == MemoryPool::Decoded)
                memory_usage += size_t(std::max<ssize_t>(0, delivered_bytes->load(std::memory_order_relaxed)));
            bool should_schedule = checkTaskSchedulingLimits(
                memory_usage, 0,
                stage.batches_in_progress.load(std::memory_order_relaxed), 0, limits);
            if (should_schedule)
            {
                for (size_t j = 0; j < diff.by_stage.size(); ++j)
                    if (j != size_t(ReadStage::Deliver) && poolOf(ReadStage(j)) == pool)
                        stages_to_schedule |= (1ull << j);
            }
        }
    }

    /// Every flush is a chance to issue more planned reads: reads landing and pages being decoded
    /// free bytes in flight and pool memory, and completed row groups advance
    /// `first_incomplete_row_group`, which is what makes the next row group's reads privileged.
    /// (Unconditional rather than only on a Metadata/Compressed deallocation: a flush that only
    /// frees Decoded memory can still be the one that advanced `first_incomplete_row_group`, and
    /// missing that wakeup can leave the queue stalled with nothing else coming to nudge it.
    /// `pumpIssueQueue` returns immediately when the queue is empty, which is the common case.)
    /// A second diff, because `pumpIssueQueue` must not call this function (recursion); it only
    /// allocates, so applying it to `pool_usage` here is all that's needed -- `startPrefetch`
    /// schedules no tasks.
    MemoryUsageDiff pump_diff(ReadStage::ColumnDataPrefetch);
    pumpIssueQueue(pump_diff);
    pump_diff.finalized = true;
    for (size_t i = 0; i < pump_diff.by_stage.size(); ++i)
    {
        chassert(pump_diff.by_stage[i] >= 0); // pumpIssueQueue doesn't do tracked deallocations
        if (pump_diff.by_stage[i] != 0)
        {
            chassert(i != size_t(ReadStage::Deliver));
            pool_usage[size_t(poolOf(ReadStage(i)))].fetch_add(pump_diff.by_stage[i], std::memory_order_relaxed);
        }
    }

    /// Deliver (and anything at/after it) is never schedulable -- scheduleTasksIfNeeded asserts
    /// stage_idx < Deliver -- so stop short of it even though scheduleAllStages() sets every bit.
    for (size_t i = 0; i < size_t(ReadStage::Deliver); ++i)
        if ((stages_to_schedule & (1ull << i)) != 0)
            scheduleTasksIfNeeded(ReadStage(i));
}

void ReadManager::scheduleTasksIfNeeded(ReadStage stage_idx)
{
    chassert(stage_idx < ReadStage::Deliver);

    Stage & stage = stages.at(size_t(stage_idx));
    MemoryUsageDiff diff(stage_idx);
    std::vector<Task> tasks;

    auto limits = poolLimits(poolOf(stage_idx));
    limits.parsing_threads = SharedResourcesExt::getLimitsPerReader(*parser_shared_resources, 1.0, stage.thread_target_fraction).parsing_threads;
    size_t memory_usage = size_t(std::max<ssize_t>(0, pool_usage[size_t(poolOf(stage_idx))].load(std::memory_order_relaxed)));
    if (poolOf(stage_idx) == MemoryPool::Decoded)
        memory_usage += size_t(std::max<ssize_t>(0, delivered_bytes->load(std::memory_order_relaxed)));
    size_t batches_in_progress = stage.batches_in_progress.load(std::memory_order_relaxed);

    LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: stage={} memory_usage={} batches_in_progress={} limits: mem_low={} mem_high={} threads={}",
              magic_enum::enum_name(stage_idx), memory_usage, batches_in_progress,
              limits.memory_low_watermark, limits.memory_high_watermark, limits.parsing_threads);
    /// Need to be careful to avoid getting deadlocked in a situation where tasks can't be scheduled
    /// because memory usage is high, while memory usage can't decrease because tasks can't be scheduled.
    /// The way we prevent it is by always allowing scheduling tasks for the lowest-numbered
    /// <row group, row subgroup> pair that hasn't been completed (delivered or skipped) yet.
    /// Below ColumnData, a row group is privileged unconditionally (not just when read_ptr ==
    /// delivery_ptr): pools are shared across several `ReadStage`s (see `poolOf(ReadStage)`), so memory held by
    /// one metadata stage (e.g. dictionary-page prefetch, released only in ColumnData) can block a
    /// *different* metadata stage the lowest incomplete row group still needs to pass through to
    /// ever reach ColumnData. Without this, that row group -- and thus the whole pool, since nothing
    /// downstream can free it -- could get stuck forever. Once in ColumnData or later, we go back to
    /// requiring read_ptr == delivery_ptr, to avoid over-admitting decode work ahead of delivery.
    auto is_privileged_task = [&](size_t row_group_idx)
    {
        size_t i = first_incomplete_row_group.load();
        if (row_group_idx != i)
            return false;
        const RowGroup & row_group = reader.row_groups[row_group_idx];
        /// Must check stage first so that read_ptr is meaningful (we start advancing it in finishRowSubgroupStage).
        /// Using acquire ordering to synchronize with the release (seq_cst) store in `finishRowGroupStage`.
        if (row_group.stage.load(std::memory_order_acquire) < ReadStage::ColumnData)
            return true;
        return row_group.read_ptr.load() == row_group.delivery_ptr.load();
    };

    while (true)
    {
        auto row_group_maybe = stage.schedulable_row_groups.findFirst();
        if (!row_group_maybe.has_value())
        {
            LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: stage={} no schedulable row groups",
                      magic_enum::enum_name(stage_idx));
            break;
        }
        size_t row_group_idx = *row_group_maybe;
        bool can_schedule = checkTaskSchedulingLimits(
                memory_usage, size_t(diff.by_stage[size_t(stage_idx)]),
                batches_in_progress, tasks.size(), limits);
        bool is_privileged = is_privileged_task(row_group_idx);
        LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: stage={} rg={} can_schedule={} is_privileged={}",
                  magic_enum::enum_name(stage_idx), row_group_idx, can_schedule, is_privileged);

        if (!can_schedule && !is_privileged)
            break;

        if (!stage.schedulable_row_groups.unset(row_group_idx, std::memory_order_acquire))
        {
            LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: another thread got row group {}", row_group_idx);
            continue; // another thread picked up this row group while we were checking limits
        }

        /// Kicks off prefetches and adds their (and other) memory usage estimate to `diff`.
        auto & stage_tasks = stage.row_group_tasks_to_schedule[row_group_idx];
        LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: want to schedule tasks rg={}, stage_tasks.size()={}", row_group_idx, stage_tasks.size());
        chassert(!stage_tasks.empty());
        for (size_t i = 0; i < stage_tasks.size(); ++i)
            scheduleTask(stage_tasks[i], i == 0, diff, tasks);
        stage_tasks.clear();
    }

    chassert(!diff.finalized);
    diff.finalized = true;
    for (size_t i = 0; i < diff.by_stage.size(); ++i)
    {
        chassert(diff.by_stage[i] >= 0); // scheduleTask doesn't do tracked deallocations
        if (diff.by_stage[i] != 0)
        {
            chassert(i != size_t(ReadStage::Deliver));
            pool_usage[size_t(poolOf(ReadStage(i)))].fetch_add(diff.by_stage[i], std::memory_order_relaxed);
        }
    }

    if (!tasks.empty())
    {
        LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: stage={} scheduling {} tasks in {} batches",
                  magic_enum::enum_name(stage_idx), tasks.size(), std::min(tasks.size(), limits.parsing_threads) + 1);

        /// Group tiny tasks into batches to reduce scheduling overhead.
        /// TODO [parquet]: Try removing this (along with cost_estimate_bytes field).
        std::vector<std::function<void()>> funcs;
        funcs.reserve(std::min(tasks.size(), limits.parsing_threads) + 1);
        size_t bytes_per_batch = size_t(diff.by_stage[size_t(stage_idx)]) / limits.parsing_threads;
        size_t tasks_per_batch = tasks.size() / limits.parsing_threads;
        size_t i = 0;
        while (i < tasks.size())
        {
            size_t bytes = 0;
            size_t n = 0;
            std::vector<Task> batch;
            while (i < tasks.size() && bytes <= bytes_per_batch && n <= tasks_per_batch)
            {
                batch.push_back(tasks[i]);
                bytes += tasks[i].cost_estimate_bytes;
                n += 1;
                ++i;
            }
            funcs.push_back([this, _batch = std::move(batch), _shutdown = shutdown]
            {
                std::shared_lock shutdown_lock(*_shutdown, std::try_to_lock);
                if (!shutdown_lock.owns_lock())
                    /// ReadManager may already be destroyed at this point — the destructor
                    /// calls shutdown->shutdown() which only waits for in-flight tasks (shared
                    /// lock holders), not for queued tasks. Accessing `this` here would be a
                    /// use-after-free. The batches_in_progress decrement is unnecessary because:
                    /// - In normal completion (read() calls shutdown), no tasks are queued
                    ///   (all row groups reached Deallocated before shutdown was called).
                    /// - In abnormal destruction (~ReadManager), nobody checks the counter.
                    return;
                runBatchOfTasks(_batch);
            });
        }
        stage.batches_in_progress.fetch_add(funcs.size(), std::memory_order_relaxed);
        ProfileEvents::increment(ProfileEvents::ParquetDecodingTasks, tasks.size());
        ProfileEvents::increment(ProfileEvents::ParquetDecodingTaskBatches, funcs.size());
        parser_shared_resources->parsing_runner.bulkSchedule(std::move(funcs));
    }
    else
    {
        LOG_TEST(getLogger("ParquetReadManager"), "scheduleTasksIfNeeded: stage={} no tasks to schedule",
                  magic_enum::enum_name(stage_idx));
    }
}

void ReadManager::scheduleTask(Task task, bool is_first_in_group, MemoryUsageDiff & diff, std::vector<Task> & out_tasks)
{
    LOG_TEST(getLogger("ParquetReadManager"), "scheduleTask: schedule task.row_group_idx={}, task.row_subgroup_idx={}, task.stage={}, task.column_idx={}, task.step_idx={}", task.row_group_idx, task.row_subgroup_idx, static_cast<Int32>(task.stage), task.column_idx, task.step_idx);

    /// Kick off prefetches and count estimated memory usage.
    std::vector<PrefetchHandle *> prefetches;
    RowGroup & row_group = reader.row_groups[task.row_group_idx];
    ssize_t memory_before = diff.by_stage[size_t(diff.cur_stage)];
    if (task.column_idx != UINT64_MAX)
    {
        ColumnChunk & column = row_group.columns.at(task.column_idx);
        switch (task.stage)
        {
            case ReadStage::BloomFilterHeader:
                /// This stage's tasks read (and then reset) the handles the planner queued for the
                /// whole row group at init, so take that entry rather than leave it for the pump.
                takeQueuedReads(ReadStage::BloomFilterHeader, task.row_group_idx, UINT64_MAX, 0, diff);
                prefetches.push_back(&column.bloom_filter_header_prefetch);
                break;
            case ReadStage::BloomFilterBlocksOrDictionary:
                takeQueuedReads(ReadStage::BloomFilterBlocksOrDictionary, task.row_group_idx, UINT64_MAX, 0, diff);
                if (column.use_dictionary_filter)
                    prefetches.push_back(&column.dictionary_page_prefetch);
                for (auto & b : column.bloom_filter_blocks)
                    prefetches.push_back(&b.prefetch);
                break;
            case ReadStage::ColumnIndexAndOffsetIndex:
            {
                takeQueuedReads(ReadStage::ColumnIndexAndOffsetIndex, task.row_group_idx, UINT64_MAX, 0, diff);
                prefetches.push_back(&column.column_index_prefetch);
                prefetches.push_back(&column.offset_index_prefetch);
                break;
            }
            case ReadStage::OffsetIndex:
                /// The offset index handles live in the row-group-level index entry (the planner puts
                /// both indexes of every column there), and this stage resets them after decoding.
                takeQueuedReads(ReadStage::ColumnIndexAndOffsetIndex, task.row_group_idx, UINT64_MAX, 0, diff);
                prefetches.push_back(&column.offset_index_prefetch);
                break;
            case ReadStage::ColumnDataPrefetch:
            {
                RowSubgroup & row_subgroup = row_group.subgroups.at(task.row_subgroup_idx);
                if (row_subgroup.filter.rows_pass == 0)
                {
                    /// Returning without draining the queue is only safe because a subgroup that has
                    /// no rows here never had a planned entry in the first place: the planner skips
                    /// `rows_pass == 0` subgroups, and `rows_pass` can only be zeroed later, by
                    /// `applyPrewhere` at a step after the one the planner plans (the first). This is
                    /// the invariant that makes `ColumnChunk::dictionary_and_whole_chunk_planned`
                    /// safe: the subgroup that claimed the column's dictionary and whole-chunk
                    /// handles always reaches this stage and drains them.
                    chassert(!hasQueuedReads(ReadStage::ColumnDataPrefetch, task.row_group_idx, task.row_subgroup_idx, task.step_idx));
                    break;
                }
                /// This subgroup's data-page reads were usually planned when the row group's offset
                /// indexes landed (`enqueueRowGroupPageReads`) and issued by the pump long before
                /// this task. Take whatever the pump hasn't got to yet out of the queue and issue it
                /// here: this task is the demand path, so it must not wait for the budget, and the
                /// decoder that follows requires every page handle it uses to have been started.
                takeQueuedReads(ReadStage::ColumnDataPrefetch, task.row_group_idx, task.row_subgroup_idx, task.step_idx, diff);
                /// Queue this subgroup's data-page reads; startPrefetch (below) issues them and charges
                /// compressed bytes to the ColumnDataPrefetch budget, separate from the decode budget,
                /// so many row groups prefetch ahead while only a few decode at once.
                /// (A no-op for a column the planner already walked: it advanced the page cursor.)
                reader.determinePagesToPrefetch(column, row_subgroup, row_group, prefetches);

                /// Side note: would be nice to avoid reading the dictionary if all dictionary-encoded
                /// pages were filtered out (e.g. if it's a 100 MB column chunk with unique long strings,
                /// typically only the first ~1 MB would be dictionary-encoded; if we only need a few
                /// rows, we likely won't hit that 1 MB). But AFAICT parquet metadata doesn't have
                /// enough information for that (there's no page encoding in offset/column indexes).
                /// The planner claims these two handles for the whole column chunk when it plans the
                /// column's pages (see `dictionary_and_whole_chunk_planned`); pushing them here as
                /// well could have two threads start the same handle at once.
                if (!column.dictionary_and_whole_chunk_planned)
                {
                    if (!column.dictionary.isInitialized() && column.dictionary_page_prefetch)
                    {
                        prefetches.push_back(&column.dictionary_page_prefetch);
                    }

                    if (column.data_pages.empty())
                    {
                        prefetches.push_back(&column.data_pages_prefetch);
                    }
                }
                break;
            }
            case ReadStage::ColumnData:
            {
                RowSubgroup & row_subgroup = row_group.subgroups.at(task.row_subgroup_idx);
                ColumnSubchunk & subchunk = row_subgroup.columns.at(task.column_idx);
                if (row_subgroup.filter.rows_pass == 0)
                    break;
                /// Reads already issued in ColumnDataPrefetch; here just reserve estimated decoded-output
                /// memory against the ColumnData budget (runTask decodes from those buffers).
                double bytes_per_row = reader.estimateColumnMemoryBytesPerRow(column, row_group, reader.primitive_columns.at(task.column_idx));
                size_t column_memory = static_cast<size_t>(bytes_per_row * static_cast<double>(row_subgroup.filter.rows_pass));
                subchunk.column_and_offsets_memory = MemoryUsageToken(column_memory, &diff);
                break;
            }
            case ReadStage::NotStarted:
            case ReadStage::Deliver:
            case ReadStage::Deallocated:
                chassert(false);
                break;
        }
    }

    if (task.stage == ReadStage::ColumnData && is_first_in_group)
    {
        RowSubgroup & row_subgroup = row_group.subgroups.at(task.row_subgroup_idx);
        /// If we're reusing filter.memory for a new step (multistage prewhere), free the old memory first.
        if (!row_subgroup.filter.memory)
            row_subgroup.filter.memory = MemoryUsageToken(row_subgroup.filter.rows_total, &diff);
    }

    reader.prefetcher.startPrefetch(prefetches, &diff);

    /// Group tiny tasks to reduce scheduling overhead, using predicted memory as a proxy for run time.
    /// Exception: ColumnDataPrefetch does its work (startPrefetch) here and has an empty runTask, so
    /// its run time is ~0 no matter how many compressed bytes it charges; report cost 0 so these tasks
    /// collapse into one batch instead of being split across many no-op thread-pool dispatches.
    ssize_t memory_after = diff.by_stage[size_t(diff.cur_stage)];
    task.cost_estimate_bytes = task.stage == ReadStage::ColumnDataPrefetch
        ? 0
        : size_t(std::max(0l, memory_after - memory_before));

    out_tasks.push_back(task);
}

void ReadManager::runBatchOfTasks(const std::vector<Task> & tasks) noexcept
{
    ReadStage stage = tasks.at(0).stage;
    size_t column_idx = UINT64_MAX;

    std::exception_ptr exc;
    try
    {
        MemoryUsageDiff diff(stage);
        for (size_t i = 0; i < tasks.size(); ++i)
        {
            chassert(tasks[i].stage == stage);
            column_idx = tasks[i].column_idx;

            runTask(tasks[i], i + 1 == tasks.size(), diff);
        }
        flushMemoryUsageDiff(std::move(diff));
    }
    catch (DB::Exception & e)
    {
        e.addMessage("read stage: {}", magic_enum::enum_name(stage));
        if (column_idx != UINT64_MAX)
            e.addMessage("column: {}", reader.primitive_columns[column_idx].name);
        exc = std::current_exception();
    }
    catch (...)
    {
        exc = std::current_exception();
    }
    if (exc)
    {
        {
            std::lock_guard lock(delivery_mutex);
            exception = exc;
        }
        delivery_cv.notify_all();
    }
}

void ReadManager::runTask(Task task, bool last_in_batch, MemoryUsageDiff & diff)
{
    RowGroup & row_group = reader.row_groups.at(task.row_group_idx);
    if (task.column_idx != UINT64_MAX)
    {
        ColumnChunk & column = row_group.columns.at(task.column_idx);
        const PrimitiveColumnInfo & column_info = reader.primitive_columns.at(task.column_idx);

        switch (task.stage)
        {
            case ReadStage::BloomFilterHeader: /// TODO [parquet]: do all columns in one task
                reader.processBloomFilterHeader(column, column_info);
                column.bloom_filter_header_prefetch.reset(&diff);
                break;
            case ReadStage::BloomFilterBlocksOrDictionary:
                if (column.use_dictionary_filter)
                {
                    bool ok = reader.decodeDictionaryPage(column, column_info);  /// NOLINT(clang-analyzer-deadcode.DeadStores)
                    chassert(ok);
                }
                break;
            case ReadStage::ColumnIndexAndOffsetIndex:
                reader.decodeOffsetIndex(column, row_group);
                column.offset_index_prefetch.reset(&diff);
                reader.applyColumnIndex(column, column_info, row_group);
                column.column_index_prefetch.reset(&diff);
                break;
            case ReadStage::OffsetIndex:
                reader.decodeOffsetIndex(column, row_group);
                column.offset_index_prefetch.reset(&diff);
                break;
            case ReadStage::ColumnDataPrefetch:
                /// Reads were issued in scheduleTask (startPrefetch) and run async in the Prefetcher;
                /// nothing to do here. The subgroup advances to ColumnData, which decodes them.
                break;
            case ReadStage::ColumnData:
            {
                RowSubgroup & row_subgroup = row_group.subgroups.at(task.row_subgroup_idx);
                if (row_subgroup.filter.rows_pass == 0)
                    break;
                if (!column.dictionary.isInitialized() && column.dictionary_page_prefetch)
                {
                    if (!reader.decodeDictionaryPage(column, column_info))
                    {
                        column.dictionary_page_prefetch.reset(&diff);
                    }
                }
                size_t prev_page_idx = column.data_pages_idx;

                chassert(task.row_subgroup_idx != UINT64_MAX);
                reader.decodePrimitiveColumn(
                    column, column_info, row_subgroup.columns.at(task.column_idx),
                    row_group, row_subgroup, diff);

                for (size_t i = prev_page_idx; i < column.data_pages_idx; ++i)
                {
                    column.data_pages.at(i).prefetch.reset(&diff);
                }
                break;
            }
            case ReadStage::NotStarted:
            case ReadStage::Deliver:
            case ReadStage::Deallocated:
                chassert(false);
                break;
        }
    }

    if (last_in_batch)
    {
        /// Decrement it before scheduling more tasks.
        size_t prev_batches_in_progress = stages.at(size_t(task.stage)).batches_in_progress.fetch_sub(1, std::memory_order_relaxed);  /// NOLINT(clang-analyzer-deadcode.DeadStores)
        chassert(prev_batches_in_progress > 0);
        diff.scheduleStage(task.stage);
    }

    if (task.row_subgroup_idx != UINT64_MAX)
    {
        size_t remaining = row_group.subgroups.at(task.row_subgroup_idx).stage_tasks_remaining.fetch_sub(1);
        chassert(remaining > 0);
        if (remaining == 1)
            finishRowSubgroupStage(task.row_group_idx, task.row_subgroup_idx, task.stage, task.step_idx, diff);
    }
    else
    {
        size_t remaining = row_group.stage_tasks_remaining.fetch_sub(1);
        chassert(remaining > 0);
        if (remaining == 1)
            finishRowGroupStage(task.row_group_idx, task.stage, diff);
    }
}

void ReadManager::clearColumnChunk(ColumnChunk & column, MemoryUsageDiff & diff)
{
    /// Many of these are usually cleared after the corresponding stages, but we clear them here too
    /// because stages can be skipped e.g. if the row group was filtered out by bloom filter.

    column.data_pages_prefetch.reset(&diff);
    column.dictionary.reset();
    for (auto & page : column.data_pages)
        page.prefetch.reset(&diff);
    column.bloom_filter_header_prefetch.reset(&diff);
    column.bloom_filter_data_prefetch.reset(&diff);
    column.dictionary_page_prefetch.reset(&diff);
    column.column_index_prefetch.reset(&diff);
    column.offset_index_prefetch.reset(&diff);
    column.data_pages_prefetch.reset(&diff);
    for (auto & block : column.bloom_filter_blocks)
        block.prefetch.reset(&diff);

    column = {};
}

void ReadManager::clearRowSubgroup(RowSubgroup & row_subgroup, MemoryUsageDiff & diff)
{
    row_subgroup.filter.clear(&diff);
    row_subgroup.output.clear();
    for (ColumnSubchunk & col : row_subgroup.columns)
        col.column_and_offsets_memory.reset(&diff);
}

std::string ReadManager::collectDeadlockDiagnostics()
{
    std::string result;

    result += " first_inc_rg: " + std::to_string(first_incomplete_row_group.load(std::memory_order_relaxed)) + " ";
    {
        std::lock_guard lock(delivery_mutex);
        result += " delivery_queue.size(): " + std::to_string(delivery_queue.size());
    }
    result += " tot_rgs: " + std::to_string(reader.row_groups.size());

    result += " pools:";
    for (size_t p = 0; p < NUM_MEMORY_POOLS; ++p)
        result += " " + std::string(magic_enum::enum_name(MemoryPool(p))) + "=" + std::to_string(pool_usage[p].load(std::memory_order_relaxed));
    result += " delivered_bytes=" + std::to_string(delivered_bytes->load(std::memory_order_relaxed));

    {
        std::lock_guard lock(issue_mutex);
        size_t queued_bytes = 0;
        for (const PlannedRead & p : issue_queue)
            queued_bytes += p.bytes;
        result += " issue_queue: " + std::to_string(issue_queue.size()) + " reads, " + std::to_string(queued_bytes) + " bytes";
    }
    result += " bytes_in_flight: " + std::to_string(reader.prefetcher.bytesInFlight()) +
        "/" + std::to_string(bytesInFlightTarget());

    result += " stages: ";
    for (size_t i = 0; i < size_t(ReadStage::Deallocated); ++i)
    {
        const auto & stage = stages[i];
        size_t schedulable_count = 0;
        for (const auto & atomic_bits : stage.schedulable_row_groups.a)
        {
            UInt64 bits = atomic_bits.load(std::memory_order_relaxed);
            schedulable_count += __builtin_popcountll(bits);
        }
        result += " st " + std::to_string(i) + " (" + std::string(magic_enum::enum_name(ReadStage(i))) + "):";
        result += " btch: " + std::to_string(stage.batches_in_progress.load(std::memory_order_relaxed));
        result += " rgs_sch: " + std::to_string(schedulable_count) + "\t";
        size_t tasks_to_schedule = 0;
        for (const auto & tasks : stage.row_group_tasks_to_schedule)
            tasks_to_schedule += tasks.size();
        result += " tasks_to_sch: " + std::to_string(tasks_to_schedule) + "\t";
    }

    result += " RGs: ";
    for (size_t rg_idx = 0; rg_idx < reader.row_groups.size(); ++rg_idx)
    {
        const auto & row_group = reader.row_groups[rg_idx];
        result += " rg[" + std::to_string(rg_idx) + "]: ";
        result += " st: " + std::string(magic_enum::enum_name(row_group.stage.load(std::memory_order_relaxed)));
        result += " del_ptr: " + std::to_string(row_group.delivery_ptr.load(std::memory_order_relaxed)) + "/" + std::to_string(row_group.subgroups.size());
        result += " read_ptr: " + std::to_string(row_group.read_ptr.load(std::memory_order_relaxed));
        result += " ";

        size_t subgroups_in_progress = 0;
        size_t subgroups_delivered = 0;
        size_t subgroups_deallocated = 0;
        size_t subgroups_not_started = 0;
        for (const auto & subgroup : row_group.subgroups)
        {
            ReadStage sg_stage = subgroup.stage.load(std::memory_order_relaxed);
            if (sg_stage == ReadStage::Deliver)
                subgroups_delivered++;
            else if (sg_stage == ReadStage::Deallocated)
                subgroups_deallocated++;
            else if (sg_stage >= ReadStage::OffsetIndex)
                subgroups_in_progress++;
            else
                subgroups_not_started++;
        }
        result += " subrgs: ns=" + std::to_string(subgroups_not_started) +
                  ", in_pr=" + std::to_string(subgroups_in_progress) +
                  ", deliv=" + std::to_string(subgroups_delivered) +
                  ", deal=" + std::to_string(subgroups_deallocated) + " ";
    }

    return result;
}

ReadManager::ReadResult ReadManager::read()
{
    Task task;
    {
        std::unique_lock lock(delivery_mutex);

        while (true)
        {
            bool thread_pool_was_idle = parser_shared_resources->parsing_runner.isIdle();

            if (exception)
                std::rethrow_exception(copyMutableException(exception));

            /// If `preserve_order`, only deliver chunks from `first_incomplete_row_group`.
            /// This ensures that row groups are delivered in order. Within a row group, row
            /// subgroups are read and added to `delivery_queue` in order.
            size_t first_inc = first_incomplete_row_group.load(std::memory_order_relaxed);
            bool can_deliver = !delivery_queue.empty() &&
                (!reader.options.format.parquet.preserve_order ||
                 delivery_queue.top().row_group_idx == first_inc);

            LOG_TEST(getLogger("ParquetReadManager"), "read: delivery_queue.size()={} first_incomplete={} thread_pool_idle={} can_deliver={}",
                      delivery_queue.size(), first_inc, thread_pool_was_idle, can_deliver);

            if (can_deliver)
            {
                task = delivery_queue.top();
                delivery_queue.pop();
                LOG_TEST(getLogger("ParquetReadManager"), "read: delivering task rg={} sg={}",
                          task.row_group_idx, task.row_subgroup_idx);
                break;
            }

            if (first_incomplete_row_group.load(std::memory_order_relaxed) == reader.row_groups.size())
            {
                /// All done. Check for memory accounting leaks.
                /// First join the threads because they might still be decrementing memory_usage.
                lock.unlock();
                shutdown->shutdown();
                lock.lock();

                /// Memory is tracked per `MemoryPool`, not per stage (see `poolOf(ReadStage)`); check each pool once.
                for (size_t p = 0; p < NUM_MEMORY_POOLS; ++p)
                {
                    ssize_t mem = pool_usage[p].load(std::memory_order_relaxed);
                    if (mem != 0)
                        throw Exception(ErrorCodes::LOGICAL_ERROR, "Leak in memory accounting in parquet reader: got {} bytes in pool {}", mem, magic_enum::enum_name(MemoryPool(p)));
                }

                for (const RowGroup & row_group : reader.row_groups)
                {
                    chassert(row_group.stage.load(std::memory_order_relaxed) == ReadStage::Deallocated);
                    chassert(row_group.delivery_ptr.load(std::memory_order_relaxed) == row_group.subgroups.size());
                    for (const RowSubgroup & subgroup : row_group.subgroups)
                        chassert(subgroup.stage.load(std::memory_order_relaxed) == ReadStage::Deallocated);
                    for (size_t i = 0; i < stages.size(); ++i)
                    {
                        size_t batches = stages[i].batches_in_progress.load(std::memory_order_relaxed);
                        size_t unsched = 0;
                        for (const auto & tasks : stages[i].row_group_tasks_to_schedule)
                            unsched += tasks.size();
                        if (batches != 0 || unsched != 0)
                            throw Exception(ErrorCodes::LOGICAL_ERROR, "Leak in task accounting in parquet reader: got {} batches, {} tasks in stage {}", batches, unsched, magic_enum::enum_name(ReadStage(i)));
                    }
                }
                return {};
            }

            if (parser_shared_resources->parsing_runner.isManual())
            {
                /// Pump the manual executor.
                lock.unlock();
                /// Note: the executor can be shared among multiple files, so we may execute someone
                /// else's task, and someone else may execute our task.
                /// Hence the thread_pool_was_idle check.
                if (!parser_shared_resources->parsing_runner.runTaskInline() && thread_pool_was_idle)
                {
                    std::string diagnostics = collectDeadlockDiagnostics();
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "Deadlock in Parquet::ReadManager (single-threaded). {}", diagnostics);
                }
                lock.lock();
            }
            else if (thread_pool_was_idle)
            {
                /// Task scheduling code is complicated and error-prone. In particular it's easy to
                /// have a bug where tasks stop getting scheduled under some conditions
                /// (see is_privileged_task). So we specifically check for getting stuck.
                LOG_DEBUG(getLogger("ParquetReadManager"), "read: DEADLOCK DETECTED - thread pool idle, collecting diagnostics");
                lock.unlock();
                std::string diagnostics = collectDeadlockDiagnostics();
                LOG_DEBUG(getLogger("ParquetReadManager"), "read: DEADLOCK diagnostics: {}", diagnostics);
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Deadlock in Parquet::ReadManager (thread pool).");
            }
            else
            {
                /// Wait for progress. Re-check parsing_runner.isIdle() every few seconds.
                delivery_cv.wait_for(lock, std::chrono::seconds(10));
            }
        }
    }

    RowGroup & row_group = reader.row_groups.at(task.row_group_idx);
    RowSubgroup & row_subgroup = row_group.subgroups.at(task.row_subgroup_idx);
    chassert(row_subgroup.stage == ReadStage::Deliver);
    Columns output_columns(reader.sample_block->columns());
    for (size_t i = 0; i < output_columns.size(); ++i)
        output_columns[i] = std::move(reader.getOrFormOutputColumn(row_subgroup, i));
    Chunk chunk(std::move(output_columns), row_subgroup.filter.rows_pass);
    BlockMissingValues block_missing_values = std::move(row_subgroup.block_missing_values);

    auto row_numbers_info = std::make_shared<ChunkInfoRowNumbers>(
        row_subgroup.start_row_idx + row_group.start_global_row_idx);
    if (row_subgroup.filter.rows_pass != row_subgroup.filter.rows_total)
    {
        chassert(row_subgroup.filter.rows_pass > 0);
        chassert(!row_subgroup.filter.filter.empty());
        chassert(countBytesInFilter(row_subgroup.filter.filter) == chunk.getNumRows());

        row_numbers_info->applied_filter = std::move(row_subgroup.filter.filter);
    }
    chunk.getChunkInfos().add(std::move(row_numbers_info));

    /// The ColumnData token for this subgroup is released below (clearRowSubgroup), but the columns
    /// live on inside `chunk`. Keep them charged to the Decoded pool until the pipeline drops the
    /// chunk (see `poolOf(ReadStage)` and the `delivered_bytes` uses in `scheduleTasksIfNeeded`/`flushMemoryUsageDiff`).
    chunk.getChunkInfos().add(std::make_shared<ChunkMemoryInfo>(delivered_bytes, chunk.allocatedBytes()));

    /// This is a terrible hack to make progress indication kind of work.
    ///
    /// TODO: Fix progress bar in many ways:
    ///        1. use number of rows instead of bytes;
    ///           don't lie about number of bytes read (getApproxBytesReadForChunk()),
    ///        2. estimate total rows to read after filtering row groups;
    ///           for rows filtered out by PREWHERE, either report them as read or reduce the
    ///           estimate of number of rows to read (make it signed),
    ///        3. report uncompressed deserialized IColumn bytes instead of file bytes, for
    ///           consistency with MergeTree reads,
    ///        4. correctly extrapolate progress when reading many files in sequence, e.g.
    ///           file('part{1..1000}.parquet'),
    ///        5. correctly merge progress info when a query reads both from MergeTree and files, or
    ///           parquet and text files.
    ///       Probably get rid of getApproxBytesReadForChunk() and use the existing
    ///       ISource::progress()/addTotalRowsApprox instead.
    ///       For (4) and (5), either add things to struct Progress or make progress bar use
    ///       ProfileEvents instead of Progress.
    /// Sum compressed sizes of selected columns only. `reader.primitive_columns[i].column_idx`
    /// indexes into the parquet row group's `columns` array, so we pick exactly the column
    /// chunks that the query reads. Using `row_group.meta->total_compressed_size` here would
    /// inflate `read_bytes` by `total_columns / selected_columns`.
    size_t selected_columns_compressed_bytes = 0;
    for (const auto & primitive_column : reader.primitive_columns)
    {
        size_t parquet_column_idx = primitive_column.column_idx;
        if (parquet_column_idx < row_group.meta->columns.size())
            selected_columns_compressed_bytes += size_t(row_group.meta->columns[parquet_column_idx].meta_data.total_compressed_size);
    }
    size_t virtual_bytes_read = selected_columns_compressed_bytes * row_subgroup.filter.rows_total / std::max(size_t(1), size_t(row_group.meta->num_rows));

    /// This updates `memory_usage` of previous stages, which may allow more tasks to be scheduled.
    MemoryUsageDiff diff(ReadStage::Deliver);
    finishRowSubgroupStage(task.row_group_idx, task.row_subgroup_idx, ReadStage::Deliver, /*step_idx=*/ 0, diff);
    flushMemoryUsageDiff(std::move(diff));

    return {std::move(chunk), std::move(block_missing_values), virtual_bytes_read};
}

}
