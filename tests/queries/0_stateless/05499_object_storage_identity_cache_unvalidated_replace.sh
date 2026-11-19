#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache is process-global, and another test may clear it
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# `s3_validate_etag_on_read = 0` sends the GET with no If-Match. A cached size must not stay the read
# bound: a small file is prefetched up to that size, so a larger external replace would be returned as
# the old prefix. The read HEADs again and returns the new rows. The HEAD also refreshes the entry, so
# the next read with validation on is a hit.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
COMMON_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, use_object_storage_list_objects_cache = 0"
VALIDATED_SETTINGS="${COMMON_SETTINGS}, s3_validate_etag_on_read = 1"
UNVALIDATED_SETTINGS="${COMMON_SETTINGS}, s3_validate_etag_on_read = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

# $1 label, $2 settings, $3 expected rows, $4 head mode (eq0 or ge1)
read_and_check()
{
    local label=$1
    local settings=$2
    local expected=$3
    local head_mode=$4
    local qid="idcache_unvalidated_${label}_${CLICKHOUSE_DATABASE}"
    local data row head

    data=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q \
        "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') ORDER BY n SETTINGS ${settings} FORMAT TabSeparated") \
        || fail "${label}: select failed"
    [[ $data == "$expected" ]] || fail "${label}: data '${data}', expected '${expected}'"

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

payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
printf '1\n' > "$payload"
curl -fsS -X PUT "$URL" --upload-file "$payload" >/dev/null || fail "first put failed"

read_and_check cold "$VALIDATED_SETTINGS" $'1' ge1
read_and_check warm "$VALIDATED_SETTINGS" $'1' eq0

# This PUT does not go through ClickHouse, so the cached identity still says the object is one row.
printf '10\n20\n30\n' > "$payload"
curl -fsS -X PUT "$URL" --upload-file "$payload" >/dev/null || fail "replace put failed"

read_and_check replaced "$UNVALIDATED_SETTINGS" $'10\n20\n30' ge1
read_and_check refreshed "$VALIDATED_SETTINGS" $'10\n20\n30' eq0

echo OK
