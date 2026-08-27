#!/usr/bin/env bash
# Tags: no-fasttest, no-random-settings
# - no-fasttest: needs S3 (s3_conn) and the `cache_for_readbigat` filesystem cache from storage_conf.xml
# - no-random-settings: asserts on read byte counters

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_align.parquet"
# 64 columns x 200k rows, uncompressed, small pages, small row groups (8000 rows, 25 row groups):
# each column chunk is only ~75 KiB, so reading 2 columns touches 25 tiny, far-apart-in-the-file
# chunks per column. A tiny chunk is what makes the cache's 1 MiB boundary alignment (`cache_for_readbigat`
# in storage_conf.xml) balloon the download; a 64 KiB per-query alignment stays close to what was asked for.
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION s3(s3_conn, filename = '${FILE}', format = 'Parquet')
  SELECT number AS k, $(for i in $(seq 1 62); do echo -n "number * $i AS c$i, "; done) toString(number) AS s
  FROM numbers(200000)
  SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 8000,
           output_format_parquet_compression_method = 'none', output_format_parquet_data_page_size = 65536,
           output_format_parquet_write_page_index = 1"

run() {
  local tag=$1 align=$2
  ${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR FILESYSTEM CACHE 'cache_for_readbigat'"
  ${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_${tag}" -q "
    SELECT sum(k), sum(c31) FROM s3(s3_conn, filename = '${FILE}', format = 'Parquet')
    SETTINGS enable_filesystem_cache = 1, filesystem_cache_name = 'cache_for_readbigat',
             filesystem_cache_boundary_alignment = ${align}, remote_read_min_bytes_for_seek = 65536,
             use_parquet_metadata_cache = 0, max_threads = 4"
}

echo "-- results identical"
# `filesystem_cache_boundary_alignment`'s own default is 0, meaning "no alignment" (see its
# description) - not "inherit the cache's configured alignment". To exercise the cache's actual
# configured 1 MiB `boundary_alignment` (`cache_for_readbigat` in storage_conf.xml) as the "what
# happens without a smart per-query override" baseline, pass it explicitly.
run default 1048576
run small 65536

echo "-- with a 64 KiB alignment the cache downloads at most 2x what the reader asked for; with the cache default (1 MiB) it downloads far more"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', '') tag,
         ProfileEvents['CachedReadBufferReadFromSourceBytes'] <= 2 * ProfileEvents['ParquetReadTaskBytes'] AS tight,
         ProfileEvents['CachedReadBufferReadFromSourceBytes'] >= 4 * ProfileEvents['ParquetReadTaskBytes'] AS loose
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id LIKE '${CLICKHOUSE_TEST_UNIQUE_NAME}_%'
  ORDER BY tag"
