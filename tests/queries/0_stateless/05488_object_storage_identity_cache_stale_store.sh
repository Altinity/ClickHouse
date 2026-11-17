#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the pause failpoint are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A metadata read is paused after its HEAD and before it stores the identity. An overwrite in that
# window advances the cache epoch. When the read resumes it must not store the identity it fetched,
# so the next read misses and returns the new rows.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 0, use_object_storage_list_objects_cache = 0"
FAILPOINT="object_storage_identity_cache_pause_before_store"

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

${CLICKHOUSE_CLIENT} -q "SYSTEM ENABLE FAILPOINT ${FAILPOINT}" || fail "enable failpoint failed"

# The read misses, HEADs the current object, and pauses before storing that identity.
${CLICKHOUSE_CLIENT} -q \
    "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') ORDER BY n SETTINGS ${READ_SETTINGS} FORMAT TabSeparated" \
    >/dev/null &
reader_pid=$!

${CLICKHOUSE_CLIENT} -q "SYSTEM WAIT FAILPOINT ${FAILPOINT} PAUSE" || fail "wait for pause failed"

insert_values "SELECT 10 UNION ALL SELECT 20 UNION ALL SELECT 30"

${CLICKHOUSE_CLIENT} -q "SYSTEM DISABLE FAILPOINT ${FAILPOINT}" || fail "disable failpoint failed"
wait "$reader_pid" || fail "in-flight read failed"

qid="idcache_stale_${CLICKHOUSE_DATABASE}"
data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
    "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') ORDER BY n SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") \
    || fail "check select failed"
[[ $data == $'10\n20\n30' ]] || fail "data '${data}', expected the three new rows"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs failed"
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
    FORMAT TabSeparated") || fail "query_log failed"
[[ -n $row ]] || fail "no query_log row"

IFS=$'\t' read -r head hits misses <<< "$row"
check_count check S3HeadObject "$head" ge1
check_count check Hits "$hits" eq0
check_count check Misses "$misses" ge1

echo OK
