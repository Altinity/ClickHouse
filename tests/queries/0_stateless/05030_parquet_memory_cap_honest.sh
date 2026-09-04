#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings
# The test asserts on `memory_usage` bounds for a specific `max_block_size` / `max_threads` /
# watermark combination; randomized settings (e.g. `enable_parallel_replicas`, a different
# `max_block_size`) would either invalidate the assertion or break the diagnostic
# `system.query_log` SELECT itself.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
F="${WORKING_DIR}/wide.parquet"

# 64 row groups, each ~6 MB decoded (8 String columns of ~50 bytes x 16384 rows).
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${F}', Parquet)
  SELECT number AS k, $(for i in 1 2 3 4 5 6 7 8; do echo -n "repeat(toString(number % 97), 25) AS s$i, "; done) 1 AS z
  FROM numbers(1048576)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 16384"

echo "-- peak memory stays near the high watermark with a slow consumer"
# `sleepEachRow` is the only WHERE condition (it references no column), so the slow, throttled
# consumption comes purely from the filter -- it does not by itself force any column to be read.
# The columns are forced into the delivered chunks by the SELECT list instead: `max(length(s<N>))`
# over every s<N> genuinely needs each column's decoded values to compute its result, unlike a
# filter predicate, which a future optimization could push down into the reader's own PREWHERE and
# then drop from the delivered chunk once evaluated (since `count()` alone needs no column values).
# If that ever happens to the aggregate's inputs too, the columns would still have to survive to
# `Deliver` for the aggregate to read them -- so `allocatedBytes()` keeps meaning something here
# regardless of how the filter itself is executed.
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_cap" -q "
  SELECT count(), $(for i in 1 2 3 4 5 6 7; do echo -n "max(length(s$i)), "; done)max(length(s8))
  FROM file('${F}', Parquet)
  WHERE sleepEachRow(0.0001) = 0
  SETTINGS input_format_parquet_memory_high_watermark = 134217728, input_format_parquet_memory_low_watermark = 16777216,
           max_threads = 8, max_block_size = 65536, function_sleep_max_microseconds_per_block = 10000000"
# Two assertions:
#  1. `memory_usage < 2 x high_watermark` -- the actual memory cap check. The scheduler's admission
#     control is a best-effort, racy approximation (see `ReadManager::scheduleTasksIfNeeded`'s
#     comments): concurrent scheduling decisions can overshoot by some slop, which is exactly what
#     this bound needs margin for. A looser bound was tried and rejected: at this row-group shape,
#     the pre-fix (buggy) build measures ~2.2-2.3x, so any bound at or above that -- including 3x --
#     stops failing on the very code this test exists to catch (empirically confirmed: reverting
#     just the fix and rerunning this exact query measured `memory_usage` well under `3 x
#     high_watermark`). 2x sits with comfortable margin below the pre-fix measurement and above 5
#     repeated post-fix runs (see the fix report for the recorded values).
#  2. `read_bytes > 0` and `result_rows = 1` -- a cheap sanity check that the query actually read
#     column data and produced its one aggregate row, so a change that made the query trivially
#     cheap (e.g. answering from metadata alone) couldn't make assertion 1 pass by measuring nothing.
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT memory_usage < 134217728 * 2, read_bytes > 0, result_rows = 1
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_cap'"

rm -rf "${WORKING_DIR}"
