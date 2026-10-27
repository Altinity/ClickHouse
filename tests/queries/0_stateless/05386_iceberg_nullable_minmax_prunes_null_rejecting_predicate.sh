#!/usr/bin/env bash
# Tags: no-fasttest
# - no-fasttest: requires `IcebergLocal` (USE_AVRO build option)

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

ROOT="${CLICKHOUSE_USER_FILES}/${CLICKHOUSE_DATABASE}_nullable_minmax"
rm -rf "${ROOT}"
trap 'rm -rf "${ROOT}"' EXIT

# A Unity liquid-clustering column is an ordinary data column. Its manifest bounds are the
# non-null min/max, and `null_value_counts` counts the nulls separately. `partner_id = 'sg2o27m'`
# is false for null, so a file whose non-null range misses that value cannot hold a match.
# `partner_id IS NULL` is true for those nulls, so that file must stay.
#
# One INSERT per file. `low` is `'aaa'` plus a null (the probe is above its max), `high` is `'zzz'`
# plus a null (the probe is below its min), and `hit` is the probe itself and has no nulls.
# Compaction would merge the files and erase the per-file bounds this checks.
${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1 --use_iceberg_metadata_files_cache=0 --query "
    CREATE TABLE nullable_minmax (partner_id Nullable(String))
    ENGINE = IcebergLocal('${ROOT}/', 'Parquet')
    ORDER BY partner_id
    SETTINGS allow_experimental_iceberg_compaction = 0;

    INSERT INTO nullable_minmax VALUES ('aaa'), (NULL);
    INSERT INTO nullable_minmax VALUES ('zzz'), (NULL);
    INSERT INTO nullable_minmax VALUES ('sg2o27m');
" < /dev/null

echo '--- a matching row is returned, including null ---'
${CLICKHOUSE_CLIENT} --use_iceberg_metadata_files_cache=0 --query "
    SELECT count() FROM nullable_minmax WHERE partner_id = 'sg2o27m';
    SELECT count() FROM nullable_minmax WHERE partner_id IS NULL;
    SELECT count() FROM nullable_minmax WHERE partner_id = 'aaa';
    SELECT count() FROM nullable_minmax WHERE NOT partner_id = 'sg2o27m';"

echo '--- min/max prunes a file with nulls when the predicate rejects null ---'
# `FORMAT Null` keeps these out of the reference. `query_log` still records the prune counters.
${CLICKHOUSE_CLIENT} --use_iceberg_metadata_files_cache=0 --query "
    SELECT count() FROM nullable_minmax WHERE partner_id = 'sg2o27m'
        SETTINGS log_comment = '${CLICKHOUSE_DATABASE}_eq_probe' FORMAT Null;
    SELECT count() FROM nullable_minmax WHERE partner_id = 'aaa'
        SETTINGS log_comment = '${CLICKHOUSE_DATABASE}_eq_low' FORMAT Null;
    SELECT count() FROM nullable_minmax WHERE partner_id IS NULL
        SETTINGS log_comment = '${CLICKHOUSE_DATABASE}_is_null' FORMAT Null;
    SELECT count() FROM nullable_minmax WHERE NOT partner_id = 'sg2o27m'
        SETTINGS log_comment = '${CLICKHOUSE_DATABASE}_not_probe' FORMAT Null;
    SYSTEM FLUSH LOGS query_log;"

${CLICKHOUSE_CLIENT} --query "
    SELECT max(ProfileEvents['IcebergMinMaxIndexPrunedFiles'])
    FROM system.query_log
    WHERE current_database = currentDatabase() AND type = 'QueryFinish'
        AND log_comment IN (
            '${CLICKHOUSE_DATABASE}_eq_probe',
            '${CLICKHOUSE_DATABASE}_eq_low',
            '${CLICKHOUSE_DATABASE}_is_null',
            '${CLICKHOUSE_DATABASE}_not_probe')
    GROUP BY log_comment
    ORDER BY indexOf(
        ['${CLICKHOUSE_DATABASE}_eq_probe', '${CLICKHOUSE_DATABASE}_eq_low',
         '${CLICKHOUSE_DATABASE}_is_null', '${CLICKHOUSE_DATABASE}_not_probe'],
        log_comment)
    SETTINGS enable_parallel_replicas = 0"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS nullable_minmax SYNC;"
