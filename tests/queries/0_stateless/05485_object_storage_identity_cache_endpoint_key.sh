#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE clears the process-global cache
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# The identity cache key includes the endpoint. localhost and 127.0.0.1 reach the same SeaweedFS
# bucket and key, but they are different endpoint strings, so a hit on one must not answer the other.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL_LOCALHOST="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
URL_LOOPBACK="http://127.0.0.1:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 url, $3 head mode (eq0|ge1|any), $4 hits mode, $5 misses mode
read_and_check()
{
    local label=$1
    local url=$2
    local head_mode=$3
    local hits_mode=$4
    local misses_mode=$5
    local qid="idcache_ep_${label}_${CLICKHOUSE_DATABASE}"
    local data row head hits misses lists

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT n FROM s3('${url}', 'test', 'testtest', 'TSV', 'n UInt64') SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    [[ $data == "42" ]] || fail "${label}: data '${data}'"

    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "${label}: flush logs failed"
    row=$(${CLICKHOUSE_CLIENT} -q "
        SELECT
            ProfileEvents['S3HeadObject'],
            ProfileEvents['ObjectStorageIdentityCacheHits'],
            ProfileEvents['ObjectStorageIdentityCacheMisses'],
            ProfileEvents['S3ListObjects']
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

    IFS=$'\t' read -r head hits misses lists <<< "$row"
    check_count "$label" S3HeadObject "$head" "$head_mode"
    check_count "$label" Hits "$hits" "$hits_mode"
    check_count "$label" Misses "$misses" "$misses_mode"
    [[ $lists == "0" ]] || fail "${label}: S3ListObjects=${lists}, expected 0 (row: ${row})"
}

check_count()
{
    local label=$1
    local name=$2
    local got=$3
    local mode=$4
    case "$mode" in
        eq0) [[ $got == "0" ]] || fail "${label}: ${name}=${got}, expected 0" ;;
        ge1) [[ $got -ge 1 ]] || fail "${label}: ${name}=${got}, expected >= 1" ;;
        any) ;;
        *) fail "${label}: bad mode ${mode}" ;;
    esac
}

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL_LOCALHOST}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 42
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

# Drop whatever the INSERT cached. Each endpoint must miss on its own first read.
${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"
read_and_check localhost_cold "$URL_LOCALHOST" ge1 eq0 ge1
read_and_check loopback_cold "$URL_LOOPBACK" ge1 eq0 ge1
read_and_check loopback_warm "$URL_LOOPBACK" eq0 ge1 eq0
read_and_check localhost_warm "$URL_LOCALHOST" eq0 ge1 eq0

echo OK
