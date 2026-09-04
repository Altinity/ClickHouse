#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings

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
