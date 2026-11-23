#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the query condition cache are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# The query condition cache key contains the ETag. An identity-cache hit still has the previous ETag,
# so a condition-cache hit that keeps only some row groups describes the previous object. The read
# then pins its GET to that ETag and fails once the object was replaced outside ClickHouse. The
# cached identity must be confirmed with a HEAD before any condition-cache hit is used, not only
# before a hit that drops the whole file.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0, s3_skip_empty_files = 1, use_query_condition_cache = 1"

PARQUET_URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_partial.parquet"
TABLE="idcache_partial_qcc_${CLICKHOUSE_DATABASE}"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM DROP QUERY CONDITION CACHE" || fail "drop query condition cache failed"
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS ${TABLE}" || fail "drop table failed"

${CLICKHOUSE_CLIENT} -q "
    CREATE TABLE ${TABLE} (n UInt64)
    ENGINE = S3('${PARQUET_URL}', 'test', 'testtest', 'Parquet')" || fail "create table failed"

# Four row groups of 1000 rows: the filter below matches only the last one, so the condition-cache
# entry keeps one row group and drops three. It must not drop the whole file.
${CLICKHOUSE_CLIENT} -q "
    INSERT INTO ${TABLE} SELECT number FROM numbers(4000)
    SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 1000, max_block_size = 1000000" || fail "insert failed"

cold=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM ${TABLE} WHERE n = 3500 SETTINGS ${READ_SETTINGS}") || fail "cold select failed"
[[ $cold == "1" ]] || fail "cold: '${cold}', expected 1"

# The warm read must hit the condition cache, or the replace below does not exercise the hit.
warm_qid="idcache_partial_qcc_warm_${CLICKHOUSE_DATABASE}"
warm=$(${CLICKHOUSE_CLIENT} --query_id "$warm_qid" -q "
    SELECT count() FROM ${TABLE} WHERE n = 3500 SETTINGS ${READ_SETTINGS}") || fail "warm select failed"
[[ $warm == "1" ]] || fail "warm: '${warm}', expected 1"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs before hit check failed"
warm_hits=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['QueryConditionCacheHits']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${warm_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "warm query_log failed"
[[ $warm_hits -ge 1 ]] || fail "warm was not a condition-cache hit (QueryConditionCacheHits=${warm_hits})"

# Replace the object outside ClickHouse with a single-row-group file that has a different ETag.
payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_partial.parquet"
${CLICKHOUSE_CLIENT} -q "SELECT arrayJoin([3500, 3500]) :: UInt64 AS n FORMAT Parquet" > "$payload" || fail "local parquet failed"
curl -fsS -X PUT "$PARQUET_URL" --upload-file "$payload" >/dev/null || fail "replace put failed"

replaced_qid="idcache_partial_qcc_replaced_${CLICKHOUSE_DATABASE}"
replaced=$(${CLICKHOUSE_CLIENT} --query_id "$replaced_qid" -q "
    SELECT count() FROM ${TABLE} WHERE n = 3500 SETTINGS ${READ_SETTINGS}") || fail "replaced select failed"
[[ $replaced == "2" ]] || fail "replaced: '${replaced}', expected 2"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs after replace failed"
replaced_heads=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['S3HeadObject']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${replaced_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "replaced query_log failed"
[[ $replaced_heads -ge 1 ]] || fail "replaced did not HEAD (S3HeadObject=${replaced_heads})"

${CLICKHOUSE_CLIENT} -q "DROP TABLE ${TABLE}" || fail "drop table at end failed"

echo OK
