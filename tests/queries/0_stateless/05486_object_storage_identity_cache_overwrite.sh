#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache is process-global, and another test may clear it
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# Overwriting an object drops its identity-cache entry. The next read misses and HEADs, so it
# sees the new size. The second insert is longer than the first. s3_validate_etag_on_read is off,
# so a stale cached size would be the read bound and the new rows would be truncated.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 0, use_object_storage_list_objects_cache = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 expected rows (tab-separated query result), $3 head mode, $4 hits mode, $5 misses mode
read_and_check()
{
    local label=$1
    local expected=$2
    local head_mode=$3
    local hits_mode=$4
    local misses_mode=$5
    local qid="idcache_ow_${label}_${CLICKHOUSE_DATABASE}"
    local data row head attrs hits misses lists

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') ORDER BY n SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    [[ $data == "$expected" ]] || fail "${label}: data '${data}', expected '${expected}'"

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
    check_count "$label" S3GetObjectAttributes "$attrs" eq0
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
        *) fail "${label}: bad mode ${mode}" ;;
    esac
}

insert_values()
{
    local values=$1
    ${CLICKHOUSE_CLIENT} -q "
        INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
        ${values}
        SETTINGS s3_truncate_on_insert = 1" || fail "insert failed: ${values}"
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

insert_values "SELECT 1"
read_and_check short_cold $'1' ge1 eq0 ge1
read_and_check short_warm $'1' eq0 ge1 eq0

# No CLEAR. The upload drops the cached identity, so the next read misses and returns every new row.
insert_values "SELECT 10 UNION ALL SELECT 20 UNION ALL SELECT 30"
read_and_check overwritten $'10\n20\n30' ge1 eq0 ge1
read_and_check overwritten_warm $'10\n20\n30' eq0 ge1 eq0

echo OK
