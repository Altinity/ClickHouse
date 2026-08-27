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
6. **Depth is bounded by files open, not by the reader's knobs.** Measured on a 3-node Iceberg cluster (Appendix A): ~25–29 GETs in flight per node regardless of subgroup read-ahead, `max_active_files`, or a 128× larger memory budget. Inside a file, row groups advance almost serially through the bloom → column-index → offset-index → data chain of dependent GETs, so depth ≈ (files open per node) × ~1. Subgroup read-ahead adds depth in the wrong dimension.
7. **One static coalescing threshold serves two regimes.** On S3 the optimal gap to read through is ≈ per-stream bandwidth × RTT (~2 MiB measured; 4 MiB over-reads ~40% for no gain); when the bytes come from the local filesystem cache it is tens of KB (a 640-byte column bridged gaps into 3.5 MiB reads per row group: 32 GB moved through the cache disk for 450 MB of pages). `remote_read_min_bytes_for_seek` is wrong for one of the two.
8. **The filesystem cache amplifies random reads and ignores the query's knobs.** `CachedOnDiskReadBufferFromFile::readBigAt` (the Parquet path) does not pass the per-query `boundary_alignment`/`segments_batch_size` to `FileCache::getOrSet` (sequential reads do); `FileSegmentsHolder::~FileSegmentsHolder` hard-codes `allow_background_download = true`; the remote GET is opened to the segment end (4–32 MiB) for any range. Measured: 450 MB of requested pages → 35 GB downloaded from S3; cold with cache on is 2× slower than with cache off. The in-memory page cache rounds `readBigAt` to `page_cache_block_size` (1 MiB) blocks — another 20–40× over-read for 25–50 KiB pages.

## 2. Goals

- Latency-bound by **one** round trip per file, then bandwidth-bound.
- Memory bounded by a cap the reader honours: two budgets by *lifetime class* — compressed bytes in flight, decoded bytes live (including delivered chunks) — plus a small fixed share for metadata.
- Subgroups of one row group decodable in parallel when a page index is present; delivery order unchanged.
- A filesystem-cache-backed deployment pays for exactly the bytes a query reads (plus ≤ one alignment unit per range), and still ends up with those bytes cached; cold time ≈ cache-off time.
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

**Issue controller.** All reads — bloom-filter headers, column indexes, offset indexes **and** data pages — are issued by a single per-`ReadManager` FIFO, not by stage-by-stage admission. The index stages are what serialize row groups today (§1.6): at row-group init the planner enqueues the index reads of **every** surviving row group of the file (each a few KB), and enqueues data pages per row group as its indexes land. Data-page planning below is the second half of that queue:

