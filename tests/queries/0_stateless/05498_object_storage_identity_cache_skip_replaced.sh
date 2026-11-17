#!/usr/bin/env bash
# Tags: no-fasttest, no-parallel, no-parallel-replicas
# Tag no-fasttest: reads a file through the mock S3 server at localhost:11111
# Tag no-parallel: the identity cache and the query condition cache are process-global
# Tag no-parallel-replicas: per-query S3 request counts change when a read is spread across replicas

# An identity-cache hit still has the previous size and ETag. Two decisions use that metadata and
# then do not GET: s3_skip_empty_files (default on) skips a cached size of 0, and the query condition
# cache skips a file whose cached ETag matched no row group. An external replace must still be read.
# A condition-cache re-HEAD that finds an empty replacement must skip it, not open it.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

READ_SETTINGS="max_threads = 1, use_hive_partitioning = 0, parallel_replicas_for_cluster_engines = 0, enable_filesystem_cache = 0, use_page_cache_for_object_storage = 0, s3_validate_etag_on_read = 1, use_object_storage_list_objects_cache = 0, s3_skip_empty_files = 1"

# --- A cached empty object must not hide a later non-empty replace. ---

EMPTY_URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_empty.tsv"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache failed"

curl -fsS -X PUT "$EMPTY_URL" --data-binary '' >/dev/null || fail "empty put failed"

