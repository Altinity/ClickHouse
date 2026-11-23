#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: reads files through the mock S3 server at localhost:11111

# Regression test: when `http_latency_aware_host_selection` is enabled, borrowing a pooled
# connection or receiving an S3 response after `SYSTEM DROP DNS CACHE` must use `tryGetResolver`
# rather than forcing a fresh DNS lookup under the endpoint pool mutex.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} -q "
    SELECT value
    FROM system.server_settings
    WHERE name = 'http_latency_aware_host_selection'"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO FUNCTION s3('http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.csv', 'test', 'testtest', 'CSV', 'n UInt64')
    SELECT number FROM numbers(1000)
    SETTINGS s3_truncate_on_insert = 1"

# Warm up the HTTP connection pool so `stored_connections` is populated.
${CLICKHOUSE_CLIENT} -q "
    SELECT count(), sum(n)
    FROM s3('http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.csv', 'test', 'testtest', 'CSV', 'n UInt64')
    SETTINGS use_object_storage_list_objects_cache = 0, use_query_condition_cache = 0"

# Drop the cached `HostResolver` while pooled connections remain in `stored_connections`, then
# read again over the reused connection.
${CLICKHOUSE_CLIENT} -q "SYSTEM DROP DNS CACHE"

${CLICKHOUSE_CLIENT} -q "
    SELECT count(), sum(n)
    FROM s3('http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}.csv', 'test', 'testtest', 'CSV', 'n UInt64')
    SETTINGS use_object_storage_list_objects_cache = 0, use_query_condition_cache = 0"
