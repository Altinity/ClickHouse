#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
F="${WORKING_DIR}/floor.parquet"

# 64 columns, row groups of 1000 rows: a column chunk is ~8 KB, so the 30 unread columns between k
# and c31 are ~240 KB -- a large ratio (a read spanning them serves ~16 KB of ~256 KB) but a small
# absolute waste, which is the shape the floor is for. Files whose columns sit a few KB apart should
# be read whole rather than split into a request per column.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${F}', Parquet)
  SELECT number AS k, $(for i in $(seq 1 62); do echo -n "number * $i AS c$i, "; done) toString(number) AS s
  FROM numbers(200000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 1000,
           output_format_parquet_compression_method = 'none', output_format_parquet_data_page_size = 65536"

Q="SELECT sum(k), sum(c31) FROM file('${F}', Parquet)"
# Force the local path to behave like object storage, and pin the ratio so only the floor varies.
BASE="input_format_parquet_local_file_min_bytes_for_seek = 4194304, input_format_parquet_bytes_per_read_task = 16777216, max_threads = 2, input_format_parquet_max_read_amplification = 4, input_format_parquet_coalesce_gap_bytes = 4194304"

echo "-- results identical"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_no_floor" -q "${Q} SETTINGS ${BASE}, input_format_parquet_read_amplification_floor_bytes = 0"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_floor" -q "${Q} SETTINGS ${BASE}, input_format_parquet_read_amplification_floor_bytes = 262144"

# Measured: 792 reads of 3.74 MB without the floor, 397 reads of 3.91 MB with it -- the floor trades a
# little more data for half the requests. Asserted with margin (a third fewer reads) rather than on the
# exact counts, which depend on how the writer lays the row groups out.
echo "-- the floor exempts these reads from the ratio: fewer, larger reads"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  WITH
    (SELECT ProfileEvents['ParquetReadTasks'] FROM system.query_log WHERE event_date >= yesterday() AND type = 'QueryFinish' AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_no_floor') AS tasks_no_floor,
    (SELECT ProfileEvents['ParquetReadTaskBytes'] FROM system.query_log WHERE event_date >= yesterday() AND type = 'QueryFinish' AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_no_floor') AS bytes_no_floor
  SELECT ProfileEvents['ParquetReadTasks'] * 3 < tasks_no_floor * 2, ProfileEvents['ParquetReadTaskBytes'] > bytes_no_floor
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish' AND current_database = currentDatabase()
    AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_floor'"

rm -rf "${WORKING_DIR}"
