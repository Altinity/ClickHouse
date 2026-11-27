#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the parquet metadata cache are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# A Parquet count reads the footer from the parquet metadata cache and does not GET. The cache key
# includes the object ETag. An identity-cache hit must not supply that ETag until a HEAD confirms it,
# or the count keeps the replaced object's row count.
# The footer-only count needs `optimize_count_from_files`. Without it the count reads the file with the GET pinned
# to the cached ETag, and a replaced object fails with `S3_OBJECT_CHANGED_DURING_READ`. The test runner
# randomizes that setting, so the read settings pin it.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.parquet"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0, use_cache_for_count_from_files = 1, schema_inference_use_cache_for_s3 = 1, use_parquet_metadata_cache = 1, optimize_count_from_files = 1, optimize_trivial_count_query = 1"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM DROP PARQUET METADATA CACHE" || fail "drop parquet metadata cache failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'Parquet', 'n UInt64')
    SELECT toUInt64(1)
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

count1=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM s3('${URL}', 'test', 'testtest', 'Parquet', 'n UInt64')
    SETTINGS ${READ_SETTINGS}") || fail "first count failed"
[[ $count1 == "1" ]] || fail "first count '${count1}', expected 1"

payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.parquet"
${CLICKHOUSE_CLIENT} -q "SELECT toUInt64(number) AS n FROM numbers(3) FORMAT Parquet" > "$payload" || fail "local parquet failed"
curl -fsS -X PUT "$URL" --upload-file "$payload" >/dev/null || fail "external put failed"

replaced_qid="idcache_parquet_count_${CLICKHOUSE_DATABASE}"
count2=$(${CLICKHOUSE_CLIENT} --query_id "$replaced_qid" -q "
    SELECT count() FROM s3('${URL}', 'test', 'testtest', 'Parquet', 'n UInt64')
    SETTINGS ${READ_SETTINGS}") || fail "replaced count failed"
[[ $count2 == "3" ]] || fail "replaced count '${count2}', expected 3"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs failed"
heads=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['S3HeadObject']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${replaced_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "query_log failed"
[[ $heads -ge 1 ]] || fail "replaced count did not HEAD (S3HeadObject=${heads})"

echo OK
