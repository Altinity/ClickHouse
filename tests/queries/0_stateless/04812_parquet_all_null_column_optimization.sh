#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$($CLICKHOUSE_CLIENT_BINARY --query "select _path,_file from file('nonexist.txt', 'CSV', 'val1 char')" 2>&1 | grep Exception | awk '{gsub("/nonexist.txt","",$9); print $9}')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
DATA_FILE="${WORKING_DIR}/all_null.parquet"

# 1000 rows, 100 rows per row group => 10 row groups. `k` varies; `c_null` is NULL in every row, so
# each of its column chunks has null_count == num_values and no min/max value. The reader
# materializes such chunks directly from statistics without fetching any data pages.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${DATA_FILE}', Parquet)
  SELECT
    number AS k,
    NULL::Nullable(Int64) AS c_null
  FROM numbers(1000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 100
"

STRUCTURE="k UInt64, c_null Nullable(Int64)"

qid_on="${CLICKHOUSE_TEST_UNIQUE_NAME}_on"
qid_off="${CLICKHOUSE_TEST_UNIQUE_NAME}_off"

echo "-- all-null column, optimization on"
${CLICKHOUSE_CLIENT} --query_id="${qid_on}" -q "
  SELECT c_null IS NULL AS is_null, count()
  FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')
  GROUP BY 1
"

echo "-- all-null column, optimization off (must be identical)"
${CLICKHOUSE_CLIENT} --query_id="${qid_off}" -q "
  SELECT c_null IS NULL AS is_null, count()
  FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')
  GROUP BY 1
  SETTINGS input_format_parquet_use_constant_column_optimization = 0
"

echo "-- the varying column is read correctly (not treated as constant)"
${CLICKHOUSE_CLIENT} -q "SELECT sum(k), min(k), max(k), count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')"

echo "-- IS NULL / IS NOT NULL filters on an all-null column"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE c_null IS NULL"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE c_null IS NOT NULL"

echo "-- null_as_default: a non-nullable hint substitutes the default (0) for every row"
${CLICKHOUSE_CLIENT} -q "
  SELECT c_null, count()
  FROM file('${DATA_FILE}', Parquet, 'k UInt64, c_null Int64')
  GROUP BY 1
  SETTINGS input_format_null_as_default = 1
"

echo "-- optimization fired only when enabled"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT ProfileEvents['ParquetConstantColumnChunks'] > 0
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND query_id = '${qid_on}' AND type = 'QueryFinish' AND current_database = currentDatabase();
  SELECT ProfileEvents['ParquetConstantColumnChunks'] = 0
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND query_id = '${qid_off}' AND type = 'QueryFinish' AND current_database = currentDatabase();
"

rm -rf "${WORKING_DIR}"
