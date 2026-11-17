#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the filesystem cache are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A filesystem-cache hit does not issue a GET, so it cannot see that an object was replaced. The read
# must revalidate the identity before using its ETag as the cache key. An unchanged object is still
# served from the cache. A replaced object is read again.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 1, filesystem_cache_name = 'cache_for_readbigat', use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 expected rows, $3 head mode, $4 get mode, $5 cache-read mode
read_and_check()
{
    local label=$1
    local expected=$2
    local head_mode=$3
    local get_mode=$4
    local cache_mode=$5
    local qid="idcache_fscache_${label}_${CLICKHOUSE_DATABASE}"
    local data row head gets cache_bytes

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') ORDER BY n SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    [[ $data == "$expected" ]] || fail "${label}: data '${data}', expected '${expected}'"

    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "${label}: flush logs failed"
    row=$(${CLICKHOUSE_CLIENT} -q "
        SELECT
            ProfileEvents['S3HeadObject'],
            ProfileEvents['S3GetObject'],
            ProfileEvents['CachedReadBufferReadFromCacheBytes']
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

    IFS=$'\t' read -r head gets cache_bytes <<< "$row"
    case "$head_mode" in
        ge1) [[ $head -ge 1 ]] || fail "${label}: S3HeadObject=${head}, expected >= 1" ;;
        *) fail "${label}: bad head mode ${head_mode}" ;;
    esac
    case "$get_mode" in
        eq0) [[ $gets == "0" ]] || fail "${label}: S3GetObject=${gets}, expected 0" ;;
        ge1) [[ $gets -ge 1 ]] || fail "${label}: S3GetObject=${gets}, expected >= 1" ;;
        *) fail "${label}: bad get mode ${get_mode}" ;;
    esac
    case "$cache_mode" in
        ge1) [[ $cache_bytes -ge 1 ]] || fail "${label}: CachedReadBufferReadFromCacheBytes=${cache_bytes}, expected >= 1" ;;
        any) ;;
        *) fail "${label}: bad cache mode ${cache_mode}" ;;
    esac
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR FILESYSTEM CACHE 'cache_for_readbigat'" || fail "clear filesystem cache failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 1
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache after insert failed"

read_and_check cold $'1' ge1 ge1 any
read_and_check cached $'1' ge1 eq0 ge1

payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
printf '10\n20\n30\n' > "$payload"
curl -fsS -X PUT "$URL" --upload-file "$payload" >/dev/null || fail "external put failed"

read_and_check replaced $'10\n20\n30' ge1 ge1 any

echo OK