- When a row group finishes `OffsetIndex` for a step (all offset indexes for that step's columns decoded), `Reader::planPageReads(row_group, step)` calls `determinePagesToPrefetch` for **every** subgroup with `rows_pass > 0`, in subgroup order, producing `std::vector<PlannedRead>{row_group_idx, row_subgroup_idx, step_idx, handles}` appended to `ReadManager::issue_queue` (mutex-protected `std::deque`).
- `ReadManager::pumpIssueQueue(diff)` pops entries in FIFO order while `pool_usage[compressed] + planned_bytes <= compressed limit` **or** the entry belongs to the privileged `(first_incomplete_row_group, read_ptr)` pair, and calls `prefetcher.startPrefetch(handles, &diff)` (charged to `compressed`). Called from `flushMemoryUsageDiff` whenever `compressed` shrinks and from `finishRowSubgroupStage` after planning.
- Subgroup admission (`finishRowSubgroupStage`) no longer has a `ColumnDataPrefetch` stage: `OffsetIndex` → `ColumnData` directly. `ColumnData` tasks for a subgroup are scheduled only after its `PlannedRead` was issued (`RowSubgroup::reads_issued` flag set by the pump; the pump schedules the subgroup's decode if it was admitted and waiting). `ReadStage::ColumnDataPrefetch` enum value is removed.
- Steps ≥ 2 and step 0 (post-PREWHERE) are planned when the subgroup reaches them, exactly as today, but through the same queue so they obey the same budget.

**Bytes-in-flight target.** The pump's admission limit is not only the `compressed` pool cap but a per-node target `bytes_in_flight ≈ stream_bandwidth × rtt × concurrency_headroom`, fitted online from the `Prefetcher`'s per-task `ReadBufferFromS3Microseconds`/task length (`Prefetcher::ReadStats`, EWMA). Measured on S3: ~1.7 GB/s × 60 ms ≈ 100 MB per node is needed to make 30–50 KB reads pay; today's structure reaches ~1 MB. `input_format_parquet_max_active_files` and `input_format_parquet_read_ahead_subgroups` (if present on the base) are removed: both are subsumed by the queue.

**Coalescing cost model (replaces one static threshold).** When `Prefetcher::pickRangesAndCreateTaskIfNotExists` decides whether to read through a gap, it asks the read buffer whether the gap bytes are already cached (`SeekableReadBuffer::getCachedRanges`, §4.4-D). Cached gap → merge (disk cost, tens of KB threshold); uncached gap → merge iff `gap ≤ stream_bandwidth × rtt` (the fitted S3 value, ~2 MiB here; the sweep in Appendix A shows 2 MiB beats 4 MiB on both time and bytes). `input_format_parquet_bytes_per_read_task` (default `0` = derive) caps the task span; `remote_read_min_bytes_for_seek` becomes the fallback when the buffer cannot answer. Phase 1 partial readiness is what makes a 2–4 MiB S3 task harmless for delivery latency, so the two are shipped together.

`max_download_threads` no longer sizes the pool: `io_threads = max(max_download_threads, min(max_parsing_threads, 16))`, overridable by `input_format_parquet_max_io_threads`.

### 4.2b Cross-file metadata prefetch (Phase 2b)

Many-small-file Iceberg tables (here ~2 300 files × 29 row groups) pay footer → indexes → data serially per file per stream; that chain, times ~50 files per stream, is the cold floor (q4: 13.5k GETs × 26 ms / 25 in flight ≈ 14 s). `StorageObjectStorageSource` (or the format-factory hook that creates `ParquetV3BlockInputFormat`) pre-opens the next N files of each stream and starts their footer and index reads under the same bytes-in-flight target, so a stream never waits on metadata RTTs between files. N derives from the target and the observed metadata size per file; setting `input_format_parquet_files_prefetch_ahead` (default `2`).

### 4.2c Cache cooperation (Phase 2c)

The filesystem cache must charge a random-access reader only for what it reads. Changes in `src/Disks/IO/CachedOnDiskReadBufferFromFile.cpp`, `src/Interpreters/Cache/FileSegment.cpp`, `src/IO/CachedInMemoryReadBufferFromFile.cpp`:

- **A. Honour per-query knobs on `readBigAt`.** Pass `info.cache_settings.boundary_alignment` and `segments_batch_size` into `FileCache::getOrSet` in `readBigAt` (today only the sequential path does). Standalone bug fix.
- **B. Honour `allow_background_download`.** `FileSegmentsHolder` carries the flag from `ReadInfo::cache_settings` and its destructor uses it instead of the hard-coded `true`; `filesystem_cache_enable_background_download_during_fetch` and `…_for_metadata_files_in_packed_storage` are either wired to that flag or deleted (they are written into `ReadSettings` and never read). Standalone bug fix.
- **C. Random-access segment policy.** Reads arriving through `readBigAt` create segments aligned to `min(boundary_alignment, random_access_boundary_alignment)` (new cache setting, default 256 KiB) and open the remote GET to that segment's end, so over-read per range is ≤ one alignment unit instead of ≤ 32 MiB. Sequential readers keep the large segments.
- **D. Cache oracle.** `SeekableReadBuffer::getCachedRanges(offset, len) -> std::vector<std::pair<size_t, size_t>>` (default: empty/unknown), implemented on `CachedOnDiskReadBufferFromFile` via a non-creating `FileCache::get`, and on `CachedInMemoryReadBufferFromFile` via the page-cache lookup. Consumed by the coalescing cost model (§4.2).
- **E. Page cache block size for random access.** `CachedInMemoryReadBufferFromFile::readBigAt` coalesces misses only up to the requested range plus one block; `page_cache_block_size` may be set to 256 KiB by the Parquet reader for its own reads (`ReadSettings` override at buffer creation).

Expected on the measured cluster: cold ≈ cache-off time (2× better) while the cache still fills with the bytes used; warm cache-disk traffic −2…50× via the cost model.

### 4.3 Per-subgroup page cursor (Phase 3 — after 2b/2c)

Demoted: the measured workload's slow queries are CPU-bound (q21 420 s CPU) or GET-count-bound; row groups have ~2 subgroups. Parallel subgroup decode addresses neither. Keep for the single-huge-row-group shape; schedule after Phases 2b and 2c.

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
| `input_format_parquet_files_prefetch_ahead` | `2` | 2b | — |
| cache setting `random_access_boundary_alignment` | `256 KiB` | 2c | cache `boundary_alignment` for random reads |
| `input_format_parquet_parallel_subgroups` | `2` | 3 | — |

Removed/subsumed: `input_format_parquet_max_active_files`, `input_format_parquet_read_ahead_subgroups` (Altinity PR #2275 knobs) — measured inert (Appendix A); replaced by the bytes-in-flight target. `remote_read_min_bytes_for_seek` stays as fallback only.

### 4.5 Observability

Profile events: `ParquetReadTasks`, `ParquetReadTaskBytes`, `ParquetPartialReadsServed` (Phase 1); `ParquetPlannedReads`, `ParquetIssueQueueStalls` (Phase 2); `ParquetParallelSubgroups` (Phase 3). `collectDeadlockDiagnostics` prints the three pools and the issue-queue length.

## 5. Invariants to test

1. Results identical with every new setting at its extremes (`parallel_subgroups` 1/2/8, `compressed_memory_fraction` 0.01/0.9, `bytes_per_read_task` tiny/huge), with and without PREWHERE, with and without page index, `max_parsing_threads = 1` and default.
2. No deadlock when PREWHERE drops all rows of a subgroup, when a row group is fully filtered, when the compressed budget is smaller than one page, and when the decoded budget is smaller than one subgroup (privileged path).
3. `ParquetPartialReadsServed > 0` on an S3 file whose two row groups coalesce into one task.
4. Peak `memory_usage` in `system.query_log` for a wide file stays under `high_watermark × 1.25` when `decoded` includes delivered chunks (Phase 2 acceptance).
5. TSan clean on the new stateless tests.
6. With the filesystem cache enabled and cold, `ReadBufferFromS3Bytes ≤ 1.5 × ParquetReadTaskBytes` for a Parquet scan (Phase 2c acceptance); the same query's second run reads 0 bytes from S3.
7. `filesystem_cache_boundary_alignment` and `filesystem_cache_allow_background_download=0` set at query level are honoured on the `readBigAt` path (test via `ReadBufferFromS3Bytes` and `FilesystemCacheBackgroundDownloadQueuePush`).

## 6. Rollout

Phase 1 → Phase 2c-A/B (small, standalone, upstream first) → Phase 2 (pools + planner incl. index stages + cost model + oracle D) → Phase 2b (cross-file metadata prefetch) → Phase 2c-C/E → Phase 3. Each its own PR to `antalya-26.6`, each default-safe. Phase 3 is gated by `input_format_parquet_parallel_subgroups = 1` reproducing today's behaviour. Upstream each phase to ClickHouse master before or alongside the Antalya PR.

## Appendix A. Measurements that shaped §1.6–1.8 (vig-test, 3 nodes, Iceberg on S3, 2026-08-27)

Build under test: `antalya-26.6` + Altinity PR #2275 read-sizing + subgroup read-ahead (`ParquetReadAheadSubgroups` fired on every row group). Caches dropped on all nodes before every run; 2 reps per arm.

- Bench "cold" was half-warm: true cold q17 19 s vs 10 s, q20 21 s vs 14 s.
- Read-ahead (`read_ahead_subgroups=1`) and `max_active_files=8`: wall unchanged; in-flight GETs 25 → 24; starvation 61% → 66%.
- `memory_low_watermark` 2 MiB → 256 MiB: in-flight 74 → 79 (q20), 25 → 27 (q4).
- Filesystem cache off: 2× faster cold everywhere (q20 18.7 → 9.8 s, q17 18.8 → 11.1, q4 12.3 → 6.4). With cache on and `remote_read_min_bytes_for_seek=64K`: 450 MB requested → 35 GB downloaded (q20). Query-level `filesystem_cache_boundary_alignment`/`allow_background_download=0`: no effect (code gaps §1.8). Page cache on, fs cache off: exactly 1.00 MB per GET.
- Seek-threshold sweep, both caches off: time ∝ GET count at ~25 in flight (26–40 ms/GET) until ~2 MB/GET, where per-GET time climbs (4.3 MB → 65–86 ms). 2 MiB: q4 4.4 s / 7.0 GB (4 MiB: 5.9 s / 9.9 GB; 64 KiB: 14.0 s / 1.9 GB); q16 3.4 s (4 MiB 3.9; 64 KiB 7.6); q17 11.6 (4 MiB 10.2; 64 KiB 21.2). q20 flat: its 4 columns form two clusters 3.4 MB apart, so only 25 KB or 3.5 MB reads exist.
- Per node, q20 cold, caches off: 29 GETs in flight in every arm; 4.3 MB/GET → 1.7 GB/s vs 33 KB/GET → 18 MB/s at the same depth. Bandwidth × RTT ≈ 100 MB per node needed for small reads to pay.
- Warm: without constant-column skip, bytes through the cache disk are 2–50× higher for the same queries (q20 30.6 GB vs 0.6 GB) — gap coalescing across a 640-byte column; the const skip's warm win was that column's removal, not decode savings.
