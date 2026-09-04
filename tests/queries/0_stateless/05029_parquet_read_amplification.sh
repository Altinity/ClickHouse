#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
F="${WORKING_DIR}/amp.parquet"

# 64 columns, 16 row groups (output_format_parquet_row_group_size = 12500); we read k and c31 only,
# so useful bytes per row group are ~2 chunks of ~130 KB out of ~8 MB, and the byte gap between the
# k and c31 column chunks (~3.8 MB, 30 unread columns in between) sits just inside the 4 MiB
# min-bytes-for-seek forced below, so an uncapped reader bridges it and reads the whole row group.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${F}', Parquet)
  SELECT number AS k, $(for i in $(seq 1 62); do echo -n "number * $i AS c$i, "; done) toString(number) AS s
  FROM numbers(200000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 12500,
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
