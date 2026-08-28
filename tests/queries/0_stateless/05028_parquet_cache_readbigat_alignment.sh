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
  local tag=$1 align=$2 extra_settings=${3:-}
  ${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR FILESYSTEM CACHE 'cache_for_readbigat'"
  ${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_${tag}" -q "
    SELECT sum(k), sum(c31) FROM s3(s3_conn, filename = '${FILE}', format = 'Parquet')
    SETTINGS enable_filesystem_cache = 1, filesystem_cache_name = 'cache_for_readbigat',
             filesystem_cache_boundary_alignment = ${align}, remote_read_min_bytes_for_seek = 65536,
             use_parquet_metadata_cache = 0, max_threads = 4${extra_settings}"
}

echo "-- results identical"
# `filesystem_cache_boundary_alignment`'s own default is 0, meaning "no alignment" (see its
# description) - not "inherit the cache's configured alignment". To exercise the cache's actual
# configured 1 MiB `boundary_alignment` (`cache_for_readbigat` in storage_conf.xml) as the "what
# happens without a smart per-query override" baseline, pass it explicitly.
run default 1048576
run small 65536
# Same (loose) 1 MiB alignment as `default`, but with background download of the segments'
# leftover ranges disabled per query: the only difference from `default` is the background-download
# flag, so any drop in `FilesystemCacheBackgroundDownloadQueuePush` is attributable to it.
run nobg 1048576 ", filesystem_cache_allow_background_download = 0"

echo "-- with a 64 KiB alignment the cache downloads at most 3x what the reader asked for; with the cache default (1 MiB) it downloads far more"
${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT replaceOne(query_id, '${CLICKHOUSE_TEST_UNIQUE_NAME}_', '') tag,
         -- 3x, not 1x: even a per-query alignment as small as 64 KiB rounds every tiny column chunk
         -- up to the next 64 KiB, and the reader's own coalescing adds the gaps it reads through, so
         -- some overshoot is expected. The point of the assertion is the separation from the >= 4x
         -- arm below, which is what the cache's 1 MiB alignment costs.
         ProfileEvents['CachedReadBufferReadFromSourceBytes'] <= 3 * ProfileEvents['ParquetReadTaskBytes'] AS tight,
         ProfileEvents['CachedReadBufferReadFromSourceBytes'] >= 4 * ProfileEvents['ParquetReadTaskBytes'] AS loose,
         ProfileEvents['FilesystemCacheBackgroundDownloadQueuePush'] = 0 AS no_bg
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id LIKE '${CLICKHOUSE_TEST_UNIQUE_NAME}_%'
    AND query_id NOT LIKE '%_partial'
  ORDER BY tag"

# Partial readiness on the filesystem-cache path. `CachedOnDiskReadBufferFromFile::readBigAt` calls the
# progress callback once per cache file segment it serves (1 MiB for `cache_for_readbigat`), reporting a
# running total, so a read task spanning many segments becomes readable range by range while it is still
# running. Two row groups of ~25 MB of incompressible-ish strings, uncompressed, coalesced into one read
# task: the first row group's ranges are complete long before the task is.
#
# The 8 MiB data pages are what makes this assertion sensitive: every range the reader requests is then
# larger than the cache's 1 MiB `max_file_segment_size`, so a per-segment progress report (rather than a
# running total) could never advance a task's readiness past one segment - `Prefetcher::publishBytesReady`
# drops non-increasing values - and no range at all could be served before the whole task completed.
PARTIAL_FILE="${CLICKHOUSE_TEST_UNIQUE_NAME}_partial.parquet"
${CLICKHOUSE_CLIENT} -q "
  INSERT INTO FUNCTION s3(s3_conn, filename = '${PARTIAL_FILE}', format = 'Parquet')
  SELECT number AS k, repeat(hex(cityHash64(number)), 8) AS s FROM numbers(400000)
  SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 200000,
           output_format_parquet_compression_method = 'none', output_format_parquet_write_page_index = 1,
           output_format_parquet_data_page_size = 8388608"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR FILESYSTEM CACHE 'cache_for_readbigat'"
echo "-- ranges of a cached multi-segment read are served before the whole read completes"
${CLICKHOUSE_CLIENT} --query_id="${CLICKHOUSE_TEST_UNIQUE_NAME}_partial" -q "
  SELECT count(), sum(k), sum(length(s)) FROM s3(s3_conn, filename = '${PARTIAL_FILE}', format = 'Parquet')
  SETTINGS enable_filesystem_cache = 1, filesystem_cache_name = 'cache_for_readbigat',
           use_page_cache_for_disks_without_file_cache = 0,
           input_format_parquet_bytes_per_read_task = 268435456, use_parquet_metadata_cache = 0, max_threads = 4"

${CLICKHOUSE_CLIENT} -q "
  SYSTEM FLUSH LOGS query_log;
  SELECT ProfileEvents['ParquetPartialReadsServed'] > 0
  FROM system.query_log
  WHERE event_date >= yesterday() AND event_time >= now() - 600 AND type = 'QueryFinish'
    AND current_database = currentDatabase() AND query_id = '${CLICKHOUSE_TEST_UNIQUE_NAME}_partial'"
