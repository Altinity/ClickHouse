#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the filesystem cache are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A filesystem-cache revalidation must HEAD. GetObjectAttributes does not return Last-Modified, and
# storing that result makes this read's `_time` NULL and drops the time already in the identity cache.
# The mock S3 server rejects GetObjectAttributes and falls back to HEAD, so the assertion is the
# request itself: the revalidating read must not call GetObjectAttributes.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
COMMON_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, filesystem_cache_name = 'cache_for_readbigat', use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 extra settings, $3 head mode, $4 attributes mode, $5 get mode, $6 cache mode
# Prints the _time value.
read_time()
{
    local label=$1
    local extra=$2
    local head_mode=$3
    local attributes_mode=$4
    local get_mode=$5
    local cache_mode=$6
    local qid="idcache_fscache_time_${label}_${CLICKHOUSE_DATABASE}"
    local value row head attributes gets cache_bytes

    value=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q "
        SELECT if(isNull(_time), 'NULL', toString(_time))
        FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
        SETTINGS ${COMMON_SETTINGS}${extra}
        FORMAT TabSeparated") || fail "${label}: select failed"

    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "${label}: flush logs failed"
    row=$(${CLICKHOUSE_CLIENT} -q "
        SELECT
            ProfileEvents['S3HeadObject'],
            ProfileEvents['S3GetObjectAttributes'],
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

    IFS=$'\t' read -r head attributes gets cache_bytes <<< "$row"
    case "$head_mode" in
        eq0) [[ $head == "0" ]] || fail "${label}: S3HeadObject=${head}, expected 0" ;;
        ge1) [[ $head -ge 1 ]] || fail "${label}: S3HeadObject=${head}, expected >= 1" ;;
        *) fail "${label}: bad head mode ${head_mode}" ;;
    esac
    case "$attributes_mode" in
        eq0) [[ $attributes == "0" ]] || fail "${label}: S3GetObjectAttributes=${attributes}, expected 0" ;;
        *) fail "${label}: bad attributes mode ${attributes_mode}" ;;
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

    printf '%s' "$value"
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR FILESYSTEM CACHE 'cache_for_readbigat'" || fail "clear filesystem cache failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 1
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache after insert failed"

primed=$(read_time primed ", enable_filesystem_cache = 1, object_storage_identity_cache_fetch_part_offsets = 0" ge1 eq0 ge1 any)
[[ $primed != "NULL" && $primed != "1970-01-01 00:00:00" && -n $primed ]] || fail "primed _time is '${primed}'"

revalidated=$(read_time revalidated ", enable_filesystem_cache = 1, object_storage_identity_cache_fetch_part_offsets = 1" ge1 eq0 eq0 ge1)
[[ $revalidated == "$primed" ]] || fail "revalidated _time is '${revalidated}', primed is '${primed}'"

later=$(read_time later ", enable_filesystem_cache = 0, object_storage_identity_cache_fetch_part_offsets = 0" eq0 eq0 ge1 any)
[[ $later == "$primed" ]] || fail "later _time is '${later}', primed is '${primed}'"

echo OK
