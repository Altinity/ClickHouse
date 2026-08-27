# Parquet Read Path Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Parquet v3 reader latency-bound by one round trip per file and memory-bound by a cap it honours, by (1) serving prefetched bytes as they land, (2) issuing all page reads of a row group from one budgeted queue instead of per-subgroup admission, and (3) giving each row subgroup its own page cursor so subgroups decode in parallel.

**Architecture:** Three phases, each an independent PR to `antalya-26.6`. Phase 1 touches only `Prefetcher` (partial readiness + pool sizing). Phase 2 rewires `ReadManager` memory accounting into three lifetime pools and adds an issue queue that replaces the `ColumnDataPrefetch` stage. Phase 3 moves the page cursor from `Reader::ColumnChunk` to `Reader::ColumnSubchunk` and lets `finishRowSubgroupStage` admit several subgroups of a row group.

**Tech Stack:** C++23, ClickHouse build (`ninja clickhouse` in `build/`), stateless shell tests under `tests/queries/0_stateless/` created with `./tests/queries/0_stateless/add-test <name>.sh`, MinIO via `s3_conn` named collection in the stateless harness.

**Spec:** `docs/superpowers/specs/2026-08-27-parquet-readpath-redesign.md`

## Amendments (2026-08-27/28)

Re-cut after the vig-test measurements (spec Appendix A) and the upstream `ReaderExecutor` review (spec §4.2c). Tasks 1–3 (Phase 1) stand. Tasks 4–6 are the patch-up for the legacy cache path and an interim coalescing rule; Tasks 7–10 are Phase 2 (pools, honest cap, read stats, issue controller); Task 11 validates on vig-test and force-pushes the branch to Altinity PR #2275 (user-authorized). Former Phase 3 tasks are kept under "Deferred" and are not executed.

## Global Constraints

- Branch off `altinity/antalya-26.6`; one PR per phase, target `antalya-26.6`, no stacked PRs.
- Allman braces; `f` not `f()` in prose; wrap SQL/class/function names in backticks in comments and commit messages.
- Every new setting: `DECLARE` in `src/Core/FormatFactorySettings.h` with a full doc string, mirror in `src/Formats/FormatSettings.h`, copy in `src/Formats/FormatFactory.cpp`, entry in the `"26.6.2.20001.altinityantalya"` block of `src/Core/SettingsChangesHistory.cpp`.
- Every new profile event in `src/Common/ProfileEvents.cpp` with a description that names the related setting.
- No `sleep` to fix races. No fallback paths that hide errors.
- Tests: new `.sh` per behaviour via `add-test`; tag `no-fasttest`; never extend existing tests; do not add `no-parallel`.
- Build: `ninja clickhouse > build/build_<task>.log 2>&1` and have a subagent summarize the log. Run tests as `./tests/clickhouse-test <name> > build/test_<name>.log 2>&1`.
- Commit after each task with a `Signed-off-by` trailer (`git commit -s`).

---

## Phase 1 — Partial readiness in `Prefetcher`

### Task 1: Pool sizing and read-task size settings

**Files:**
- Modify: `src/Core/FormatFactorySettings.h` (after `input_format_parquet_local_file_min_bytes_for_seek`, ~line 250)
- Modify: `src/Formats/FormatSettings.h` (struct `Parquet`, ~line 359)
- Modify: `src/Formats/FormatFactory.cpp` (~line 248, next to `enable_row_group_prefetch`)
- Modify: `src/Core/SettingsChangesHistory.cpp` (`"26.6.2.20001.altinityantalya"` block, ~line 42)
- Modify: `src/Processors/Formats/Impl/ParquetV3BlockInputFormat.cpp:57-76`

**Interfaces:**
- Produces: `format_settings.parquet.max_io_threads` (`size_t`, 0 = derive), `format_settings.parquet.bytes_per_read_task` (`size_t`, 0 = `4 × min_bytes_for_seek`); `FormatParserSharedResources::io_threads` (`size_t`, the pool size actually created).

- [ ] **Step 1: Declare the settings**

In `src/Core/FormatFactorySettings.h`, after the `input_format_parquet_local_file_min_bytes_for_seek` block:

```cpp
    DECLARE(UInt64, input_format_parquet_max_io_threads, 0, R"(
Size of the thread pool that issues reads for the Parquet reader, shared by all files read by the
query. `0` derives it as `max(max_download_threads, min(max_parsing_threads, 16))`.

With too few reads in flight to cover the storage's response time, decoding threads end up waiting
for reads.
)", 0) \
    DECLARE(UInt64, input_format_parquet_bytes_per_read_task, 0, R"(
Target size of a single read issued by the Parquet reader; nearby column chunks and pages are
coalesced up to this size. `0` derives it as four times the min-bytes-for-seek of the underlying
storage. Bytes of a coalesced read become available to decoding as they arrive, so a large value
does not delay the first row group of the read.
)", 0) \
```

In `src/Formats/FormatSettings.h` inside `struct Parquet`:

```cpp
        /// 0 = derive from max_download_threads / max_parsing_threads.
        size_t max_io_threads = 0;
        /// 0 = derive from the storage's min-bytes-for-seek.
        size_t bytes_per_read_task = 0;
```

In `src/Formats/FormatFactory.cpp` next to `format_settings.parquet.enable_row_group_prefetch = ...`:

```cpp
    format_settings.parquet.max_io_threads = settings[Setting::input_format_parquet_max_io_threads];
    format_settings.parquet.bytes_per_read_task = settings[Setting::input_format_parquet_bytes_per_read_task];
```

In `src/Core/SettingsChangesHistory.cpp`, inside `addSettingsChanges(settings_changes_history, "26.6.2.20001.altinityantalya", { ... })`:

```cpp
            {"input_format_parquet_max_io_threads", 0, 0, "New setting: size of the thread pool that issues reads for the Parquet reader. 0 derives it from `max_download_threads` and `max_parsing_threads`; the derived value is larger than the previous hard-coded `max_download_threads` (default 4)."},
            {"input_format_parquet_bytes_per_read_task", 0, 0, "New setting: target size of a single coalesced read issued by the Parquet reader. 0 derives it from the min-bytes-for-seek of the underlying storage, as before."},
```

- [ ] **Step 2: Record the created pool size on the shared resources**

In `src/Formats/FormatParserSharedResources.h` add after `const size_t max_io_threads = 0;`:

```cpp
    /// Size of `io_runner`'s pool once created (see ParquetV3BlockInputFormat::initializeIfNeeded);
    /// 0 until then. Readers size their read-ahead from this, not from `max_io_threads`.
    std::atomic<size_t> io_threads {0};
```

- [ ] **Step 3: Use the settings in `ParquetV3BlockInputFormat`**

Replace `read_options.bytes_per_read_task = min_bytes_for_seek * 4;` (line 60) with:

```cpp
    read_options.bytes_per_read_task = format_settings.parquet.bytes_per_read_task != 0
        ? format_settings.parquet.bytes_per_read_task
        : min_bytes_for_seek * 4;
```

Replace the `initOnce` body's IO pool creation (lines 73-75) with:

```cpp
                /// `max_download_threads` defaults to 4, picked for the URL engine; on object storage
                /// that rarely keeps the decoding threads fed.
                size_t io_threads = format_settings.parquet.max_io_threads;
                if (io_threads == 0)
                    io_threads = std::max(
                        parser_shared_resources->max_io_threads,
                        std::min<size_t>(parser_shared_resources->max_parsing_threads, 16));
                if (format_settings.parquet.enable_row_group_prefetch && io_threads > 0)
                {
                    parser_shared_resources->io_runner.initThreadPool(
                        getFormatParsingThreadPool().get(), io_threads, ThreadName::PARQUET_PREFETCH, CurrentThread::getGroup());
                    parser_shared_resources->io_threads.store(io_threads, std::memory_order_relaxed);
                }
```

- [ ] **Step 4: Build**

Run: `ninja clickhouse > build/build_task1.log 2>&1` (from `build/`), subagent summarizes. Expected: success.

- [ ] **Step 5: Smoke test**

Run: `build/programs/clickhouse local -q "SELECT count() FROM file('tests/queries/0_stateless/data_parquet/nested_maps.snappy.parquet') SETTINGS input_format_parquet_max_io_threads = 8, input_format_parquet_bytes_per_read_task = 65536"`. Expected: a row count, no error.

- [ ] **Step 6: Commit**

```bash
git add src/Core/FormatFactorySettings.h src/Formats/FormatSettings.h src/Formats/FormatFactory.cpp src/Core/SettingsChangesHistory.cpp src/Formats/FormatParserSharedResources.h src/Processors/Formats/Impl/ParquetV3BlockInputFormat.cpp
git commit -s -m "Parquet: derive the IO pool size from the query and make the read-task size a setting"
```

### Task 2: Per-task `bytes_ready` with threshold waiting

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/Prefetcher.h` (`struct Task`, ~line 130; private members ~line 180)
- Modify: `src/Processors/Formats/Impl/Parquet/Prefetcher.cpp` (`readSync` ~line 132, `getRangeData` ~line 435, `runTask` ~line 500)
- Modify: `src/Common/ProfileEvents.cpp` (Parquet block, ~line 1476)

**Interfaces:**
- Produces: `Prefetcher::readSync(char * to, size_t n, size_t offset, const std::function<void(size_t)> & on_progress)`; `Task::bytes_ready`; `Task::min_waiting_threshold`; `Prefetcher::waitForBytes(Task *, size_t need)`.
- Consumes: `ReadBuffer::readBigAt(char *, size_t, size_t, const std::function<bool(size_t)> &)` — the callback receives *cumulative* bytes copied for this call (see `copyFromIStreamWithProgressCallback`).

- [ ] **Step 1: Add the profile event**

In `src/Common/ProfileEvents.cpp` next to `ParquetPrefetcherReadRandomRead`:

```cpp
    M(ParquetPartialReadsServed, "Times the Parquet reader started decoding from a coalesced read before that read had finished, because the requested bytes had already arrived", ValueType::Number) \
    M(ParquetReadTasks, "Coalesced read tasks created by the Parquet reader", ValueType::Number) \
    M(ParquetReadTaskBytes, "Bytes covered by `ParquetReadTasks`, including bytes read to close short gaps between requested ranges", ValueType::Bytes) \
