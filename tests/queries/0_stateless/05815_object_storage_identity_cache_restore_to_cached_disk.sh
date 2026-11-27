#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: requires S3

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# RESTORE copies backup objects natively into the destination S3 disk. The `s3_cache` disk is a filesystem cache
# layer over the `s3_disk`, so the destination object storage is wrapped. The restore must still use the native
# copy and must not fail because the cache layer hides the S3 object storage.

set -e

client_opts=(
  --allow_repeated_settings
  --send_logs_level 'error'
)

$CLICKHOUSE_CLIENT "${client_opts[@]}" -m -q "
    DROP TABLE IF EXISTS src_s3_cache;
    DROP TABLE IF EXISTS restored_s3_cache;
    CREATE TABLE src_s3_cache (n UInt64) ENGINE = MergeTree ORDER BY tuple() SETTINGS disk = 's3_cache';
    INSERT INTO src_s3_cache SELECT number FROM numbers(10);
"

$CLICKHOUSE_CLIENT "${client_opts[@]}" -q "SELECT disk_name FROM system.parts WHERE database = currentDatabase() AND table = 'src_s3_cache' AND active LIMIT 1"

$CLICKHOUSE_CLIENT --format Null "${client_opts[@]}" -q "BACKUP TABLE src_s3_cache TO S3(s3_conn, 'backups/$CLICKHOUSE_DATABASE/restore_to_cached_disk')"

query_id=$(random_str 10)
$CLICKHOUSE_CLIENT --format Null "${client_opts[@]}" --query_id "$query_id" -q "RESTORE TABLE src_s3_cache AS restored_s3_cache FROM S3(s3_conn, 'backups/$CLICKHOUSE_DATABASE/restore_to_cached_disk') SETTINGS allow_s3_native_copy = true"

$CLICKHOUSE_CLIENT "${client_opts[@]}" -q "SELECT count(), sum(n) FROM restored_s3_cache"

$CLICKHOUSE_CLIENT "${client_opts[@]}" -m -q "
    SYSTEM FLUSH LOGS query_log;
    SELECT ProfileEvents['S3CopyObject'] > 0 FROM system.query_log WHERE type = 'QueryFinish' AND event_date >= yesterday() AND event_time >= now() - 600 AND current_database = '$CLICKHOUSE_DATABASE' AND query_id = '$query_id';
"

$CLICKHOUSE_CLIENT "${client_opts[@]}" -m -q "
    DROP TABLE IF EXISTS src_s3_cache;
    DROP TABLE IF EXISTS restored_s3_cache;
"
