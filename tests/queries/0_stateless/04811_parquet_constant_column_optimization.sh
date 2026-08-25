#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$($CLICKHOUSE_CLIENT_BINARY --query "select _path,_file from file('nonexist.txt', 'CSV', 'val1 char')" 2>&1 | grep Exception | awk '{gsub("/nonexist.txt","",$9); print $9}')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
DATA_FILE="${WORKING_DIR}/const.parquet"

# 1000 rows, 100 rows per row group => 10 row groups. `k` varies; the other four columns each hold a
# single value in every row, so their per-chunk min/max statistics have min == max and no nulls.
# `c_dt` is written as TIMESTAMP_MILLIS and read back with a DateTime hint, exercising the
# milliseconds -> seconds stats conversion (the value is in the post-cast output domain).
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${DATA_FILE}', Parquet)
  SELECT
    number AS k,
    42::Int64 AS c_int,
    'hello' AS c_str,
    toDateTime('2020-01-02 03:04:05') AS c_dt,
    7::Nullable(Int64) AS c_nullable
  FROM numbers(1000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 100
"

STRUCTURE="k UInt64, c_int Int64, c_str String, c_dt DateTime, c_nullable Nullable(Int64)"

qid_on="${CLICKHOUSE_TEST_UNIQUE_NAME}_on"
qid_off="${CLICKHOUSE_TEST_UNIQUE_NAME}_off"

echo "-- values, optimization on"
${CLICKHOUSE_CLIENT} --query_id="${qid_on}" -q "
  SELECT c_int, c_str, c_dt, c_nullable, count()
  FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')
  GROUP BY 1, 2, 3, 4
"

echo "-- values, optimization off (must be identical)"
${CLICKHOUSE_CLIENT} --query_id="${qid_off}" -q "
  SELECT c_int, c_str, c_dt, c_nullable, count()
  FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')
  GROUP BY 1, 2, 3, 4
  SETTINGS input_format_parquet_use_constant_column_optimization = 0
"

echo "-- the varying column is read correctly (not treated as constant)"
${CLICKHOUSE_CLIENT} -q "SELECT sum(k), min(k), max(k), count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')"

echo "-- filters on a constant column still work"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE c_int = 42"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE c_int = 43"

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
