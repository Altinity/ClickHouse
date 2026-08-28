#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings
# Randomized settings would change the memory watermarks and the number of parsing threads, which
# the issue-queue assertions below depend on.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

USER_FILES_PATH=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.server_settings WHERE name = 'user_files_path'" | sed 's|/$||')
WORKING_DIR="${USER_FILES_PATH}/${CLICKHOUSE_TEST_UNIQUE_NAME}"
mkdir -p "${WORKING_DIR}"
F="${WORKING_DIR}/issue.parquet"

# 3 row groups x 64 columns, page index on (the default), small data pages so that each row group's
# subgroups need several pages per column. The reader plans the index reads of all three row groups
# up front and the data pages of a row group as soon as its offset indexes land, then issues them
# under the bytes-in-flight target.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION file('${F}', Parquet)
  SELECT number AS k, number % 1000 AS v, $(for i in $(seq 1 61); do echo -n "number * $i AS c$i, "; done) toString(number) AS s
  FROM numbers(300000)
  SETTINGS engine_file_truncate_on_insert = 1, output_format_parquet_row_group_size = 100000,
           output_format_parquet_compression_method = 'none', output_format_parquet_data_page_size = 65536"

# Force the local file to behave like object storage: a 4 MiB seek threshold.
BASE="input_format_parquet_local_file_min_bytes_for_seek = 4194304"
# One number, but it needs every column: the whole point of these four arms is a subgroup whose
# pages are worth megabytes.
WIDE="sum(k)$(for i in $(seq 1 61); do echo -n " + sum(c$i)"; done)"

echo "-- same results for every bytes-in-flight target (0 = read-ahead planning off), with and without a filter, 1 and default parsing threads"
for flight in 0 4096 67108864 1073741824; do
  for threads in 1 0; do
    ${CLICKHOUSE_CLIENT} -q "
      SELECT sum(k), sum(v), sum(c31), count() FROM file('${F}', Parquet)
      SETTINGS ${BASE}, input_format_parquet_min_bytes_in_flight = ${flight}, max_parsing_threads = ${threads}"
    ${CLICKHOUSE_CLIENT} -q "
      SELECT sum(k), sum(v), sum(c31), count() FROM file('${F}', Parquet) WHERE v < 100
      SETTINGS ${BASE}, input_format_parquet_min_bytes_in_flight = ${flight}, max_parsing_threads = ${threads}"
  done
done

echo "-- reads are planned ahead; the queue waits when a budget is full, never waits when none is, and plans nothing when disabled"
# Two ways the queue can have to wait, one per budget:
#  * `_stall_target`: the bytes-in-flight target. It is the *larger* of the fitted target and
#    `input_format_parquet_min_bytes_in_flight`, and the fitted one is floored at four read tasks, so
#    a small setting only bites together with a small `input_format_parquet_bytes_per_read_task`
#    (65536 here -> a 256 KiB floor, well under one subgroup's pages).
# All four read every column, so that one subgroup's pages are worth megabytes and the reads are
# numerous enough for the fitted round-trip time to settle at the local file's real (small) value.
#  * `_stall_pool`: the compressed memory pool, capped below one subgroup's worth of pages. Only the
#    next subgroup to be read of the row group next to be delivered bypasses it (see
#    `ReadManager::isPrivilegedRead`), so read-ahead for the other subgroups waits.
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_stall_target" -q "
  SELECT ${WIDE} FROM file('${F}', Parquet)
  SETTINGS ${BASE}, input_format_parquet_min_bytes_in_flight = 4096,
           input_format_parquet_bytes_per_read_task = 65536"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_stall_pool" -q "
  SELECT ${WIDE} FROM file('${F}', Parquet)
  SETTINGS ${BASE}, input_format_parquet_min_bytes_in_flight = 4096,
           input_format_parquet_memory_high_watermark = 16777216, input_format_parquet_memory_low_watermark = 1048576"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_no_stall" -q "
  SELECT ${WIDE} FROM file('${F}', Parquet)
  SETTINGS ${BASE}, input_format_parquet_min_bytes_in_flight = 1073741824"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_disabled" -q "
  SELECT ${WIDE} FROM file('${F}', Parquet)
  SETTINGS ${BASE}, input_format_parquet_min_bytes_in_flight = 0"

${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT
    replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', ''),
    ProfileEvents['ParquetIssueQueueStalls'] > 0,
    -- disabled: nothing is planned, so the controller issues nothing at all.
    -- otherwise: at least 3 row groups x 2 groups of reads (indexes, then data pages)
    if(query_id LIKE '%_disabled', ProfileEvents['ParquetPlannedReads'] = 0, ProfileEvents['ParquetPlannedReads'] >= 6)
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase()
    AND query_id IN ('${CLICKHOUSE_TEST_UNIQUE_NAME}_stall_target', '${CLICKHOUSE_TEST_UNIQUE_NAME}_stall_pool',
                     '${CLICKHOUSE_TEST_UNIQUE_NAME}_no_stall', '${CLICKHOUSE_TEST_UNIQUE_NAME}_disabled')
  ORDER BY 1"

rm -rf "${WORKING_DIR}"
