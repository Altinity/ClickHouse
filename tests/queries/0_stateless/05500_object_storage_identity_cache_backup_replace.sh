#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache is process-global, and another test may clear it
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A backup overwrites its objects through BackupWriterS3, not S3ObjectStorage. After the first backup is
# read, the identity cache holds that ETag. A second backup to the same URL must drop the entry, or the
# next read sends If-Match for the previous ETag and fails with S3_OBJECT_CHANGED_DURING_READ.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

BACKUP_URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}/"
OBJECT_URL="${BACKUP_URL}.backup"
COMMON_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, use_object_storage_list_objects_cache = 0, s3_validate_etag_on_read = 1"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 head mode (eq0 or ge1)
read_backup_and_check()
{
    local label=$1
    local head_mode=$2
    local qid="idcache_backup_${label}_${CLICKHOUSE_DATABASE}"
    local data row head

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT count() FROM s3('${OBJECT_URL}', 'test', 'testtest', 'LineAsString', 's String') SETTINGS ${COMMON_SETTINGS} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    [[ $data -ge 1 ]] || fail "${label}: data '${data}', expected at least 1 row"

    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "${label}: flush logs failed"
    row=$(${CLICKHOUSE_CLIENT} -q "
        SELECT ProfileEvents['S3HeadObject']
        FROM system.query_log
        WHERE event_date >= yesterday()
            AND event_time >= now() - 600
            AND type = 'QueryFinish'
            AND query_id = '${qid}'
            AND current_database = currentDatabase()
        ORDER BY event_time_microseconds DESC
        LIMIT 1
        FORMAT TabSeparated") || fail "${label}: query_log failed"
    [[ -n $row ]] || fail "${label}: no query_log row"
    head=$row
    case "$head_mode" in
        eq0) [[ $head == "0" ]] || fail "${label}: S3HeadObject=${head}, expected 0" ;;
        ge1) [[ $head -ge 1 ]] || fail "${label}: S3HeadObject=${head}, expected >= 1" ;;
        *) fail "${label}: bad mode ${head_mode}" ;;
    esac
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"
${CLICKHOUSE_CLIENT} -q "CREATE TABLE t (n UInt64) ENGINE = Memory" || fail "create failed"
${CLICKHOUSE_CLIENT} -q "INSERT INTO t VALUES (1)" || fail "insert failed"
${CLICKHOUSE_CLIENT} -q "BACKUP TABLE t TO S3('${BACKUP_URL}', 'test', 'testtest')" >/dev/null || fail "first backup failed"

read_backup_and_check cold ge1
read_backup_and_check warm eq0

${CLICKHOUSE_CLIENT} -q "INSERT INTO t VALUES (2)" || fail "second insert failed"
${CLICKHOUSE_CLIENT} -q "BACKUP TABLE t TO S3('${BACKUP_URL}', 'test', 'testtest')" >/dev/null || fail "second backup failed"

read_backup_and_check replaced ge1

echo OK
