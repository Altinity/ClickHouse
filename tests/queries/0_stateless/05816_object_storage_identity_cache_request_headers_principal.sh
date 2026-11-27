#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE clears the process-global cache
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# The identity cache is keyed per principal. A principal can authenticate with request headers instead of AWS
# credentials, for example `headers('Authorization' = 'Bearer ...')` of the `s3` table function, so two reads of the same
# object with the same credentials but different headers must not share a cached identity: the second principal has to
# issue its own HEAD and miss, otherwise it would learn the size, ETag and Last-Modified of an object it may not be
# allowed to see. The same headers still hit.

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

# $1 label, $2 extra S3 table function argument (may be empty), $3 head mode (eq0|ge1), $4 hits mode, $5 misses mode
read_and_check()
{
    local label=$1
    local extra_arg=$2
    local head_mode=$3
    local hits_mode=$4
    local misses_mode=$5
    local qid="idcache_headers_${label}_${CLICKHOUSE_DATABASE}"
    local data row head hits misses

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64'${extra_arg}) SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    [[ $data == "42" ]] || fail "${label}: data '${data}'"

    ${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "${label}: flush logs failed"
    row=$(${CLICKHOUSE_CLIENT} -q "
        SELECT
            ProfileEvents['S3HeadObject'],
            ProfileEvents['ObjectStorageIdentityCacheHits'],
            ProfileEvents['ObjectStorageIdentityCacheMisses']
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

    IFS=$'\t' read -r head hits misses <<< "$row"
    check_count "$label" S3HeadObject "$head" "$head_mode"
    check_count "$label" Hits "$hits" "$hits_mode"
    check_count "$label" Misses "$misses" "$misses_mode"
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

HEADERS_A=", headers('X-Idcache-Principal' = 'principal_a')"
HEADERS_B=", headers('X-Idcache-Principal' = 'principal_b')"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 42
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

# Principal A misses, then hits with the same headers.
read_and_check a_cold "$HEADERS_A" ge1 eq0 ge1
read_and_check a_warm "$HEADERS_A" eq0 ge1 eq0

# Principal B has the same AWS credentials and different headers: it must not hit A's entry.
read_and_check b_cold "$HEADERS_B" ge1 eq0 ge1
read_and_check b_warm "$HEADERS_B" eq0 ge1 eq0

# No headers at all is a third principal.
read_and_check none_cold "" ge1 eq0 ge1
read_and_check none_warm "" eq0 ge1 eq0

echo OK
