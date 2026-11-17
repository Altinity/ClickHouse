#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the failpoint are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# GetObjectAttributes does not return Last-Modified. Caching that as "time unknown" makes every later
# `_time` NULL. The failpoint simulates that response. The next read, with part offsets off, must HEAD
# and return the real time.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 0, use_object_storage_list_objects_cache = 0"
FAILPOINT="object_storage_identity_cache_skip_last_modified"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

cleanup()
{
    ${CLICKHOUSE_CLIENT} -q "SYSTEM DISABLE FAILPOINT ${FAILPOINT}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

read_time()
{
    local label=$1
    local extra_settings=$2
    local qid="idcache_time_${label}_${CLICKHOUSE_DATABASE}"
    local value

    value=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q "
        SELECT if(isNull(_time), 'NULL', toString(_time))
        FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
        SETTINGS ${READ_SETTINGS}${extra_settings}
        FORMAT TabSeparated") || fail "${label}: select failed"
    printf '%s' "$value"
}

head_count()
{
    local qid=$1
    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs failed"
    ${CLICKHOUSE_CLIENT} -q "
        SELECT ProfileEvents['S3HeadObject']
        FROM system.query_log
        WHERE event_date >= yesterday()
            AND event_time >= now() - 600
            AND type = 'QueryFinish'
            AND query_id = '${qid}'
            AND current_database = currentDatabase()
        ORDER BY event_time_microseconds DESC
        LIMIT 1
        FORMAT TabSeparated"
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 1
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear after insert failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM ENABLE FAILPOINT ${FAILPOINT}" || fail "enable failpoint failed"

without_time=$(read_time part_offsets ", object_storage_identity_cache_fetch_part_offsets = 1")
[[ $without_time == "NULL" ]] || fail "part-offset fetch stored a time '${without_time}', failpoint did not apply"

${CLICKHOUSE_CLIENT} -q "SYSTEM DISABLE FAILPOINT ${FAILPOINT}" || fail "disable failpoint failed"

later_qid="idcache_time_later_${CLICKHOUSE_DATABASE}"
later=$(${CLICKHOUSE_CLIENT} --query_id "$later_qid" -q "
    SELECT if(isNull(_time), 'NULL', toString(_time))
    FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS}
    FORMAT TabSeparated") || fail "later select failed"
[[ $later != "NULL" && $later != "1970-01-01 00:00:00" && -n $later ]] || fail "later _time is '${later}'"

heads=$(head_count "$later_qid")
[[ $heads -ge 1 ]] || fail "later read did not HEAD (S3HeadObject=${heads})"

echo OK
