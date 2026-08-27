# Parquet v3 read path redesign — design spec

Date: 2026-08-27. Base: `antalya-26.6` (`fc67ca28aab`). Scope: `src/Processors/Formats/Impl/Parquet/{Prefetcher,ReadManager,Reader,ReadCommon}.*` and `ParquetV3BlockInputFormat.cpp`.

Related: https://github.com/Altinity/ClickHouse/pull/2275 (read sizing — the workaround set this design replaces), https://github.com/Altinity/ClickHouse/pull/2266, https://github.com/Altinity/ClickHouse/pull/2235.

## 1. Problem

The v3 reader is latency-bound on object storage for structural reasons:

1. **Whole-task readiness.** A coalesced read `Prefetcher::Task` covers several requested ranges; `Prefetcher::getRangeData` waits for the *entire* task (`Task::completion`) even when the caller's bytes were the first to arrive. Coalescing across row groups therefore serializes their delivery, and decode of a subgroup cannot start until the last byte of a 16 MiB task lands. #2275 works around this with a "one task never spans two row groups" rule and adaptive task sizing.
2. **Issue is coupled to admission.** A subgroup's data-page reads are issued by its own `ColumnDataPrefetch` stage, which runs only after the previous subgroup's main step finished (`ReadManager::finishRowSubgroupStage`, `read_ptr`). One storage round trip per subgroup, serially. #2275 adds a read-ahead knob for the next subgroup.
3. **Memory budget keyed by pipeline stage.** Five stages each get `memory_target_fraction = 0.2` of `input_format_parquet_memory_high_watermark` (`ReadManager.cpp:82`). Compressed bytes in flight (~23 MB / row group) and decoded columns (~423 MB / row group) share the same kind of budget, so "fetch deep, decode shallow" is not expressible. Delivered `Chunk`s are never charged (`Deliver` fraction 0), so the high watermark is not a cap.
4. **Sequential page cursor per column chunk.** `Reader::ColumnChunk` holds `page`, `next_page_offset`, `data_pages_idx` shared by all subgroups of a row group (`Reader.h:389-395`). Subgroups must decode strictly in order; a single huge row group cannot use more than one decode thread per column.
5. **IO pool sized by an unrelated default.** `max_download_threads` (4, chosen for the URL engine) is the IO pool size (`ParquetV3BlockInputFormat.cpp:73-75`).

## 2. Goals

- Latency-bound by **one** round trip per file, then bandwidth-bound.
- Memory bounded by a cap the reader honours: two budgets by *lifetime class* — compressed bytes in flight, decoded bytes live (including delivered chunks) — plus a small fixed share for metadata.
- Subgroups of one row group decodable in parallel when a page index is present; delivery order unchanged.
- No behaviour change for local files at defaults beyond fewer syscalls; identical query results everywhere.
- Every phase independently shippable and default-safe; every new setting has a `SettingsChangesHistory` entry and a `DECLARE` doc string.

## 3. Non-goals

- New decoders, schema conversion, PREWHERE evaluation, bloom/column-index logic — untouched.
- A separate "v4" `IInputFormat`. This is an in-place redesign of the scheduler and prefetcher.
- Changing the on-disk or Native formats.

## 4. Design

### 4.1 Partial readiness in `Prefetcher` (Phase 1)

A task's ranges are sorted by offset and an HTTP body streams in offset order, so "bytes landed" is one monotonic counter per task.