empty_cold=$(${CLICKHOUSE_CLIENT} -q "
    SELECT n FROM s3('${EMPTY_URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") || fail "empty cold select failed"
[[ -z $empty_cold ]] || fail "empty cold: data '${empty_cold}'"

empty_warm=$(${CLICKHOUSE_CLIENT} -q "
    SELECT n FROM s3('${EMPTY_URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") || fail "empty warm select failed"
[[ -z $empty_warm ]] || fail "empty warm: data '${empty_warm}'"

empty_payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_empty.tsv"
printf '7\n' > "$empty_payload"
curl -fsS -X PUT "$EMPTY_URL" --upload-file "$empty_payload" >/dev/null || fail "empty replace put failed"

replaced_qid="idcache_skip_empty_${CLICKHOUSE_DATABASE}"
empty_replaced=$(${CLICKHOUSE_CLIENT} --query_id "$replaced_qid" -q "
    SELECT n FROM s3('${EMPTY_URL}', 'test', 'testtest', 'TSV', 'n UInt64')
    SETTINGS ${READ_SETTINGS} FORMAT TabSeparated") || fail "empty replaced select failed"
[[ $empty_replaced == "7" ]] || fail "empty replaced: data '${empty_replaced}', expected 7"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs failed"
empty_heads=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['S3HeadObject']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${replaced_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "empty query_log failed"
[[ $empty_heads -ge 1 ]] || fail "empty replaced did not HEAD (S3HeadObject=${empty_heads})"

# --- A query-condition-cache hit on the previous ETag must not drop the replaced file. ---

PARQUET_URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_qcc.parquet"
TABLE="idcache_skip_qcc_${CLICKHOUSE_DATABASE}"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache before parquet failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM DROP QUERY CONDITION CACHE" || fail "drop query condition cache failed"
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS ${TABLE}" || fail "drop table failed"

${CLICKHOUSE_CLIENT} -q "
    CREATE TABLE ${TABLE} (n UInt64)
    ENGINE = S3('${PARQUET_URL}', 'test', 'testtest', 'Parquet')" || fail "create table failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO ${TABLE} SELECT toUInt64(1)
    SETTINGS s3_truncate_on_insert = 1" || fail "parquet insert failed"

FILTER_SETTINGS="${READ_SETTINGS}, use_query_condition_cache = 1"

qcc_first=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM ${TABLE} WHERE n = 3 SETTINGS ${FILTER_SETTINGS}") || fail "qcc first select failed"
[[ $qcc_first == "0" ]] || fail "qcc first: '${qcc_first}', expected 0"

# The second read must hit the condition cache, or the replace below does not exercise the skip.
qcc_second_qid="idcache_skip_qcc_warm_${CLICKHOUSE_DATABASE}"
qcc_second=$(${CLICKHOUSE_CLIENT} --query_id "$qcc_second_qid" -q "
    SELECT count() FROM ${TABLE} WHERE n = 3 SETTINGS ${FILTER_SETTINGS}") || fail "qcc warm select failed"
[[ $qcc_second == "0" ]] || fail "qcc warm: '${qcc_second}', expected 0"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs before qcc hit check failed"
qcc_hits=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['QueryConditionCacheHits']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${qcc_second_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "qcc warm query_log failed"
[[ $qcc_hits -ge 1 ]] || fail "qcc warm was not a condition-cache hit (QueryConditionCacheHits=${qcc_hits})"

parquet_payload="${CLICKHOUSE_TMP}/${CLICKHOUSE_TEST_UNIQUE_NAME}_qcc.parquet"
${CLICKHOUSE_CLIENT} -q "SELECT toUInt64(3) AS n FORMAT Parquet" > "$parquet_payload" || fail "local parquet failed"
curl -fsS -X PUT "$PARQUET_URL" --upload-file "$parquet_payload" >/dev/null || fail "parquet replace put failed"

qcc_replaced_qid="idcache_skip_qcc_replaced_${CLICKHOUSE_DATABASE}"
qcc_replaced=$(${CLICKHOUSE_CLIENT} --query_id "$qcc_replaced_qid" -q "
    SELECT count() FROM ${TABLE} WHERE n = 3 SETTINGS ${FILTER_SETTINGS}") || fail "qcc replaced select failed"
[[ $qcc_replaced == "1" ]] || fail "qcc replaced: '${qcc_replaced}', expected 1"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs after qcc replace failed"
qcc_heads=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['S3HeadObject']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${qcc_replaced_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "qcc replaced query_log failed"
[[ $qcc_heads -ge 1 ]] || fail "qcc replaced did not HEAD (S3HeadObject=${qcc_heads})"

${CLICKHOUSE_CLIENT} -q "DROP TABLE ${TABLE}" || fail "drop table at end failed"

# --- A condition-cache re-HEAD that finds an empty replacement must skip it, not open it. ---

EMPTY_PARQUET_URL="http://localhost:11111/test/${CLICKHOUSE_TEST_UNIQUE_NAME}_qcc_empty.parquet"
EMPTY_TABLE="idcache_skip_qcc_empty_${CLICKHOUSE_DATABASE}"

${CLICKHOUSE_CLIENT} -q "SYSTEM CLEAR OBJECT STORAGE IDENTITY CACHE" || fail "clear identity cache before empty parquet failed"
${CLICKHOUSE_CLIENT} -q "SYSTEM DROP QUERY CONDITION CACHE" || fail "drop query condition cache before empty parquet failed"
${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS ${EMPTY_TABLE}" || fail "drop empty parquet table failed"

${CLICKHOUSE_CLIENT} -q "
    CREATE TABLE ${EMPTY_TABLE} (n UInt64)
    ENGINE = S3('${EMPTY_PARQUET_URL}', 'test', 'testtest', 'Parquet')" || fail "create empty parquet table failed"

${CLICKHOUSE_CLIENT} -q "
    INSERT INTO ${EMPTY_TABLE} SELECT toUInt64(1)
    SETTINGS s3_truncate_on_insert = 1" || fail "empty parquet insert failed"

empty_qcc_first=$(${CLICKHOUSE_CLIENT} -q "
    SELECT count() FROM ${EMPTY_TABLE} WHERE n = 3 SETTINGS ${FILTER_SETTINGS}") || fail "empty parquet first select failed"
[[ $empty_qcc_first == "0" ]] || fail "empty parquet first: '${empty_qcc_first}', expected 0"

empty_qcc_warm_qid="idcache_skip_qcc_empty_warm_${CLICKHOUSE_DATABASE}"
empty_qcc_warm=$(${CLICKHOUSE_CLIENT} --query_id "$empty_qcc_warm_qid" -q "
    SELECT count() FROM ${EMPTY_TABLE} WHERE n = 3 SETTINGS ${FILTER_SETTINGS}") || fail "empty parquet warm select failed"
[[ $empty_qcc_warm == "0" ]] || fail "empty parquet warm: '${empty_qcc_warm}', expected 0"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs before empty parquet hit check failed"
empty_qcc_hits=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['QueryConditionCacheHits']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${empty_qcc_warm_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "empty parquet warm query_log failed"
[[ $empty_qcc_hits -ge 1 ]] || fail "empty parquet warm was not a condition-cache hit (QueryConditionCacheHits=${empty_qcc_hits})"

curl -fsS -X PUT "$EMPTY_PARQUET_URL" --data-binary '' >/dev/null || fail "empty parquet replace put failed"

empty_qcc_replaced_qid="idcache_skip_qcc_empty_replaced_${CLICKHOUSE_DATABASE}"
empty_qcc_replaced=$(${CLICKHOUSE_CLIENT} --query_id "$empty_qcc_replaced_qid" -q "
    SELECT count() FROM ${EMPTY_TABLE} WHERE n = 3 SETTINGS ${FILTER_SETTINGS}") || fail "empty parquet replaced select failed"
[[ $empty_qcc_replaced == "0" ]] || fail "empty parquet replaced: '${empty_qcc_replaced}', expected 0"

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log" || fail "flush logs after empty parquet replace failed"
empty_qcc_heads=$(${CLICKHOUSE_CLIENT} -q "
    SELECT ProfileEvents['S3HeadObject']
    FROM system.query_log
    WHERE event_date >= yesterday()
        AND event_time >= now() - 600
        AND type = 'QueryFinish'
        AND query_id = '${empty_qcc_replaced_qid}'
        AND current_database = currentDatabase()
    ORDER BY event_time_microseconds DESC
    LIMIT 1
    FORMAT TabSeparated") || fail "empty parquet replaced query_log failed"
[[ $empty_qcc_heads -ge 1 ]] || fail "empty parquet replaced did not HEAD (S3HeadObject=${empty_qcc_heads})"

${CLICKHOUSE_CLIENT} -q "DROP TABLE ${EMPTY_TABLE}" || fail "drop empty parquet table at end failed"

echo OK