```

- [ ] **Step 2: Extend `Task`**

In `Prefetcher.h`, inside `struct Task` after `CompletionNotification completion;`:

```cpp
        /// Bytes of `buf` (or `cached_region`) that have landed, counted from `offset`. Monotonic.
        /// Ranges inside a task are sorted by offset and object storage streams a range request in
        /// order, so a request whose end is <= bytes_ready can be served before the task finishes.
        std::atomic<size_t> bytes_ready {0};
        /// Lowest `bytes_ready` value some waiter is blocked on; SIZE_MAX if nobody waits.
        /// The producer notifies `ready_cv` only when `bytes_ready` reaches it.
        std::atomic<size_t> min_waiting_threshold {std::numeric_limits<size_t>::max()};
```

Add private members next to `std::mutex exception_mutex;`:

```cpp
    /// For partial-readiness waits (see Task::bytes_ready). One pair for all tasks: waits are rare
    /// (decode outran the read) and short.
    std::mutex ready_mutex;
    std::condition_variable ready_cv;

    /// Blocks until `task->bytes_ready >= need` or the task left the Running state. Returns the
    /// task state observed last.
    Task::State waitForBytes(Task * task, size_t need);
    /// Called from the read's progress callback and at completion.
    void publishBytesReady(Task * task, size_t bytes_ready);
```

Change the `readSync` declaration to:

```cpp
    void readSync(char * to, size_t n, size_t offset, const std::function<void(size_t /*cumulative*/)> & on_progress = {});