- `Task::bytes_ready` (`std::atomic<size_t>`), advanced by the `readBigAt` progress callback (`ReadBufferFromS3::readBigAt` and `ReadWriteBufferFromHTTP` already call `copyFromIStreamWithProgressCallback` per ~1 MiB chunk; `CachedInMemoryReadBufferFromFile` calls it once; local `pread`, Azure and HDFS never call it — readiness then equals completion, today's behaviour).
- `getRangeData(handle)` needs `bytes_ready >= task_offset + length` **or** state `Done`. Waiting uses one per-task `min_waiting_threshold` (atomic, lowest pending threshold) and the `Prefetcher`-wide `ready_mutex`/`ready_cv`; the producer notifies only when `bytes_ready` crosses `min_waiting_threshold`. `Exception` and `Deallocated` wake everyone.
- Zero-copy cache path (`readBigAtRetainCells`) and `SeekAndRead`/`EntireFileIsInMemory` set `bytes_ready = length` at completion.
- Consequence: coalescing may span row groups without serializing delivery. `bytes_per_read_task` becomes purely a bandwidth/GET-count knob.

### 4.2 Issue controller and two budgets in `ReadManager` (Phase 2)

**Budgets.** Replace per-stage fractions with three pools, each an `std::atomic<ssize_t>` on `ReadManager` plus a per-reader limit from `SharedResourcesExt::getLimitsPerReader`:

| pool | charged by | released when | share of high watermark |
|---|---|---|---|
| `metadata` | bloom filter, column index, offset index, dictionary page prefetch handles | handle reset (unchanged) | `0.05` fixed |
| `compressed` | data-page prefetch handles (`PrefetchHandle::memory`) | page handle reset after decode | `input_format_parquet_compressed_memory_fraction` (default `0.35`) |
| `decoded` | `ColumnSubchunk::column_and_offsets_memory` **and** the delivered `Chunk` | column token reset; `ChunkMemoryInfo` destructor when the downstream pipeline drops the chunk | `1 - 0.05 - compressed` |

`MemoryUsageDiff::by_stage` is kept as the accounting vehicle but each `ReadStage` maps to one of the three pools (`poolOf(ReadStage)`); `Stage::memory_usage` is replaced by `ReadManager::pool_usage[3]`. Scheduling limits (`checkTaskSchedulingLimits`) read the pool of the stage being scheduled.

**Honest cap.** `ReadManager::read` attaches `ChunkMemoryInfo` (a `ChunkInfo`) holding `shared_ptr<std::atomic<ssize_t>>` to the `decoded` counter and the chunk's `allocatedBytes()`; its destructor subtracts. The counter is shared so it outlives `ReadManager`.

**Issue controller.** Data-page reads are issued by a single per-`ReadManager` FIFO, not by subgroup admission:

- When a row group finishes `OffsetIndex` for a step (all offset indexes for that step's columns decoded), `Reader::planPageReads(row_group, step)` calls `determinePagesToPrefetch` for **every** subgroup with `rows_pass > 0`, in subgroup order, producing `std::vector<PlannedRead>{row_group_idx, row_subgroup_idx, step_idx, handles}` appended to `ReadManager::issue_queue` (mutex-protected `std::deque`).
- `ReadManager::pumpIssueQueue(diff)` pops entries in FIFO order while `pool_usage[compressed] + planned_bytes <= compressed limit` **or** the entry belongs to the privileged `(first_incomplete_row_group, read_ptr)` pair, and calls `prefetcher.startPrefetch(handles, &diff)` (charged to `compressed`). Called from `flushMemoryUsageDiff` whenever `compressed` shrinks and from `finishRowSubgroupStage` after planning.
- Subgroup admission (`finishRowSubgroupStage`) no longer has a `ColumnDataPrefetch` stage: `OffsetIndex` → `ColumnData` directly. `ColumnData` tasks for a subgroup are scheduled only after its `PlannedRead` was issued (`RowSubgroup::reads_issued` flag set by the pump; the pump schedules the subgroup's decode if it was admitted and waiting). `ReadStage::ColumnDataPrefetch` enum value is removed.
- Steps ≥ 2 and step 0 (post-PREWHERE) are planned when the subgroup reaches them, exactly as today, but through the same queue so they obey the same budget.

Depth of read-ahead is now a consequence of the `compressed` budget: with the default 4 GiB high watermark and `0.35`, ~1.4 GiB of compressed pages may be in flight across all row groups and subgroups of a file. `input_format_parquet_bytes_per_read_task` (default `0` = `4 × min_bytes_for_seek`) controls coalescing only. `max_download_threads` no longer sizes the pool: `io_threads = max(max_download_threads, min(max_parsing_threads, 16))`, overridable by `input_format_parquet_max_io_threads`.

### 4.3 Per-subgroup page cursor (Phase 3)

- Move `PageState page`, `size_t next_page_offset`, `size_t data_pages_idx` from `ColumnChunk` into a new `struct PageCursor` owned by `ColumnSubchunk`. `ColumnChunk` keeps `data_pages`, `dictionary`, `offset_index`, `data_pages_prefetch_idx`.
- `Reader::skipToRowOrNextPage` and `readRowsInPage` take `PageCursor &` and the `ColumnChunk &`. With an offset index, a cursor is positioned with `std::upper_bound` on `data_pages[].end_row_idx` from `row_subgroup.start_row_idx` — no dependence on where the previous subgroup stopped. Without an offset index the cursor lives on the `ColumnChunk` (single sequential cursor, `ColumnChunk::sequential_cursor`) and subgroup admission stays strictly sequential for that row group (`RowGroup::sequential_decode = true`).
- Dictionary: `ColumnChunk::dictionary_mutex` + `std::atomic<bool> dictionary_ready`; first decoder to need it takes the mutex and decodes (`decodeDictionaryPage`), others wait on the mutex. Inline dictionary pages (no `dictionary_page_offset`) force `sequential_decode`.
- Page handles are released by refcount: `DataPage::users_remaining` (`std::atomic<UInt32>`) is set during planning to the number of subgroups whose row range intersects the page; each subgroup decrements after decoding its rows from the page; the last one calls `prefetch.reset(&diff)`. A page straddling two subgroups is decompressed twice (bounded: one page per boundary per column).
- Admission: `RowGroup::read_ptr` becomes "next subgroup to admit"; up to `input_format_parquet_parallel_subgroups` (default `2`; `1` = today's behaviour) subgroups of one row group may be in `ColumnData` when `!sequential_decode`. Delivery stays in order: a finished subgroup is marked `ready_for_delivery`; `RowGroup::delivery_cursor` pushes `subgroups[delivery_cursor]` to `delivery_queue` while it is ready, then advances. `is_privileged_task` uses `row_group.subgroups_in_progress == 0`.
- `clearColumnChunk` runs when `RowGroup::subgroups_decoded_remaining` reaches 0, not when `read_ptr == subgroups.size()`.

### 4.4 Settings

| setting | default | phase | replaces |
|---|---|---|---|
| `input_format_parquet_max_io_threads` | `0` (derive) | 1 | `max_download_threads` as pool size |
| `input_format_parquet_bytes_per_read_task` | `0` (= `4 × min_bytes_for_seek`) | 1 | hard-coded multiplier |
| `input_format_parquet_compressed_memory_fraction` | `0.35` | 2 | five `0.2` stage fractions |
| `input_format_parquet_parallel_subgroups` | `2` | 3 | — |

### 4.5 Observability

Profile events: `ParquetReadTasks`, `ParquetReadTaskBytes`, `ParquetPartialReadsServed` (Phase 1); `ParquetPlannedReads`, `ParquetIssueQueueStalls` (Phase 2); `ParquetParallelSubgroups` (Phase 3). `collectDeadlockDiagnostics` prints the three pools and the issue-queue length.

## 5. Invariants to test

1. Results identical with every new setting at its extremes (`parallel_subgroups` 1/2/8, `compressed_memory_fraction` 0.01/0.9, `bytes_per_read_task` tiny/huge), with and without PREWHERE, with and without page index, `max_parsing_threads = 1` and default.
2. No deadlock when PREWHERE drops all rows of a subgroup, when a row group is fully filtered, when the compressed budget is smaller than one page, and when the decoded budget is smaller than one subgroup (privileged path).
3. `ParquetPartialReadsServed > 0` on an S3 file whose two row groups coalesce into one task.
4. Peak `memory_usage` in `system.query_log` for a wide file stays under `high_watermark × 1.25` when `decoded` includes delivered chunks (Phase 2 acceptance).
5. TSan clean on the new stateless tests.

## 6. Rollout

Phase 1 → Phase 2 → Phase 3, each its own PR to `antalya-26.6`, each default-safe. Phase 3 is gated by `input_format_parquet_parallel_subgroups = 1` reproducing today's behaviour. Upstream each phase to ClickHouse master before or alongside the Antalya PR.
