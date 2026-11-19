#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache is process-global, and another test may clear it
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A cache hit must return the Last-Modified stored with the identity. Otherwise the second read of
# the same object reports `_time` as NULL.

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

# $1 label, $2 hits mode, $3 misses mode. Prints the _time value.
read_time()
{
    local label=$1
    local hits_mode=$2
    local misses_mode=$3
    local qid="idcache_time_${label}_${CLICKHOUSE_DATABASE}"
    local value row head hits misses

    value=$(${CLICKHOUSE_CLIENT} --query_id "$qid" -q "
        SELECT if(isNull(_time), 'NULL', toString(_time))
        FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
        SETTINGS ${READ_SETTINGS}
        FORMAT TabSeparated") || fail "${label}: select failed"

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
    case "$hits_mode" in
        eq0) [[ $hits == "0" ]] || fail "${label}: Hits=${hits}, expected 0" ;;
        ge1)
            [[ $hits -ge 1 ]] || fail "${label}: Hits=${hits}, expected >= 1"
            [[ $head == "0" ]] || fail "${label}: cache hit still sent S3HeadObject=${head}"
            ;;
    esac
    case "$misses_mode" in
        eq0) [[ $misses == "0" ]] || fail "${label}: Misses=${misses}, expected 0" ;;
        ge1)
            [[ $misses -ge 1 ]] || fail "${label}: Misses=${misses}, expected >= 1"
            [[ $head -ge 1 ]] || fail "${label}: S3HeadObject=${head}, expected >= 1"
            ;;
    esac

    printf '%s' "$value"
}

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 1
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

cold=$(read_time cold eq0 ge1)
warm=$(read_time warm ge1 eq0)

[[ $cold != "NULL" && -n $cold ]] || fail "cold _time is '${cold}'"
[[ $warm == "$cold" ]] || fail "warm _time '${warm}', cold '${cold}'"

echo OK
