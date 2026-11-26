#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE clears the process-global cache, and the test reads the global metric
# Tag no-parallel-replicas: the cache is per server, and the read is spread across replicas otherwise

# The identity cache is weighted by its memory use, and the key of an entry (endpoint, bucket and object path) is part
# of that memory. With a long object path the key is most of an entry, so the weight has to count it. Otherwise the
# cache holds several times more memory than `object_storage_identity_cache_size` and `ObjectStorageIdentityCacheBytes`
# report. Here one entry with a 100-byte path, under the test name prefix, must weigh at least 400 bytes. Without the key the
# weight is about 250 bytes. The mock S3 server rejects longer keys, so the path stays short.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

LONG_NAME=$(printf 'p%.0s' $(seq 1 100))
URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_${LONG_NAME}.tsv"
READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SELECT 42
    SETTINGS s3_truncate_on_insert = 1" || fail "insert failed"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear failed"

# A cold read stores exactly one entry: the identity of this object.
data=$(${CLICKHOUSE_CLIENT} -q "SELECT n FROM s3('${URL}', 'test', 'testtest', 'TSV', 'n UInt64') SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") \
    || fail "select failed"
[[ $data == "42" ]] || fail "data '${data}'"

bytes=$(${CLICKHOUSE_CLIENT} -q "SELECT value FROM system.metrics WHERE metric = 'ObjectStorageIdentityCacheBytes'") \
    || fail "metric query failed"
[[ $bytes -ge 400 ]] || fail "ObjectStorageIdentityCacheBytes=${bytes} is less than 400, the entry does not count its key"

echo OK
