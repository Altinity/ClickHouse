#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings
# The test asserts on exact `memory_usage` bounds for a specific `max_block_size` /
# `max_threads` / watermark combination; randomized settings (e.g. `enable_parallel_replicas`,
# a different `max_block_size`) would either invalidate the assertion or break the diagnostic
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
# The WHERE clause references every s<N> column (not just the sleep call) so the reader actually
# decodes all 8 String columns per row group -- otherwise the query needs no column data at all
# and there is nothing for the Decoded pool to charge.
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_cap" -q "
  SELECT count() FROM file('${F}', Parquet)
  WHERE sleepEachRow(0.0001) = 0 AND ($(for i in 1 2 3 4 5 6 7 8; do echo -n "length(s$i) + "; done)0) >= 0
  SETTINGS input_format_parquet_memory_high_watermark = 134217728, input_format_parquet_memory_low_watermark = 16777216,
           max_threads = 8, max_block_size = 65536, function_sleep_max_microseconds_per_block = 10000000"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT memory_usage < 134217728 * 2
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_cap'"

rm -rf "${WORKING_DIR}"
