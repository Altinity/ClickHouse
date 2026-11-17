#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache is process-global, and another test may clear it
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A replace that does not go through this server leaves the cached size and ETag in place.
# The next read is pinned to that ETag, fails with S3_OBJECT_CHANGED_DURING_READ, and must
# drop the entry. The retry misses, HEADs, and returns the new rows.

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

# $1 label, $2 expected rows (tab-separated query result), $3 head mode, $4 hits mode, $5 misses mode
read_and_check()
{
    local label=$1
    local expected=$2
    local head_mode=$3
    local hits_mode=$4
    local misses_mode=$5
    local qid="idcache_ext_${label}_${CLICKHOUSE_DATABASE}"
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

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 1
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

read_and_check cold $'1' ge1 eq0 ge1
read_and_check warm $'1' eq0 ge1 eq0

# This PUT does not go through S3ObjectStorage::writeObject, so nothing drops the cached identity.
payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
printf '10\n20\n30\n' > "$payload"
curl -fsS -X PUT "$URL" --upload-file "$payload" >/dev/null || fail "external put failed"

stale_err=$(${CLICKHOUSE_CLIENT} -q \
    "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') ORDER BY n SETTINGS ${READ_SETTINGS} FORMAT TabSeparated" 2>&1) \
    && fail "stale: the replaced object was read"
[[ $stale_err == *"S3_OBJECT_CHANGED_DURING_READ"* ]] || fail "stale: unexpected error: ${stale_err}"

read_and_check retried $'10\n20\n30' ge1 eq0 ge1
read_and_check retried_warm $'10\n20\n30' eq0 ge1 eq0

echo OK
