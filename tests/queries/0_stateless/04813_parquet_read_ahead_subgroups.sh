#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
DATA_FILE="${WORKING_DIR}/ra.parquet"

# 3 row groups of 100k rows, small data pages and a page index, so that each row group is read as many
# subgroups (input_format_parquet_max_block_size below) with several pages per subgroup and column.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${DATA_FILE}', Parquet)
  SELECT
    number AS k,
    number * 7 % 1000 AS v,
    toString(number % 5000) AS s,
    if(number % 3 = 0, NULL, number % 100)::Nullable(UInt8) AS n
  FROM numbers(300000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 100000,
           output_format_parquet_data_page_size = 8192, output_format_parquet_write_page_index = 1
"

STRUCTURE="k UInt64, v UInt64, s String, n Nullable(UInt8)"
# Small subgroups and a low decode watermark, so the scheduler throttles non-first row groups and
# subgroups queue up behind each other (the situation read-ahead is for).
COMMON="input_format_parquet_max_block_size = 4096, input_format_parquet_prefer_block_bytes = 0, input_format_parquet_memory_high_watermark = 4194304, input_format_parquet_memory_low_watermark = 1048576"

FULL="SELECT count(), sum(k), sum(v), sum(cityHash64(s)), sum(n), countIf(n IS NULL) FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')"
FILTERED="SELECT count(), sum(k), sum(cityHash64(s)), sum(n) FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE v < 100 AND n IS NOT NULL"

for ahead in 0 1 3; do
  echo "-- read_ahead_subgroups = ${ahead}"
  ${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_full_${ahead}" -q "${FULL} SETTINGS ${COMMON}, input_format_parquet_read_ahead_subgroups = ${ahead}"
  ${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_filtered_${ahead}" -q "${FILTERED} SETTINGS ${COMMON}, input_format_parquet_read_ahead_subgroups = ${ahead}"
  # Single-threaded parsing exercises the same-thread scheduling path.
  ${CLICKHOUSE_CLIENT} -q "${FILTERED} SETTINGS ${COMMON}, input_format_parquet_read_ahead_subgroups = ${ahead}, max_parsing_threads = 1, max_threads = 1"
done

echo "-- read_ahead_memory_fraction = 0 disables read-ahead even when subgroups > 0"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_nobudget" -q "${FULL} SETTINGS ${COMMON}, input_format_parquet_read_ahead_subgroups = 1, input_format_parquet_read_ahead_memory_fraction = 0"

echo "-- read-ahead fired only when enabled and budgeted (ParquetReadAheadSubgroups > 0)"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', ''), ProfileEvents['ParquetReadAheadSubgroups'] > 0
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase()
    AND query_id IN ('${CLICKHOUSE_TEST_UNIQUE_NAME}_full_0', '${CLICKHOUSE_TEST_UNIQUE_NAME}_full_1', '${CLICKHOUSE_TEST_UNIQUE_NAME}_full_3', '${CLICKHOUSE_TEST_UNIQUE_NAME}_filtered_1', '${CLICKHOUSE_TEST_UNIQUE_NAME}_nobudget')
  ORDER BY 1
"

rm -rf "${WORKING_DIR}"
