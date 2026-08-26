#!/usr/bin/env bash
# Tags: no-fasttest

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
REL_DIR="${CLICKHOUSE_TEST_UNIQUE_NAME}"
WORKING_DIR="${USER_FILES_PATH}/${REL_DIR}"
mkdir -p "${WORKING_DIR}"
DATA_FILE="${WORKING_DIR}/kinds.parquet"

# 1000 rows, 100 rows per row group => 10 row groups. Per chunk:
#   n_all    - every row NULL                        -> all-default (sparse), pages not read
#   n_sparse - 7 in 1 row of 100, NULL otherwise     -> single value + 99% nulls -> sparse, values not decoded
#   n_dense  - NULL in 1 row of 100, 7 otherwise     -> single value + 1% nulls  -> dense Nullable, values not decoded
#   s_sparse - 'x' in 1 row of 100, NULL otherwise   -> BYTE_ARRAY with exact min/max flags -> sparse
#   k        - varies                                -> normal decode
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${DATA_FILE}', Parquet)
  SELECT
    number AS k,
    NULL::Nullable(Int64) AS n_all,
    if(number % 100 = 0, 7, NULL)::Nullable(Int64) AS n_sparse,
    if(number % 100 = 0, NULL, 7)::Nullable(Int64) AS n_dense,
    if(number % 100 = 0, 'x', NULL)::Nullable(String) AS s_sparse
  FROM numbers(1000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 100
"

STRUCTURE="k UInt64, n_all Nullable(Int64), n_sparse Nullable(Int64), n_dense Nullable(Int64), s_sparse Nullable(String)"
FULL_HASH="SELECT sum(cityHash64(k, ifNull(n_all, -1), ifNull(n_sparse, -1), ifNull(n_dense, -1), ifNull(s_sparse, '<null>'))), count(n_all), countIf(n_sparse = 7), count(n_sparse), countIf(n_dense IS NULL), count(n_dense), countIf(s_sparse = 'x'), sum(k) FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}')"

qid_on="${CLICKHOUSE_TEST_UNIQUE_NAME}_on"
qid_dense="${CLICKHOUSE_TEST_UNIQUE_NAME}_dense"
qid_off="${CLICKHOUSE_TEST_UNIQUE_NAME}_off"

echo "-- optimization on (sparse + dense kinds)"
${CLICKHOUSE_CLIENT} --query_id="${qid_on}" -q "${FULL_HASH}"
echo "-- optimization on, sparse disabled via ratio = 1 (dense kind only)"
${CLICKHOUSE_CLIENT} --query_id="${qid_dense}" -q "${FULL_HASH} SETTINGS input_format_parquet_constant_column_sparse_ratio = 1"
echo "-- optimization off (must be identical)"
${CLICKHOUSE_CLIENT} --query_id="${qid_off}" -q "${FULL_HASH} SETTINGS input_format_parquet_use_constant_column_optimization = 0"

echo "-- filters and aggregation over sparse / all-null columns"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE n_sparse = 7"
${CLICKHOUSE_CLIENT} -q "SELECT count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE n_sparse IS NULL AND n_all IS NULL"
${CLICKHOUSE_CLIENT} -q "SELECT n_sparse, s_sparse, count() FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') GROUP BY 1, 2 ORDER BY 1, 2"
${CLICKHOUSE_CLIENT} -q "SELECT k, n_all, n_sparse, n_dense, s_sparse FROM file('${DATA_FILE}', Parquet, '${STRUCTURE}') WHERE k IN (0, 1, 100, 999) ORDER BY k"

echo "-- null_as_default with non-nullable output types"
${CLICKHOUSE_CLIENT} -q "
  SELECT sum(n_all), sum(n_sparse), countIf(n_sparse = 0), sum(n_dense), countIf(s_sparse = ''), countIf(s_sparse = 'x')
  FROM file('${DATA_FILE}', Parquet, 'k UInt64, n_all Int64, n_sparse Int64, n_dense Int64, s_sparse String')
  SETTINGS input_format_null_as_default = 1
"

echo "-- DEFAULT expressions fill the null rows (AddingDefaultsTransform over sparse columns)"
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_kinds_file"
${CLICKHOUSE_CLIENT} -q "
  CREATE TABLE t_kinds_file (k UInt64, n_all Int64 DEFAULT 5, n_sparse Int64 DEFAULT 9, n_dense Int64 DEFAULT 11, s_sparse String DEFAULT 'd')
  ENGINE = File(Parquet, '${REL_DIR}/kinds.parquet')
"
${CLICKHOUSE_CLIENT} -q "SELECT sum(n_all), sum(n_sparse), sum(n_dense), countIf(s_sparse = 'd'), countIf(s_sparse = 'x') FROM t_kinds_file SETTINGS input_format_null_as_default = 1"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_kinds_file"

echo "-- LowCardinality output cannot be sparse: falls back to dense / normal decode with the same result"
${CLICKHOUSE_CLIENT} -q "
  SELECT count(n_all), countIf(n_sparse = 7), count(n_sparse), countIf(s_sparse = 'x'), count(s_sparse)
  FROM file('${DATA_FILE}', Parquet, 'k UInt64, n_all LowCardinality(Nullable(Int64)), n_sparse LowCardinality(Nullable(Int64)), n_dense LowCardinality(Nullable(Int64)), s_sparse LowCardinality(Nullable(String))')
  SETTINGS allow_suspicious_low_cardinality_types = 1
"

echo "-- profile events: all-null chunks skip pages; single-value-plus-nulls chunks skip values; nothing when disabled"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT ProfileEvents['ParquetConstantColumnChunks'], ProfileEvents['ParquetConstantColumnChunksWithNulls']
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND query_id = '${qid_on}' AND type = 'QueryFinish' AND current_database = currentDatabase();
  SELECT ProfileEvents['ParquetConstantColumnChunks'], ProfileEvents['ParquetConstantColumnChunksWithNulls']
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND query_id = '${qid_dense}' AND type = 'QueryFinish' AND current_database = currentDatabase();
  SELECT ProfileEvents['ParquetConstantColumnChunks'], ProfileEvents['ParquetConstantColumnChunksWithNulls']
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600
    AND query_id = '${qid_off}' AND type = 'QueryFinish' AND current_database = currentDatabase();
"

rm -rf "${WORKING_DIR}"
