#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the schema cache are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# An identity-cache hit returns the stored Last-Modified for `_time`, but that time must not prove
# the object is unchanged. Otherwise `count()` keeps the previous row count after an external replace.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0, use_cache_for_count_from_files = 1, schema_inference_use_cache_for_s3 = 1"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 1
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

# Last-Modified has second resolution. The count is registered in the same second as the upload unless
# we wait, and a time equal to the registration does not reproduce the stale-count hit.
sleep 1

count1=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS}") || fail "first count failed"
[[ $count1 == "1" ]] || fail "first count '${count1}', expected 1"

payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}.tsv"
printf '10\n20\n30\n' > "$payload"
curl -fsS -X PUT "$URL" --upload-file "$payload" >/dev/null || fail "external put failed"

stale_err=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS}" 2>&1) && fail "replaced object: count succeeded with '${stale_err}'"
[[ $stale_err == *"S3_OBJECT_CHANGED_DURING_READ"* ]] || fail "replaced object: unexpected error: ${stale_err}"

count2=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS}") || fail "retry count failed"
[[ $count2 == "3" ]] || fail "retry count '${count2}', expected 3"

echo OK
