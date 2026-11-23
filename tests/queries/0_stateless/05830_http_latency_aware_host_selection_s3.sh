#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: reads files through the mock S3 server at localhost:11111

# The test server config enables `http_latency_aware_host_selection`. Reads from the S3 endpoint then
# feed the time-to-first-byte of every `GET` and `HEAD` back to the host resolver, and the connection pool
# hands out the pooled connection with the lowest latency. Many parallel reads over reused connections
# must return the same data as with the plain selection.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} -q "
    SELECT value
    FROM system.server_settings
    WHERE name = 'http_latency_aware_host_selection'"

for i in $(seq 0 9)
do
    ${CLICKHOUSE_CLIENT} -q "
        INSERT INTO FUNCTION s3('http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_${i}.csv', 'test', 'testtest', 'CSV', 'n UInt64')
        SELECT number + ${i} * 1000 FROM numbers(1000)
        SETTINGS s3_truncate_on_insert = 1"
done

for _ in $(seq 1 5)
do
    ${CLICKHOUSE_CLIENT} -q "
        SELECT count(), sum(n)
        FROM s3('http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_{0..9}.csv', 'test', 'testtest', 'CSV', 'n UInt64')
        SETTINGS max_threads = 8, use_object_storage_list_objects_cache = 0, use_query_condition_cache = 0, s3_max_connections = 4"
done