```

- [ ] **Step 3: Thread progress through `readSync`**

In `Prefetcher.cpp`, `readSync`:

```cpp
void Prefetcher::readSync(char * to, size_t n, size_t offset, const std::function<void(size_t)> & on_progress)
{
    if (offset > file_size || n > file_size - offset)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "File read out of bounds: offset {}, length {}, file size {}", offset, n, file_size);

    size_t nread = 0;
    switch (read_mode)
    {
        case ReadMode::RandomRead:
        {
            /// `readBigAt` reports cumulative bytes copied for this call; not every transport
            /// calls it (local pread, Azure, HDFS don't), in which case readiness equals completion.
            std::function<bool(size_t)> progress;
            if (on_progress)
                progress = [&](size_t copied) { on_progress(copied); return true; };
            nread = reader->readBigAt(to, n, offset, progress);
            ProfileEvents::increment(ProfileEvents::ParquetPrefetcherReadRandomRead);
            break;
        }
        case ReadMode::SeekAndRead:
        {
            std::lock_guard lock(read_mutex);
            reader->seek(offset, SEEK_SET);
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
        throw Exception(ErrorCodes::CANNOT_READ_ALL_DATA, "Unexpected end of file: read {} bytes instead of {} at offset {}", nread, n, offset);
    if (on_progress)
        on_progress(n);
}
```

(Keep the existing `SeekAndRead`/`EntireFileIsInMemory` bodies if they differ; only the callback plumbing is new.)

- [ ] **Step 4: Publish and wait**

Add to `Prefetcher.cpp`:

```cpp
void Prefetcher::publishBytesReady(Task * task, size_t bytes_ready)
{
    size_t prev = task->bytes_ready.load(std::memory_order_relaxed);
    if (bytes_ready <= prev)
        return;
    task->bytes_ready.store(bytes_ready, std::memory_order_release);
    if (bytes_ready >= task->min_waiting_threshold.load(std::memory_order_acquire))
    {
        /// Waiters re-register their threshold if they are still unsatisfied after waking.
        task->min_waiting_threshold.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
        std::lock_guard lock(ready_mutex);
        ready_cv.notify_all();
    }
}

Prefetcher::Task::State Prefetcher::waitForBytes(Task * task, size_t need)
{
    std::unique_lock lock(ready_mutex);
    while (true)
    {
        Task::State s = task->state.load(std::memory_order_acquire);
        if (s != Task::State::Running)
            return s;
        if (task->bytes_ready.load(std::memory_order_acquire) >= need)
            return s;
        /// Register the threshold, then re-check: the producer reads the threshold after storing
        /// bytes_ready, we store the threshold before re-reading bytes_ready, so one of us sees the other.
        size_t cur = task->min_waiting_threshold.load(std::memory_order_relaxed);
        while (cur > need && !task->min_waiting_threshold.compare_exchange_weak(cur, need, std::memory_order_acq_rel))
        {
        }
        if (task->bytes_ready.load(std::memory_order_acquire) >= need || task->state.load(std::memory_order_acquire) != Task::State::Running)
            continue;
        ready_cv.wait(lock);
    }
}
```

In `runTask`, replace `readSync(task->buf.data(), task->length, task->offset);` with:

```cpp
            readSync(task->buf.data(), task->length, task->offset,
                [this, task](size_t copied) { publishBytesReady(task, copied); });
```

and in the zero-copy branch, after filling `task->cached_region` / `task->buf`, add `publishBytesReady(task, task->length);`. After the final state CAS (`compare_exchange_strong(s, final_state)`) and before `task->completion.notify();` add:

```cpp
    {
        /// Wake partial waiters too: the task is Done, Exception or Deallocated now.
        std::lock_guard lock(ready_mutex);
        ready_cv.notify_all();
    }
```

Also in `decreaseTaskRefcount`, after the `state.exchange(Deallocated)`, waiters must not be left sleeping: this path only runs when no `PrefetchHandle` references the task any more, so nobody can be waiting; add `chassert(task->min_waiting_threshold.load() == std::numeric_limits<size_t>::max());`.

- [ ] **Step 5: Serve partial reads in `getRangeData`**

Replace the waiting block in `getRangeData`:

```cpp
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

        if (s == Task::State::Running)
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
```

Below, the zero-copy branch is only reachable when `s == Done` (cache regions are published whole), so guard it with `if (s == Task::State::Done && task->cached_region.has_value())`. The `task->buf` span return stays: `buf` was resized to `task->length` before the read started, so `buf.data() + task_offset` is stable while the read continues to fill later bytes.

Also count tasks: in `pickRangesAndCreateTaskIfNotExists` after `task.length = end_offset - task.offset;`:

```cpp
    ProfileEvents::increment(ProfileEvents::ParquetReadTasks);
    ProfileEvents::increment(ProfileEvents::ParquetReadTaskBytes, task.length);
```

- [ ] **Step 6: Build**

`ninja clickhouse > build/build_task2.log 2>&1`, subagent summarizes. Expected: success.

- [ ] **Step 7: Existing regression tests still pass**

Run: `./tests/clickhouse-test 03723_parquet_prefetcher_read_big_at 03596_parquet_prewhere_page_skip_bug 03408_parquet_checksums > build/test_task2.log 2>&1`. Expected: all `OK`.

- [ ] **Step 8: Commit**

```bash
git add src/Processors/Formats/Impl/Parquet/Prefetcher.h src/Processors/Formats/Impl/Parquet/Prefetcher.cpp src/Common/ProfileEvents.cpp
git commit -s -m "Parquet: serve a coalesced read's bytes as they arrive instead of waiting for the whole task"
```

### Task 3: Stateless test for partial readiness over S3

**Files:**
- Create: `tests/queries/0_stateless/<N>_parquet_partial_read_readiness.sh` and `.reference` via `./tests/queries/0_stateless/add-test parquet_partial_read_readiness.sh`

- [ ] **Step 1: Write the test**

```bash
#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

TABLE="t_${CLICKHOUSE_TEST_UNIQUE_NAME}"
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS ${TABLE}"
${CLICKHOUSE_CLIENT} -q "
  CREATE TABLE ${TABLE} (k UInt64, s String)
  ENGINE = S3(s3_conn, filename = '${CLICKHOUSE_TEST_UNIQUE_NAME}_partial.parquet', format = 'Parquet')"

# Two row groups of ~8 MB of incompressible-ish strings each; one coalesced read task (bytes_per_read_task
# is far above both) spans them, so the first row group's bytes arrive long before the task completes.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO ${TABLE} SELECT number, repeat(hex(cityHash64(number)), 32) FROM numbers(400000)
  SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 200000,
           output_format_parquet_compression_method = 'none', output_format_parquet_write_page_index = 1"

echo "-- results identical with tiny and huge read tasks"
Q="SELECT count(), sum(k), sum(length(s)), sum(cityHash64(s)) FROM ${TABLE}"
${CLICKHOUSE_CLIENT} -q "${Q} SETTINGS input_format_parquet_bytes_per_read_task = 65536, use_parquet_metadata_cache = 0"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_big" -q "${Q} SETTINGS input_format_parquet_bytes_per_read_task = 268435456, use_parquet_metadata_cache = 0, max_threads = 4"

echo "-- one coalesced read spanned both row groups and decoding started before it finished"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT ProfileEvents['ParquetReadTasks'] <= 3, ProfileEvents['ParquetPartialReadsServed'] > 0
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_big'"

${CLICKHOUSE_CLIENT} -q "DROP TABLE ${TABLE}"
```

Reference:

```
-- results identical with tiny and huge read tasks
400000	79999800000	25600000	<hash>
400000	79999800000	25600000	<hash>
-- one coalesced read spanned both row groups and decoding started before it finished
1	1
```

Fill `<hash>` from the first run (both lines must be equal).

- [ ] **Step 2: Run it**

`./tests/clickhouse-test <N>_parquet_partial_read_readiness > build/test_task3.log 2>&1`. Expected: `OK`. If `ParquetPartialReadsServed` is 0, the MinIO body arrived in one chunk: raise the row count until the read exceeds `DBMS_DEFAULT_BUFFER_SIZE × 4`, do not weaken the assertion.

- [ ] **Step 3: Commit**

```bash
git add tests/queries/0_stateless/<N>_parquet_partial_read_readiness.*
git commit -s -m "Parquet: test that decoding starts on a coalesced read before it completes"
```

---

## Patch-up — legacy cache path and interim coalescing (Phase 2c-A/B + interim)

These land first after Phase 1 and are what the Antalya 26.6 legacy read path runs; see spec §4.2c.

### Task 4: `readBigAt` honours the per-query cache boundary alignment (2c-A)

**Files:**
- Modify: `src/Disks/IO/CachedOnDiskReadBufferFromFile.cpp:1551-1561` (the `getOrSet` call inside `readBigAt`)
- Test: `tests/queries/0_stateless/<N>_parquet_cache_readbigat_alignment.sh` via `./tests/queries/0_stateless/add-test parquet_cache_readbigat_alignment.sh`

**Interfaces:**
- Consumes: `FileCache::getOrSet(key, offset, size, file_size, settings, file_segments_limit, origin, std::optional<size_t> boundary_alignment_)` (`src/Interpreters/FileCache/FileCache.h:145-153`); `info.cache_settings.boundary_alignment` (`std::optional<size_t>`, from the query setting `filesystem_cache_boundary_alignment`, `StorageObjectStorageSource.cpp:1459`).
- Produces: random-access reads create/lookup file segments aligned to the query's alignment, as sequential reads already do (`CachedOnDiskReadBufferFromFile.cpp:210-218`).

Why: a `readBigAt` for 50 KiB in the middle of a 4 MiB-aligned segment must download from the segment's committed frontier (its start) up to the requested end before it can serve — ~2 MB per GET measured for 50 KiB requests (spec Appendix A). The sequential path passes the per-query alignment; the random-access path does not.

- [ ] **Step 1: Write the failing test**

```bash
#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings
# - no-fasttest: needs S3 (s3_conn) and the `cache_for_readbigat` filesystem cache from storage_conf.xml
# - no-random-settings: asserts on read byte counters

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_align.parquet"
# 64 columns x 200k rows, uncompressed, small pages: each row group is ~50 MB with 64 column chunks,
# so reading 2 columns touches two ~800 KB chunks per row group that sit far apart in the file.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION s3(s3_conn, filename = '${FILE}', format = 'Parquet')
  SELECT number AS k, $(for i in $(seq 1 62); do echo -n "number * $i AS c$i, "; done) toString(number) AS s
  FROM numbers(200000)
  SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 100000,
           output_format_parquet_compression_method = 'none', output_format_parquet_data_page_size = 65536,
           output_format_parquet_write_page_index = 1"

run() {
  local tag=$1 align=$2
  ${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR FILESYSTEM CACHE 'cache_for_readbigat'"
  ${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_${tag}" -q "
    SELECT sum(k), sum(c31) FROM s3(s3_conn, filename = '${FILE}', format = 'Parquet')
    SETTINGS enable_filesystem_cache = 1, filesystem_cache_name = 'cache_for_readbigat',
             filesystem_cache_boundary_alignment = ${align}, remote_read_min_bytes_for_seek = 65536,
             use_parquet_metadata_cache = 0, max_threads = 4"
}

echo "-- results identical"
run default 0
run small 65536

echo "-- with a 64 KiB alignment the cache downloads at most 2x what the reader asked for; with the cache default (1 MiB) it downloads far more"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', '') tag,
         ProfileEvents['CachedReadBufferReadFromSourceBytes'] <= 2 * ProfileEvents['ParquetReadTaskBytes'] AS tight,
         ProfileEvents['CachedReadBufferReadFromSourceBytes'] >= 4 * ProfileEvents['ParquetReadTaskBytes'] AS loose
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id LIKE '${CLICKHOUSE_TEST_UNIQUE_NAME}_%'
  ORDER BY tag"
```

Reference:

```
-- results identical
19999900000	619996900000
19999900000	619996900000
-- with a 64 KiB alignment the cache downloads at most 2x what the reader asked for; with the cache default (1 MiB) it downloads far more
default	0	1
small	1	0
```

(`cache_for_readbigat` has `boundary_alignment` 1 MiB in `tests/config/config.d/storage_conf.xml:173-178`. Sum of `c31` = 31 × 19999900000 = 619996900000.)

- [ ] **Step 2: Run it to verify it fails**

`./tests/clickhouse-test <N>_parquet_cache_readbigat_alignment > build/test_task4_red.log 2>&1`. Expected: FAIL — the `small` row shows `0 1` because the alignment is ignored.

- [ ] **Step 3: Implement**

In `readBigAt`, replace the `getOrSet` call:

```cpp
        CreateFileSegmentSettings create_settings(FileSegmentKind::Regular);
        /// Random-access reads must honour the per-query alignment like the sequential path does
        /// (nextFileSegmentsBatch): a small read in the middle of a large aligned segment has to
        /// download from the segment's committed frontier up to the requested end before it can
        /// be served, so the alignment is the read amplification for small ranges.
        current_info.file_segments = cache->getOrSet(
            info.cache_key,
            /* offset */range_begin,
            /* size */n,
            file_size.value(),
            create_settings,
            /* batch_size */0,
            origin,
            info.cache_settings.boundary_alignment);
```

`batch_size` stays 0: `readBigAt` loops over exactly the segments it holds, so a batch limit would truncate the read.

- [ ] **Step 4: Build, run test to verify it passes**

`ninja clickhouse > build/build_task4.log 2>&1` (foreground, `timeout: 600000`; re-run if cut off); then `./tests/clickhouse-test <N>_parquet_cache_readbigat_alignment 03988_cached_read_big_at > build/test_task4.log 2>&1`. Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/IO/CachedOnDiskReadBufferFromFile.cpp tests/queries/0_stateless/<N>_parquet_cache_readbigat_alignment.*
git commit -s -m "Filesystem cache: honour the per-query boundary alignment on the readBigAt path"
```

### Task 5: `FileSegmentsHolder` honours `filesystem_cache_allow_background_download` (2c-B)

**Files:**
- Modify: `src/Interpreters/FileCache/FileSegment.h:323-362` (struct `FileSegmentsHolder`), `src/Interpreters/FileCache/FileSegment.cpp:1300-1325` (`reset`)
- Modify: `src/Disks/IO/CachedOnDiskReadBufferFromFile.cpp` — after every `info.file_segments = cache->get/getOrSet(...)` (`:198-218`) and `current_info.file_segments = ...` (`:1543-1561`)
- Test: extend `<N>_parquet_cache_readbigat_alignment.sh` from Task 4 with a third run

**Interfaces:**
- Produces: `void FileSegmentsHolder::setAllowBackgroundDownload(bool)`; `reset` uses the stored flag instead of the hard-coded `true`.

Why: `~FileSegmentsHolder` → `reset` → `completeAndPopFrontImpl(/*allow_background_download=*/true, …)` enqueues the rest of every partially read segment for background download regardless of the query setting; `readBigAt` holders are destroyed after every random read, so every 50 KiB read schedules up to a whole segment of background traffic. The existing comment in `reset` argues for `true` when another reader partially read the segment; keeping the default `true` and letting the reader opt out per query preserves that.

- [ ] **Step 1: Extend the test (failing first)**

Append to the test after the `small` run:

```bash
run nobg 65536
```

and change `run` to accept a third argument appended to `SETTINGS`: `run nobg 65536 ", filesystem_cache_allow_background_download = 0"`. Add to the readback query a column `ProfileEvents['FilesystemCacheBackgroundDownloadQueuePush'] = 0 AS no_bg`, and to the reference:

```
default	0	1	0
nobg	1	0	1
small	1	0	0
```

Run: expected FAIL (`nobg … 0` in the last column).

- [ ] **Step 2: Implement**

`FileSegment.h`, inside `FileSegmentsHolder`:

```cpp
    /// Whether segments left partially downloaded when this holder is destroyed may be queued for
    /// background download. Defaults to true (see the comment in `reset`); a reader that knows its
    /// reads are one-shot random accesses (`filesystem_cache_allow_background_download = 0`) opts out.
    void setAllowBackgroundDownload(bool value) { allow_background_download_on_reset = value; }
```

and a private member `bool allow_background_download_on_reset = true;`. In `reset`:

```cpp
            file_segment_it = completeAndPopFrontImpl(allow_background_download_on_reset, /*force_shrink_to_downloaded_size=*/false);
```

(keep the existing comment, add one line: "`allow_background_download_on_reset` lets a reader opt out per query.")

`CachedOnDiskReadBufferFromFile.cpp`: after each of the four holder assignments add

```cpp
    info.file_segments->setAllowBackgroundDownload(info.cache_settings.allow_background_download);
```

(`current_info.file_segments->…` in `readBigAt`). Do not touch the two unread settings `filesystem_cache_enable_background_download_during_fetch` / `…_for_metadata_files_in_packed_storage`; note them in the report as dead.

- [ ] **Step 3: Build, test, commit**

`ninja clickhouse > build/build_task5.log 2>&1`; `./tests/clickhouse-test <N>_parquet_cache_readbigat_alignment 02240_filesystem_cache_bypass_cache_threshold 03988_cached_read_big_at > build/test_task5.log 2>&1` (the second exists on base; if not, run `./tests/clickhouse-test filesystem_cache > …` and report counts). Expected: all `OK`.

```bash
git add src/Interpreters/FileCache/FileSegment.h src/Interpreters/FileCache/FileSegment.cpp src/Disks/IO/CachedOnDiskReadBufferFromFile.cpp tests/queries/0_stateless/<N>_parquet_cache_readbigat_alignment.*
git commit -s -m "Filesystem cache: let a reader opt out of background download of partially read segments"
```

### Task 6: Interim coalescing rule — amplification cap and a 2 MiB remote gap

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/Prefetcher.h` (private members), `src/Processors/Formats/Impl/Parquet/Prefetcher.cpp` (`init` ~line 30, `pickRangesAndCreateTaskIfNotExists` ~lines 303-372)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadCommon.h` (`struct ReadOptions`: add `coalesce_gap_bytes`, `max_read_amplification`), `src/Processors/Formats/Impl/ParquetV3BlockInputFormat.cpp:57-62` (fill them)
- Settings (four files per Global Constraints): `input_format_parquet_coalesce_gap_bytes` (UInt64, default `2097152`), `input_format_parquet_max_read_amplification` (Double, default `4`)
- Test: `tests/queries/0_stateless/<N>_parquet_read_amplification.sh`

**Interfaces:**
- Produces: `ReadOptions::coalesce_gap_bytes`, `ReadOptions::max_read_amplification`; `Prefetcher::gap_bytes` (= `min(min_bytes_for_seek, coalesce_gap_bytes)`, or `min_bytes_for_seek` when the setting is 0).

Why (spec §1.7, Appendix A): on S3 the cold optimum gap is ≈ bandwidth × RTT ≈ 2 MiB (4 MiB over-reads ~40% for no time gain); on a warm cache one 640-byte column bridged gaps into 3.5 MiB reads per row group (70× amplification). The cap bounds `task span / useful bytes` so a tiny column can never drag megabytes; the gap default trims the S3 case. Both are interim until the storage layer's cost model takes over (spec §4.2c).

- [ ] **Step 1: Settings**

```cpp
    DECLARE(UInt64, input_format_parquet_coalesce_gap_bytes, 2097152, R"(
Largest gap between two needed byte ranges of a Parquet file that the reader reads through in order to
serve both with one request. Applied on top of the storage's min-bytes-for-seek (the smaller wins);
`0` uses the storage value only. On object storage the useful gap is about one round trip's worth of
bandwidth, ~2 MiB; reading through larger gaps costs bytes without saving time.
)", 0) \
    DECLARE(Double, input_format_parquet_max_read_amplification, 4, R"(
Upper bound on `bytes read / bytes needed` for one coalesced Parquet read. Coalescing stops extending a
read when the span would exceed this multiple of the useful bytes it covers, so a few small column chunks
cannot drag megabytes of unrelated data through the cache or the network. `0` disables the bound.
)", 0) \
```

`FormatSettings.h`: `size_t coalesce_gap_bytes = 2097152; double max_read_amplification = 4;`. `FormatFactory.cpp`: copy both. `SettingsChangesHistory.cpp` (Antalya block): `{"input_format_parquet_coalesce_gap_bytes", 0, 2097152, "New setting: cap on the gap the Parquet reader reads through when coalescing nearby ranges; previously the storage's min-bytes-for-seek (4 MiB on object storage) applied unconditionally."}`, `{"input_format_parquet_max_read_amplification", 0, 4, "New setting: bound on bytes read / bytes needed per coalesced Parquet read."}`.

`ReadCommon.h` `struct ReadOptions`: add `size_t coalesce_gap_bytes = 0; double max_read_amplification = 0;`. `ParquetV3BlockInputFormat.cpp` after `read_options.bytes_per_read_task = …`:

```cpp
    read_options.coalesce_gap_bytes = format_settings.parquet.coalesce_gap_bytes;
    read_options.max_read_amplification = format_settings.parquet.max_read_amplification;
```

- [ ] **Step 2: Prefetcher**

`Prefetcher.h` private: `size_t gap_bytes{}; double max_read_amplification = 0;`. In `Prefetcher::init` after `bytes_per_read_task = options.bytes_per_read_task;`:

```cpp
    gap_bytes = options.coalesce_gap_bytes ? std::min(min_bytes_for_seek, options.coalesce_gap_bytes) : min_bytes_for_seek;
    max_read_amplification = options.max_read_amplification;
```

In `pickRangesAndCreateTaskIfNotExists`, both loops: replace `min_bytes_for_seek` in the gap tests with `gap_bytes`, and add the amplification test. Left loop condition becomes:

```cpp
        if (r.end + gap_bytes <= start_offset || // gap too long to read through
            r.start + bytes_per_read_task <= initial_offset || // task not too big
            exceedsAmplification(std::max(end_offset, r.end) - std::min(start_offset, r.start), total_length_of_covered_ranges + r.length()) ||
            !r.request->allow_incidental_read.load(std::memory_order_relaxed)) // range wants to be coalesced
            break;
```

right loop:

```cpp
        if (end_offset + gap_bytes <= r.start ||
            initial_offset + bytes_per_read_task <= r.end ||
            exceedsAmplification(std::max(end_offset, r.end) - std::min(start_offset, r.start), total_length_of_covered_ranges + r.length()) ||
            !r.request->allow_incidental_read.load(std::memory_order_relaxed))
            break;
```

with a private helper:

```cpp
    /// True if a task spanning `span` bytes to serve `useful` bytes would exceed max_read_amplification.
    bool exceedsAmplification(size_t span, size_t useful) const
    {
        return max_read_amplification > 0 && static_cast<double>(span) > max_read_amplification * static_cast<double>(useful);
    }
```

Note `splitRange`'s "request already short" check (`range.length() < min_bytes_for_seek`, `Prefetcher.cpp:261`) keeps `min_bytes_for_seek` — it is about whether splitting is worth it, not about gaps.

- [ ] **Step 3: Test**

```bash
#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
F="${WORKING_DIR}/amp.parquet"

# 64 columns; we read k and c31 only, so useful bytes per row group are ~2 chunks of ~800 KB out of ~50 MB.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${F}', Parquet)
  SELECT number AS k, $(for i in $(seq 1 62); do echo -n "number * $i AS c$i, "; done) toString(number) AS s
  FROM numbers(200000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 100000,
           output_format_parquet_compression_method = 'none', output_format_parquet_data_page_size = 65536"

Q="SELECT sum(k), sum(c31) FROM file('${F}', Parquet)"
# Force the local path to behave like object storage: a 4 MiB seek threshold and 16 MiB tasks.
BASE="input_format_parquet_local_file_min_bytes_for_seek = 4194304, input_format_parquet_bytes_per_read_task = 16777216, max_threads = 2"

echo "-- results identical"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_uncapped" -q "${Q} SETTINGS ${BASE}, input_format_parquet_max_read_amplification = 0, input_format_parquet_coalesce_gap_bytes = 0"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_capped" -q "${Q} SETTINGS ${BASE}, input_format_parquet_max_read_amplification = 4, input_format_parquet_coalesce_gap_bytes = 0"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_gap" -q "${Q} SETTINGS ${BASE}, input_format_parquet_max_read_amplification = 0, input_format_parquet_coalesce_gap_bytes = 65536"

echo "-- the cap and the gap each cut bytes read by more than 2x versus uncapped"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  WITH (SELECT ProfileEvents['ParquetReadTaskBytes'] FROM system.query_log WHERE event_date >= yesterday() AND type = 'QueryFinish' AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_uncapped') AS uncapped
  SELECT replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', ''), ProfileEvents['ParquetReadTaskBytes'] * 2 < uncapped
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish' AND current_database = currentDatabase()
    AND query_id IN ('${CLICKHOUSE_TEST_UNIQUE_NAME}_capped', '${CLICKHOUSE_TEST_UNIQUE_NAME}_gap')
  ORDER BY 1"

rm -rf "${WORKING_DIR}"
```

Reference:

```
-- results identical
19999900000	619996900000
19999900000	619996900000
19999900000	619996900000
-- the cap and the gap each cut bytes read by more than 2x versus uncapped
capped	1
gap	1
```

Run red first (expect the `capped`/`gap` rows `0` before the change — the settings don't exist yet, so the red run errors with unknown setting; record that), then build, then green.

- [ ] **Step 4: Build, run `<N>_parquet_read_amplification` plus `03723_parquet_prefetcher_read_big_at` and the Phase 1 tests, commit**

```bash
git add src/Core/FormatFactorySettings.h src/Formats/FormatSettings.h src/Formats/FormatFactory.cpp src/Core/SettingsChangesHistory.cpp src/Processors/Formats/Impl/Parquet/ReadCommon.h src/Processors/Formats/Impl/Parquet/Prefetcher.h src/Processors/Formats/Impl/Parquet/Prefetcher.cpp src/Processors/Formats/Impl/ParquetV3BlockInputFormat.cpp tests/queries/0_stateless/<N>_parquet_read_amplification.*
git commit -s -m "Parquet: bound read amplification and cap the coalescing gap at 2 MiB on remote storage"
```

---

## Phase 2 — Lifetime pools, honest cap, and the issue controller

### Task 7: Replace per-stage memory usage with three pools

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/ReadCommon.h` (`ReadStage` enum ~line 82; `MemoryUsageDiff` ~line 112; `SharedResourcesExt` ~line 41)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.h` (`struct Stage` ~line 85)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.cpp` (`init` fractions ~line 78-100; `flushMemoryUsageDiff` ~line 590; `scheduleTasksIfNeeded` ~line 620; `collectDeadlockDiagnostics` ~line 1010)
- Modify: settings files as in Task 1 for `input_format_parquet_compressed_memory_fraction`

**Interfaces:**
- Produces: `enum class MemoryPool : UInt8 { Metadata, Compressed, Decoded }`; `constexpr MemoryPool poolOf(ReadStage)`; `ReadManager::pool_usage[3]` (`std::atomic<ssize_t>`); `ReadManager::poolLimits(MemoryPool) -> SharedResourcesExt::Limits`.

- [ ] **Step 1: Define pools**

In `ReadCommon.h` after the `ReadStage` enum:

```cpp
/// Memory is budgeted by how long bytes live and what they cost, not by pipeline stage:
///  Metadata   - bloom filters, column/offset indexes, dictionary pages. Small, short-lived.
///  Compressed - data pages in flight or awaiting decode. ~20-30 MB per row group, released as
///               pages are decoded. Depth of read-ahead is bounded by this pool.
///  Decoded    - IColumn memory for decoded subgroups *including chunks already delivered* to the
///               pipeline but not yet consumed. ~10-20x Compressed per row group.
enum class MemoryPool : UInt8
{
    Metadata,
    Compressed,
    Decoded,
};
constexpr size_t NUM_MEMORY_POOLS = 3;

constexpr MemoryPool poolOf(ReadStage stage)
{
    switch (stage)
    {
        case ReadStage::BloomFilterHeader:
        case ReadStage::BloomFilterBlocksOrDictionary:
        case ReadStage::ColumnIndexAndOffsetIndex:
        case ReadStage::OffsetIndex:
            return MemoryPool::Metadata;
        case ReadStage::ColumnDataPrefetch:
            return MemoryPool::Compressed;
        case ReadStage::NotStarted:
        case ReadStage::ColumnData:
        case ReadStage::Deliver:
        case ReadStage::Deallocated:
            return MemoryPool::Decoded;
    }
}
```

(`ColumnDataPrefetch` is removed in Task 6; until then it maps to `Compressed`.)

- [ ] **Step 2: Replace `Stage::memory_usage` and fractions**

In `ReadManager.h`, `struct Stage`: delete `std::atomic<size_t> memory_usage {0};` and `double memory_target_fraction = 1;`. Add to `ReadManager`:

```cpp
    /// See MemoryPool. Signed because deallocations can be flushed before the matching allocation
    /// on another thread.
    std::array<std::atomic<ssize_t>, NUM_MEMORY_POOLS> pool_usage {};
    std::array<double, NUM_MEMORY_POOLS> pool_fraction {};

    SharedResourcesExt::Limits poolLimits(MemoryPool pool) const;
```

In `ReadManager::init`, replace the block that sets `memory_target_fraction` per stage (keep the thread fractions):

```cpp
    const double compressed_fraction = reader.options.format.parquet.compressed_memory_fraction;
    if (!(compressed_fraction > 0 && compressed_fraction < 0.95))
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "input_format_parquet_compressed_memory_fraction must be in (0, 0.95), got {}", compressed_fraction);
    pool_fraction[size_t(MemoryPool::Metadata)] = 0.05;
    pool_fraction[size_t(MemoryPool::Compressed)] = compressed_fraction;
    pool_fraction[size_t(MemoryPool::Decoded)] = 1.0 - 0.05 - compressed_fraction;
```

```cpp
SharedResourcesExt::Limits ReadManager::poolLimits(MemoryPool pool) const
{
    /// Thread fraction is per stage, not per pool; callers that need it read Stage::thread_target_fraction.
    return SharedResourcesExt::getLimitsPerReader(*parser_shared_resources, pool_fraction[size_t(pool)], /*thread_fraction=*/ 1.0);
}
```

- [ ] **Step 3: Route `MemoryUsageDiff` into pools**

`MemoryUsageDiff::by_stage` stays keyed by stage (tokens remember their stage). In `flushMemoryUsageDiff` and at the end of `scheduleTasksIfNeeded`, replace `stages[i].memory_usage.fetch_add(d)` with `pool_usage[size_t(poolOf(ReadStage(i)))].fetch_add(d, std::memory_order_relaxed);`. In `scheduleTasksIfNeeded`, replace

```cpp
    auto limits = SharedResourcesExt::getLimitsPerReader(*parser_shared_resources, stage.memory_target_fraction, stage.thread_target_fraction);
    size_t memory_usage = stage.memory_usage.load(std::memory_order_relaxed);
```

with

```cpp
    auto limits = poolLimits(poolOf(stage_idx));
    limits.parsing_threads = SharedResourcesExt::getLimitsPerReader(*parser_shared_resources, 1.0, stage.thread_target_fraction).parsing_threads;
    size_t memory_usage = size_t(std::max<ssize_t>(0, pool_usage[size_t(poolOf(stage_idx))].load(std::memory_order_relaxed)));
```

and the same substitution in `flushMemoryUsageDiff`'s `should_schedule` computation. In `collectDeadlockDiagnostics`, print the three pools before the per-stage loop:

```cpp
    result += " pools:";
    for (size_t p = 0; p < NUM_MEMORY_POOLS; ++p)
        result += " " + std::string(magic_enum::enum_name(MemoryPool(p))) + "=" + std::to_string(pool_usage[p].load(std::memory_order_relaxed));
```

- [ ] **Step 4: Setting plumbing**

`FormatFactorySettings.h`:

```cpp
    DECLARE(Double, input_format_parquet_compressed_memory_fraction, 0.35, R"(
Share of `input_format_parquet_memory_high_watermark` the Parquet reader may hold as compressed data
pages that are in flight or waiting to be decoded. This bounds how far ahead of decoding the reader
reads. The rest of the budget (minus 5% for metadata) holds decoded columns, including chunks already
handed to the query pipeline. Range `(0, 0.95)`.
)", 0) \
```

`FormatSettings.h`: `double compressed_memory_fraction = 0.35;`. `FormatFactory.cpp`: copy. `SettingsChangesHistory.cpp`: `{"input_format_parquet_compressed_memory_fraction", 0.35, 0.35, "New setting: share of the Parquet reader memory budget held as compressed pages in flight; replaces the previous fixed per-stage split, which gave the data read 20% of the budget."}`.

- [ ] **Step 5: Build, run the existing Parquet stateless suite**

`ninja clickhouse > build/build_task7.log 2>&1`; then `./tests/clickhouse-test parquet > build/test_task7.log 2>&1` (substring match runs every parquet test). Expected: all `OK`.

- [ ] **Step 6: Commit**

```bash
git add src/Processors/Formats/Impl/Parquet/ReadCommon.h src/Processors/Formats/Impl/Parquet/ReadManager.h src/Processors/Formats/Impl/Parquet/ReadManager.cpp src/Core/FormatFactorySettings.h src/Formats/FormatSettings.h src/Formats/FormatFactory.cpp src/Core/SettingsChangesHistory.cpp
git commit -s -m "Parquet: budget reader memory by lifetime (metadata / compressed / decoded) instead of by stage"
```

### Task 8: Charge delivered chunks to the `Decoded` pool

**Files:**
- Create: `src/Processors/Formats/Impl/Parquet/ChunkMemoryInfo.h`
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.h` (add `std::shared_ptr<std::atomic<ssize_t>> delivered_bytes`)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.cpp` (`read` ~line 1140, where `Chunk chunk(...)` is built)
- Test: `tests/queries/0_stateless/<N>_parquet_memory_cap_honest.sh`

**Interfaces:**
- Produces: `class ChunkMemoryInfo : public ChunkInfoCloneable<ChunkMemoryInfo>` holding `std::shared_ptr<std::atomic<ssize_t>> counter; size_t bytes;`, destructor does `counter->fetch_sub(bytes)`.

- [ ] **Step 1: Write the ChunkInfo**

```cpp
#pragma once
#include <Processors/Chunk.h>
#include <atomic>
#include <memory>

namespace DB::Parquet
{

/// Keeps a delivered chunk's bytes charged to the reader's Decoded pool until the pipeline drops
/// the chunk. The counter is shared with ReadManager so it outlives the reader.
class ChunkMemoryInfo : public ChunkInfoCloneable<ChunkMemoryInfo>
{
public:
    ChunkMemoryInfo(std::shared_ptr<std::atomic<ssize_t>> counter_, size_t bytes_)
        : counter(std::move(counter_)), bytes(bytes_)
    {
        counter->fetch_add(ssize_t(bytes), std::memory_order_relaxed);
    }
    ChunkMemoryInfo(const ChunkMemoryInfo & other) : counter(other.counter), bytes(other.bytes)
    {
        counter->fetch_add(ssize_t(bytes), std::memory_order_relaxed);
    }
    ~ChunkMemoryInfo() override
    {
        counter->fetch_sub(ssize_t(bytes), std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<ssize_t>> counter;
    size_t bytes;
};

}
```

(Check `ChunkInfoCloneable` exists in `src/Processors/Chunk.h`; if the base is plain `ChunkInfo` with a `clone()` virtual, implement `clone()` returning `std::make_shared<ChunkMemoryInfo>(*this)`.)

- [ ] **Step 2: Attach it in `ReadManager::read`**

Add member `std::shared_ptr<std::atomic<ssize_t>> delivered_bytes = std::make_shared<std::atomic<ssize_t>>(0);`. After `chunk.getChunkInfos().add(std::move(row_numbers_info));`:

```cpp
    /// The ColumnData token for this subgroup is released below (clearRowSubgroup), but the columns
    /// live on inside `chunk`. Keep them charged until the pipeline drops the chunk.
    chunk.getChunkInfos().add(std::make_shared<ChunkMemoryInfo>(delivered_bytes, chunk.allocatedBytes()));
```

In `scheduleTasksIfNeeded` and `flushMemoryUsageDiff`, when the pool is `Decoded`, add `delivered_bytes->load()` to `memory_usage` before calling `checkTaskSchedulingLimits`:

```cpp
    if (poolOf(stage_idx) == MemoryPool::Decoded)
        memory_usage += size_t(std::max<ssize_t>(0, delivered_bytes->load(std::memory_order_relaxed)));
```

The privileged-task rule (`is_privileged_task`) guarantees progress when the pool is over budget, so no wake-up from the chunk destructor is needed.

- [ ] **Step 3: Test**

```bash
#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
F="${WORKING_DIR}/wide.parquet"

# 16 row groups, each ~25 MB decoded (8 String columns of 50 bytes x 65k rows).
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${F}', Parquet)
  SELECT number AS k, $(for i in 1 2 3 4 5 6 7 8; do echo -n "repeat(toString(number % 97), 25) AS s$i, "; done) 1 AS z
  FROM numbers(1048576)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 65536"

echo "-- peak memory stays near the high watermark with a slow consumer"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_cap" -q "
  SELECT count() FROM file('${F}', Parquet) WHERE sleepEachRow(0.0001) = 0
  SETTINGS input_format_parquet_memory_high_watermark = 134217728, input_format_parquet_memory_low_watermark = 16777216,
           max_threads = 8, max_block_size = 65536"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT memory_usage < 134217728 * 2
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_cap'"

rm -rf "${WORKING_DIR}"
```

Reference:

```
-- peak memory stays near the high watermark with a slow consumer
1048576
1
```

Before the change this query's `memory_usage` exceeds 2× the watermark (delivered chunks pile up uncharged); verify that by running the test against the pre-task binary once and recording the number in the commit message.

- [ ] **Step 4: Build, run test, commit**

`ninja clickhouse > build/build_task8.log 2>&1`; `./tests/clickhouse-test <N>_parquet_memory_cap_honest > build/test_task8.log 2>&1`. Expected: `OK`.

```bash
git add src/Processors/Formats/Impl/Parquet/ChunkMemoryInfo.h src/Processors/Formats/Impl/Parquet/ReadManager.h src/Processors/Formats/Impl/Parquet/ReadManager.cpp tests/queries/0_stateless/<N>_parquet_memory_cap_honest.*
git commit -s -m "Parquet: keep delivered chunks charged to the reader's memory budget until the pipeline drops them"
```

### Task 9: `Prefetcher::ReadStats` — fitted bandwidth and round-trip time, bytes in flight

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/Prefetcher.h` (public `struct ReadStats`, accessors), `src/Processors/Formats/Impl/Parquet/Prefetcher.cpp` (`runTask`, `publishBytesReady` from Task 2, `scheduleTask`)
- Modify: `src/Common/ProfileEvents.cpp` (`ParquetReadFirstByteMicroseconds`, `ParquetReadTransferMicroseconds`)

**Interfaces:**
- Produces:
  ```cpp
  struct Prefetcher::ReadStats
  {
      /// EWMA (alpha 0.2) of time to the first progress callback (or completion) and of transfer
      /// bandwidth after it, over tasks read from the source (cache-served tasks are excluded:
      /// they complete in one shot with no first-byte gap).
      double rtt_us = 50'000;            // prior: 50 ms
      double bandwidth_bytes_per_us = 64; // prior: ~64 MB/s per stream
      size_t samples = 0;
  };
  ReadStats Prefetcher::readStats() const;          // lock-free snapshot of atomics
  size_t Prefetcher::bytesInFlight() const;         // sum of `length` of Scheduled/Running tasks
  size_t Prefetcher::targetBytesInFlight(size_t concurrency) const; // bandwidth × rtt × concurrency × 2
  ```
- Consumes: `Task::bytes_ready` and `publishBytesReady` (Task 2), `tasks_in_flight`-style accounting is new here (`bytes_in_flight` atomic, add in `scheduleTask`, subtract in `runTask` completion and in `decreaseTaskRefcount` when a `Scheduled` task is dropped — mirror the `tasks_in_flight` pattern from Altinity PR #2275 if you want a reference, but implement bytes, not counts).

- [ ] **Step 1:** add `std::atomic<size_t> bytes_in_flight{0}` and the stats atomics (`std::atomic<double>` not portable for fetch ops — store as `std::atomic<uint64_t>` micro-units and update under `ready_mutex`, which `publishBytesReady` already takes on notify; sampling once per task is cheap). In `runTask`: record `Stopwatch` at start; on the first `publishBytesReady` call for the task (bytes_ready went 0 → >0) record `first_byte_us`; at completion compute `transfer_us = total_us - first_byte_us` and update EWMAs when `task->cached_region` is not set and `read_mode == RandomRead`.
- [ ] **Step 2:** `targetBytesInFlight(c) = size_t(bandwidth_bytes_per_us * rtt_us) * c * 2`, floored at `4 × bytes_per_read_task`.
- [ ] **Step 3:** unit-free check via a stateless test is impractical; verify with `clickhouse local` on a local file that `bytesInFlight()` returns to 0 after a query (add `chassert(bytes_in_flight == 0)` in `~Prefetcher`), and that the two new profile events are non-zero on an S3 read in `03723_parquet_prefetcher_read_big_at` (add them to that test's SELECT? No — new test `<N>_parquet_read_stats.sql` on `s3_conn` asserting `ParquetReadFirstByteMicroseconds > 0`).
- [ ] **Step 4:** build, run `03723`, `<N>_parquet_read_stats`, Phase 1 tests; commit `Parquet: measure per-read first-byte time and bandwidth in the prefetcher`.

### Task 10: Issue controller — pre-issue index and page reads for all row groups under a bytes-in-flight target

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.h` (`issue_queue`, `issue_mutex`, `pumpIssueQueue`, `enqueueRowGroupIndexReads`, `enqueueRowGroupPageReads`)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.cpp` (`init` after row groups are initialised; `finishRowGroupStage` at the `OffsetIndex` transition; `flushMemoryUsageDiff`)
- Modify: `src/Processors/Formats/Impl/Parquet/Reader.h/.cpp` (`planPageReads(RowGroup &, size_t step_idx, std::vector<PlannedRead> &)` as specified in the superseded Task 6 text of this plan's history — reproduce it here: for every subgroup with `rows_pass > 0`, call `determinePagesToPrefetch` for the step's columns and push dictionary/whole-chunk handles)
- Settings: `input_format_parquet_min_bytes_in_flight` (UInt64, default `67108864`) — floor for the fitted target
- Test: `tests/queries/0_stateless/<N>_parquet_issue_controller.sh`

**Design (spec §4.2, "Issue controller").** The stage machine is left intact. What changes is *when reads are started*: instead of each stage's `scheduleTask` calling `startPrefetch` for one row group at a time, a per-`ReadManager` FIFO holds `PlannedRead {stage, row_group_idx, row_subgroup_idx, handles, bytes}` in delivery order, and `pumpIssueQueue` starts them while `prefetcher.bytesInFlight() + planned.bytes ≤ max(prefetcher.targetBytesInFlight(io_threads), min_bytes_in_flight)` or the entry is privileged (`row_group_idx == first_incomplete_row_group`). `startPrefetch` is idempotent, so when a stage task later runs `scheduleTask` for the same handles, it finds them started and charges no memory twice (`if (!handle->memory)`).

- On `init` (after `prefilterAndInitRowGroups`/`initializePrefetches`): for every row group in order, enqueue one `PlannedRead` per index stage with the handles `initializePrefetches` registered: `bloom_filter_header_prefetch`, then `column_index_prefetch`/`offset_index_prefetch` (stage `ColumnIndexAndOffsetIndex`), then `dictionary_page_prefetch` when `use_dictionary_filter`. Charged to the stage they belong to (`diff.cur_stage = stage` around `startPrefetch`), i.e. the `Metadata` pool from Task 7.
- In `finishRowGroupStage` when the row group reaches `OffsetIndex` (subgroups exist): `reader.planPageReads(row_group, firstStep(), planned)` and enqueue, charged to `ColumnDataPrefetch` (`Compressed` pool). Later steps' pages are enqueued from `finishRowSubgroupStage`'s `OffsetIndex` case exactly as today via `scheduleTask`.
- `pumpIssueQueue(diff)` is called at the end of `init`, after each enqueue, and from `flushMemoryUsageDiff` whenever a `Compressed` or `Metadata` deallocation is flushed (bytes in flight dropped).
- Profile events: `ParquetPlannedReads`, `ParquetIssueQueueStalls`, `ParquetBytesInFlightTarget` (gauge-like: increment by the target once per pump; used only for diagnostics), added to `collectDeadlockDiagnostics` output with the queue length.

- [ ] **Step 1:** implement `planPageReads` and `PlannedRead` in `Reader`; `enqueue*`/`pumpIssueQueue` in `ReadManager`; wire the three call sites. Keep `scheduleTask`'s existing `startPrefetch` calls (they become no-ops for already-started handles).
- [ ] **Step 2:** test — reuse the data generator from Task 6's test (many columns, 3 row groups, page index on, local file with `input_format_parquet_local_file_min_bytes_for_seek = 4194304`). Assert: results identical for `input_format_parquet_min_bytes_in_flight` in `4096`, `67108864`, `1073741824`, with and without a `WHERE` PREWHERE-able filter, `max_parsing_threads` 1 and default; `ParquetIssueQueueStalls > 0` at `4096` and `= 0` at `1 GiB`; `ParquetPlannedReads >= 3 × 2` (3 row groups × ≥2 stages).
- [ ] **Step 3:** run the whole `parquet` stateless subset plus the Phase 1 and patch-up tests; run `03596_parquet_prewhere_page_skip_bug` and `02841_parquet_filter_pushdown` explicitly (PREWHERE drops whole subgroups).
- [ ] **Step 4:** commit `Parquet: pre-issue index and page reads for all row groups under a bytes-in-flight target`.

### Task 11: Validation on vig-test and hand-over to PR #2275

**Files:** none in-repo except the PR description. Scripts: `tmp/vig_cold/run.sh` family (arms are SQL `SETTINGS` clauses; caches dropped before each run).

- [ ] **Step 1: Build a release image or binary for the cluster** — coordinate with the user (the cluster is deployed from CI images `altinityinfra/clickhouse-server:<PR>-26.6.2.…altinityantalya`); pushing the branch to `parquet-v3-read-sizing` produces the image (Step 4), so run Step 4 first with the PR marked draft, then measure, then finalize the description.
- [ ] **Step 2: Warm and true-cold runs**, 2 reps, all 23 queries, arms: base image (`0-26.6.2.…`) vs this branch at defaults. Record per query: wall, `ParquetReadTaskBytes`, `CachedReadBufferReadFromSourceBytes`, `ReadBufferFromS3Bytes`, GETs, `ParquetIssueQueueStalls`, in-flight (`ReadBufferFromS3Microseconds / wall`), `memory_usage`. Acceptance (spec §5, Appendix A): cold q4/q17/q20 in-flight ≥ 60 per node (from 25–29); warm q20 ≤ 1.5 s and cache-disk bytes ≤ 2 GB (from 4.5 s / 32 GB); no query slower than base by more than 5% on warm; `memory_usage` ≤ 1.25 × `input_format_parquet_memory_high_watermark` on the wide-file check from Task 8.
- [ ] **Step 3: If acceptance fails**, stop and report the table — do not tune settings to pass.
- [ ] **Step 4: Push** `git push --force-with-lease altinity parquet-reader-readpath-redesign:parquet-v3-read-sizing` (user-authorized force push; use `--force-with-lease`, never bare `--force`). Update the PR #2275 description from `.github/PULL_REQUEST_TEMPLATE.md`: what changed (Phase 1, patch-up, Phase 2), the measurement tables, `Performance Improvement` category, changelog entry naming every new setting and profile event, `Related:` links to #2266, #2235, upstream #102282 / #103706 / #115816, and the spec path.

## Deferred (not in this plan's execution)

### Phase 3 — Per-subgroup page cursor and parallel subgroup decode (deferred; spec §4.3)

Kept as design notes. Not executed in this plan: no measured workload needs it (spec Appendix A).

#### (deferred) Move the page cursor into `ColumnSubchunk`

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/Reader.h` (`struct ColumnChunk` lines 389-395; `struct ColumnSubchunk`; new `struct PageCursor`)
- Modify: `src/Processors/Formats/Impl/Parquet/Reader.cpp` (`decodePrimitiveColumn` 1364-1560, `skipToRowOrNextPage` 1563-1620, `initializeDataPage` 1665, `skipRowsInPage` 1902, `readRowsInPage` 2051, `decompressPageIfCompressed` 2175)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.cpp` (`runTask` `ColumnData` case: page handle release loop ~line 985)

**Interfaces:**
- Produces:
  ```cpp
  struct Reader::PageCursor
  {
      PageState page;
      size_t next_page_offset = 0;   // used only without offset index (sequential mode)
      size_t data_pages_idx = 0;     // index into ColumnChunk::data_pages corresponding to `page`
      size_t first_page_idx = 0;     // first data_pages index touched by this cursor (for release)
  };
  ```
  `ColumnSubchunk::cursor` (`PageCursor`), `ColumnChunk::sequential_cursor` (`PageCursor`, used when `data_pages.empty()`), `Reader::cursorFor(ColumnChunk &, ColumnSubchunk &) -> PageCursor &`.
- All of `skipToRowOrNextPage`, `initializeDataPage`, `skipRowsInPage`, `readRowsInPage`, `decompressPageIfCompressed`, `createPageDecoder` take `PageCursor & cursor` instead of reading `column.page`.

- [ ] **Step 1: Introduce `PageCursor` and mechanical signature change**

In `Reader.h` replace inside `ColumnChunk`:

```cpp
        PageState page;
        size_t next_page_offset = 0;
        size_t data_pages_idx = 0;
```

with

```cpp
        /// Used only when there is no offset index (`data_pages.empty()`): pages must then be walked
        /// sequentially and subgroups of this row group decode one at a time (RowGroup::sequential_decode).
        PageCursor sequential_cursor;
```

and add `PageCursor cursor;` to `ColumnSubchunk`. Add:

```cpp
    PageCursor & cursorFor(ColumnChunk & column, ColumnSubchunk & subchunk) const
    {
        return column.data_pages.empty() ? column.sequential_cursor : subchunk.cursor;
    }
```

Change every `column.page` / `column.next_page_offset` / `column.data_pages_idx` in `Reader.cpp` to `cursor.page` / `cursor.next_page_offset` / `cursor.data_pages_idx`, passing `PageCursor & cursor` down from `decodePrimitiveColumn` (`PageCursor & cursor = cursorFor(column, subchunk);`). `use_filter_in_decoder` reads `cursor.page.initialized`.

- [ ] **Step 2: Position a fresh cursor from the offset index**

In `skipToRowOrNextPage`, the `!column.data_pages.empty()` branch currently advances `data_pages_idx` forward only. Replace the forward scan with a search, so a cursor starting mid-row-group works:

```cpp
        if (!cursor.page.initialized)
        {
            /// Fresh cursor: position on the page containing row_idx (pages are sorted by end_row_idx).
            auto it = std::upper_bound(column.data_pages.begin(), column.data_pages.end(), *row_idx,
                [](size_t row, const DataPage & p) { return row < p.end_row_idx; });
            cursor.data_pages_idx = size_t(it - column.data_pages.begin());
            cursor.first_page_idx = cursor.data_pages_idx;
        }
        else
        {
            while (cursor.data_pages_idx < column.data_pages.size() &&
                   column.data_pages[cursor.data_pages_idx].end_row_idx <= *row_idx)
                ++cursor.data_pages_idx;
        }
```

- [ ] **Step 3: Page handle release by refcount**

`Reader.h`, `struct DataPage`: add `std::atomic<UInt32> users_remaining {0};`. In `determinePagesToPrefetch`, where a page is pushed for a subgroup (`out.push_back(&page.prefetch)`), add `page.users_remaining.fetch_add(1, std::memory_order_relaxed);`. In `ReadManager::runTask` `ColumnData` case replace

```cpp
                for (size_t i = prev_page_idx; i < column.data_pages_idx; ++i)
                    column.data_pages.at(i).prefetch.reset(&diff);
```

with

```cpp
                PageCursor & cursor = reader.cursorFor(column, row_subgroup.columns.at(task.column_idx));
                /// Pages this subgroup touched: [first_page_idx, data_pages_idx], the last one only if
                /// fully consumed. Release a page when its last user is done.
                size_t last = cursor.data_pages_idx;
                if (cursor.page.initialized && cursor.page.value_idx < cursor.page.num_values)
                    last = last == 0 ? 0 : last; // still mid-page; that page is released by its later user
                for (size_t i = cursor.first_page_idx; i < column.data_pages.size() && i <= last; ++i)
                {
                    DataPage & page = column.data_pages[i];
                    if (i == cursor.data_pages_idx && cursor.page.initialized && cursor.page.value_idx < cursor.page.num_values)
                        break;
                    if (page.users_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        page.prefetch.reset(&diff);
                }
```

In sequential mode (`data_pages.empty()`) nothing changes: the whole-chunk handle is released in `clearColumnChunk`.

- [ ] **Step 4: Dictionary initialised once under a mutex**

`ColumnChunk`: add `std::mutex dictionary_mutex;`. In `ReadManager::runTask` `ColumnData` case replace the `decodeDictionaryPage` block with:

```cpp
                if (!column.dictionary.isInitialized() && column.dictionary_page_prefetch)
                {
                    std::lock_guard lock(column.dictionary_mutex);
                    if (!column.dictionary.isInitialized() && !reader.decodeDictionaryPage(column, column_info))
                        column.dictionary_page_prefetch.reset(&diff);
                }
```

`Dictionary::isInitialized` must be an acquire load of an atomic flag set last in `decodeDictionaryPageImpl`; change `Dictionary`'s flag to `std::atomic<bool>` if it is a plain `bool`.

- [ ] **Step 5: Build, run the Parquet suite**

`ninja clickhouse > build/build_task7.log 2>&1`; `./tests/clickhouse-test parquet > build/test_task7.log 2>&1`. Expected: all `OK` — behaviour is unchanged so far (still one subgroup at a time).

- [ ] **Step 6: Commit**

```bash
git add src/Processors/Formats/Impl/Parquet/Reader.h src/Processors/Formats/Impl/Parquet/Reader.cpp src/Processors/Formats/Impl/Parquet/ReadManager.cpp
git commit -s -m "Parquet: give each row subgroup its own page cursor"
```

#### (deferred) Admit several subgroups of a row group

**Files:**
- Modify: `src/Processors/Formats/Impl/Parquet/Reader.h` (`struct RowGroup`: add `bool sequential_decode`, `std::atomic<size_t> subgroups_in_progress`, `std::atomic<size_t> subgroups_decoded_remaining`, `std::atomic<size_t> delivery_cursor`; `struct RowSubgroup`: add `std::atomic<bool> ready_for_delivery`)
- Modify: `src/Processors/Formats/Impl/Parquet/Reader.cpp` (`intersectColumnIndexResultsAndInitSubgroups` ~line 1083: set `sequential_decode`)
- Modify: `src/Processors/Formats/Impl/Parquet/ReadManager.cpp` (`finishRowSubgroupStage` main-step branch and the "start next subgroup" loop; `is_privileged_task` in `scheduleTasksIfNeeded`)
- Settings: `input_format_parquet_parallel_subgroups` (default 2)
- Test: `tests/queries/0_stateless/<N>_parquet_parallel_subgroups.sh`

**Interfaces:**
- Consumes: `PageCursor` per subchunk (Task 7), issue queue (Task 6).
- Produces: `RowGroup::sequential_decode` — true when any selected column has no offset index or an inline dictionary page (`!meta_data.__isset.dictionary_page_offset && dictionary present`), or `parallel_subgroups == 1`.

- [ ] **Step 1: Setting**

`DECLARE(UInt64, input_format_parquet_parallel_subgroups, 2, R"(How many row subgroups of one Parquet row group may be decoded at the same time. Requires a page index in the file; without one, or with `1`, subgroups are decoded one after another as before. Output order is unchanged.)", 0)` plus the four mirrors (see Global Constraints). History entry: `{"input_format_parquet_parallel_subgroups", 1, 2, "New setting: decode up to N row subgroups of one Parquet row group concurrently when the file has a page index. 1 restores the previous sequential behavior."}`.

- [ ] **Step 2: Decide `sequential_decode` per row group**

At the end of `intersectColumnIndexResultsAndInitSubgroups`:

```cpp
    row_group.sequential_decode = options.format.parquet.parallel_subgroups <= 1;
    for (size_t i = 0; i < primitive_columns.size() && !row_group.sequential_decode; ++i)
    {
        const ColumnChunk & c = row_group.columns.at(i);
        /// No offset index -> pages must be walked in order. Inline dictionary page (not declared in
        /// metadata) -> the first data-page walk finds it, which only works sequentially.
        if (!c.offset_index_prefetch && c.offset_index.page_locations.empty())
            row_group.sequential_decode = true;
        else if (!c.meta->meta_data.__isset.dictionary_page_offset && c.meta->meta_data.__isset.encoding_stats
                 && std::any_of(c.meta->meta_data.encoding_stats.begin(), c.meta->meta_data.encoding_stats.end(),
                        [](const auto & e) { return e.encoding == parq::Encoding::RLE_DICTIONARY || e.encoding == parq::Encoding::PLAIN_DICTIONARY; }))
            row_group.sequential_decode = true;
    }
    row_group.subgroups_decoded_remaining.store(row_group.subgroups.size());
```

- [ ] **Step 3: Admission loop**

In `finishRowSubgroupStage`, the main-step-finished branch (`step_idx == 0`) becomes:

```cpp
                row_subgroup.stage.store(ReadStage::Deliver, std::memory_order::relaxed);
                row_subgroup.ready_for_delivery.store(true, std::memory_order_release);
                row_group.subgroups_in_progress.fetch_sub(1);
                pushReadySubgroupsInOrder(row_group_idx);   // see below
                if (row_group.subgroups_decoded_remaining.fetch_sub(1) == 1)
                    for (size_t i = 0; i < reader.primitive_columns.size(); ++i)
                        clearColumnChunk(row_group.columns.at(i), diff);
                break;
```

```cpp
void ReadManager::pushReadySubgroupsInOrder(size_t row_group_idx)
{
    RowGroup & row_group = reader.row_groups[row_group_idx];
    std::lock_guard lock(delivery_mutex);
    size_t cur = row_group.delivery_cursor.load();
    while (cur < row_group.subgroups.size())
    {
        RowSubgroup & sg = row_group.subgroups[cur];
        ReadStage st = sg.stage.load(std::memory_order_acquire);
        if (st == ReadStage::Deallocated) { ++cur; continue; }            // filtered out, nothing to deliver
        if (!sg.ready_for_delivery.exchange(false)) break;                 // not decoded yet, or already queued
        delivery_queue.push(Task {.stage = ReadStage::Deliver, .row_group_idx = row_group_idx, .row_subgroup_idx = cur});
        ++cur;
    }
    row_group.delivery_cursor.store(cur);
    delivery_cv.notify_one();
}
```

The "start next subgroup" loop after the switch becomes: admit while `subgroups_in_progress < limit`, where `limit = row_group.sequential_decode ? 1 : options.parallel_subgroups`:

```cpp
    const size_t limit = row_group.sequential_decode ? 1 : reader.options.format.parquet.parallel_subgroups;
    while (true)
    {
        size_t in_progress = row_group.subgroups_in_progress.load();
        if (in_progress >= limit)
            break;
        size_t idx = row_group.read_ptr.load();
        if (idx >= row_group.subgroups.size())
            break;
        if (!row_group.read_ptr.compare_exchange_strong(idx, idx + 1))
            continue;
        RowSubgroup & next = row_group.subgroups[idx];
        if (next.filter.rows_pass == 0)
        {
            next.stage.store(ReadStage::Deallocated);
            clearRowSubgroup(next, diff);
            row_group.subgroups_decoded_remaining.fetch_sub(1);
            pushReadySubgroupsInOrder(row_group_idx);
            advanceDeliveryPtrIfNeeded(row_group_idx, diff);
            continue;
        }
        row_group.subgroups_in_progress.fetch_add(1);
        next.stage.store(ReadStage::OffsetIndex);
        addTasksToReadColumns(row_group_idx, idx, ReadStage::OffsetIndex, firstStepIdx(), diff);
    }
```

with `size_t ReadManager::firstStepIdx() const { return reader.steps.empty() ? 0 : 1; }`. The PREWHERE-drops-all-rows case (`rows_pass` becomes 0 after `applyPrewhere`) must now be handled explicitly in the `ColumnData` case: `if (row_subgroup.filter.rows_pass == 0) { row_subgroup.stage.store(Deallocated); clearRowSubgroup(...); subgroups_in_progress--; subgroups_decoded_remaining--; pushReadySubgroupsInOrder; advanceDeliveryPtrIfNeeded; break; }` — do not rely on the admission loop revisiting it.

`is_privileged_task` in `scheduleTasksIfNeeded`: replace `return row_group.read_ptr.load() == row_group.delivery_ptr.load();` with `return row_group.subgroups_in_progress.load() <= 1;` (the single in-flight subgroup of the first incomplete row group must always be schedulable).

- [ ] **Step 4: Build and run the whole Parquet suite plus the earlier new tests**

`ninja clickhouse > build/build_task8.log 2>&1`; `./tests/clickhouse-test parquet > build/test_task8.log 2>&1`. Expected: all `OK`.

- [ ] **Step 5: Test**

Create via `add-test parquet_parallel_subgroups.sh`; same data generator as Task 6's test but with 1 row group of 300000 rows (`output_format_parquet_row_group_size = 300000`), plus a second file written with `output_format_parquet_write_page_index = 0`. Queries `Q1`/`Q2` from Task 6 for `parallel_subgroups` in `1 2 8`, `max_parsing_threads` in `1 8`, both files, `input_format_parquet_max_block_size = 4096`. Then:

```bash
echo "-- parallel decode happened with a page index and did not without one"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', ''), ProfileEvents['ParquetParallelSubgroups'] > 0
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase()
    AND query_id IN ('${CLICKHOUSE_TEST_UNIQUE_NAME}_idx8', '${CLICKHOUSE_TEST_UNIQUE_NAME}_noidx8', '${CLICKHOUSE_TEST_UNIQUE_NAME}_idx1')
  ORDER BY 1"
```

with `ParquetParallelSubgroups` incremented in the admission loop whenever `in_progress >= 1` at admission time. Expected reference tail: `idx1	0`, `idx8	1`, `noidx8	0`.

- [ ] **Step 6: Commit**

```bash
git add src/Processors/Formats/Impl/Parquet/ src/Core/FormatFactorySettings.h src/Formats/FormatSettings.h src/Formats/FormatFactory.cpp src/Core/SettingsChangesHistory.cpp src/Common/ProfileEvents.cpp tests/queries/0_stateless/<N>_parquet_parallel_subgroups.*
git commit -s -m "Parquet: decode several row subgroups of a row group concurrently when the file has a page index"
```

#### (deferred) Performance evidence — superseded by Task 11

**Files:** none in-repo; results go into the PR descriptions.

- [ ] **Step 1: Single big remote file** — the July harness: one ~40 GB wide Parquet file on S3, `INSERT INTO FUNCTION null(...) SELECT * FROM s3(...)`, interleaved n≥5, arms: base `antalya-26.6`, Phase 1, Phase 1+2, Phase 1+2+3. Record wall time, `ParquetFetchWaitTimeMicroseconds`, `ParquetPartialReadsServed`, `ParquetIssueQueueStalls`, peak `memory_usage`.
- [ ] **Step 2: Many small files** — IcebergBench q01–q23 on the same arms (this is where #2275's `max_active_files` was aimed; the compressed pool must not regress it).
- [ ] **Step 3: Local file, default settings** — `hits.parquet` full scan; expect parity within noise (no regressions for the local case).
- [ ] **Step 4:** Put the tables in each phase's PR description under `Performance Improvement`.

---


## Self-review notes

- Spec §4.1 → Tasks 1–3. §4.2c-A/B → Tasks 4–5. §4.2 interim coalescing → Task 6. §4.2 pools/honest cap → Tasks 7–8. §4.2 bytes-in-flight + issue controller → Tasks 9–10. §5 invariants 1–3 → Tasks 3, 6, 10 tests; invariant 4 → Task 8 test; invariants 6–7 → Tasks 4–5 test; invariant 5 → run the new tests under a TSan build before pushing. §4.2b (cross-file metadata prefetch) has no task yet — add after Task 11's numbers.
- `ReadStage::ColumnDataPrefetch` exists on base `antalya-26.6` (it arrived with #2235). Task 4 maps it to the `Compressed` pool; Task 6 removes it together with every `switch` case naming it (`finishRowGroupStage`, `finishRowSubgroupStage`, `scheduleTask`, `runTask`, `addTasksToReadColumns`).
- Names used across tasks: `PlannedRead`, `planPageReads`, `pumpIssueQueue`, `reads_issued`, `waiting_for_reads`, `PageCursor`, `cursorFor`, `sequential_cursor`, `sequential_decode`, `subgroups_in_progress`, `subgroups_decoded_remaining`, `delivery_cursor`, `ready_for_delivery`, `pushReadySubgroupsInOrder`, `firstStepIdx`, `pool_usage`, `poolLimits`, `poolOf`, `ChunkMemoryInfo`, `delivered_bytes`, `publishBytesReady`, `waitForBytes`, `requestLength`, `io_threads`.
