#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE clears the process-global cache
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# HeadObject spells an ETag as `"abc"`. GetObjectAttributes returns the same value without the
# quotes. The identity cache and If-Match compare those bytes, so both paths must store the quoted
# spelling. s3_validate_etag_on_read stays on: an unquoted cached ETag is sent as If-Match and the
# read fails even though the object did not change. `_etag` is the value stored for that read.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 extra settings, $3 head mode (eq0|ge1|any), $4 attrs mode, $5 hits mode, $6 misses mode
# prints the _etag on stdout
read_etag()
{
    local label=$1
    local extra=$2
    local head_mode=$3
    local attrs_mode=$4
    local hits_mode=$5
    local misses_mode=$6
    local qid="idcache_etag_${label}_${CLICKHOUSE_DATABASE}"
    local settings data row n etag head attrs hits misses lists

    settings="${READ_SETTINGS}"
    if [[ -n $extra ]]; then
        settings="${settings}, ${extra}"
    fi

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT n, _etag FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') SETTINGS ${settings} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    IFS=$'\t' read -r n etag <<< "$data"
    [[ $n == "42" ]] || fail "${label}: data '${data}'"
    [[ $etag == '"'*'"' ]] || fail "${label}: etag '${etag}' is not quoted"

    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "${label}: flush logs failed"
    row=$(${CLICKHOUSE_CLIENT} -q "
        SELECT
            ProfileEvents['S3HeadObject'],
            ProfileEvents['S3GetObjectAttributes'],
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

    IFS=$'\t' read -r head attrs hits misses lists <<< "$row"
    check_count "$label" S3HeadObject "$head" "$head_mode"
    check_count "$label" S3GetObjectAttributes "$attrs" "$attrs_mode"
    check_count "$label" Hits "$hits" "$hits_mode"
    check_count "$label" Misses "$misses" "$misses_mode"
    [[ $lists == "0" ]] || fail "${label}: S3ListObjects=${lists}, expected 0 (row: ${row})"

    printf '%s' "$etag"
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
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 42
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"
head_etag=$(read_etag head "" ge1 eq0 eq0 ge1) || exit 1

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear before attributes failed"
# GetObjectAttributes may fall back to HEAD, so the HEAD counter is unconstrained.
attrs_etag=$(read_etag attrs "object_storage_identity_cache_fetch_part_offsets = 1" any ge1 eq0 ge1) || exit 1
[[ $attrs_etag == "$head_etag" ]] || fail "etag spelling differs: head '${head_etag}', attributes '${attrs_etag}'"

# The attributes read filled the cache. The next read must reuse that ETag, not HEAD again.
hit_etag=$(read_etag attrs_hit "object_storage_identity_cache_fetch_part_offsets = 1" eq0 eq0 ge1 eq0) || exit 1
[[ $hit_etag == "$head_etag" ]] || fail "cached etag '${hit_etag}' differs from head '${head_etag}'"

echo OK
