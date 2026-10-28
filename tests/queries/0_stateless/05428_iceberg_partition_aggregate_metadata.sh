#!/usr/bin/env bash
# Tags: no-fasttest, no-replicated-database
# - no-fasttest: Iceberg needs Avro, and the S3 engines need MinIO.
# - no-replicated-database: these engines are not replicated.
#
# The partition value and `record_count` live in the Iceberg manifest, so the same shortcut
# applies to every engine that reads through `IcebergMetadata`: the local table, the S3 table,
# the generic `Iceberg` table, their table functions, and `icebergS3Cluster`. A plain `S3`
# table has no manifest and is not covered here.
#
# On a cluster the initiator's `query_log.read_rows` is the sum across shards. The answer has
# to come from one metadata pass: two shards each returning the whole table would double
# `count` and the grouped counts.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -euo pipefail

PREFIX="${CLICKHOUSE_TEST_UNIQUE_NAME}"
LOCAL_TABLE="t_${PREFIX}_local"
S3_TABLE="t_${PREFIX}_s3"
ICEBERG_TABLE="t_${PREFIX}_ice"
LOCAL_PATH="${USER_FILES_PATH}/${LOCAL_TABLE}/"
S3_KEY="${PREFIX}/s3"
ICEBERG_KEY="${PREFIX}/ice"
ICEBERG_URL="http://localhost:11111/test/${ICEBERG_KEY}/"

cleanup()
{
    ${CLICKHOUSE_CLIENT} --query "
        DROP TABLE IF EXISTS ${LOCAL_TABLE};
        DROP TABLE IF EXISTS ${S3_TABLE};
        DROP TABLE IF EXISTS ${ICEBERG_TABLE}" || true
    rm -rf "${LOCAL_PATH}"
}
trap cleanup EXIT

data_files()
{
    ${CLICKHOUSE_CLIENT} --format TabSeparated --query "
        SELECT count()
        FROM system.iceberg_files
        WHERE database = currentDatabase() AND table = '${1}' AND toString(content) = 'DATA'"
}

create_table()
{
    local table=$1
    local engine=$2
    ${CLICKHOUSE_CLIENT} --query "
        CREATE TABLE ${table} (d Date32, k Int64, v Float64)
        ENGINE = ${engine}
        PARTITION BY d
        SETTINGS allow_experimental_iceberg_compaction = 0"
    ${CLICKHOUSE_CLIENT} --query "
        INSERT INTO ${table}
        SELECT toDate32('2026-01-01') + intDiv(number, 1000), number % 100, 0
        FROM numbers(10000)
        SETTINGS allow_insert_into_iceberg = 1, async_insert = 0"
    local files
    files=$(data_files "${table}")
    if [ "${files}" -ne 10 ]; then
        echo "${table}: expected 10 data files, got ${files}"
        exit 1
    fi
}

run()
{
    local tag=$1
    local sql=$2
    ${CLICKHOUSE_CLIENT} --format TabSeparated --query "${sql}
SETTINGS
    log_comment = '${tag}',
    log_queries = 1,
    optimize_trivial_count_query = 1,
    use_iceberg_metadata_files_cache = 0,
    use_iceberg_partition_pruning = 1,
    enable_parallel_replicas = 0"
}

# One metadata pass reads at most the data files selected by the predicate.
# `max(k)` is not a partition column, so it still reads the 10000 data rows.
exercise()
{
    local label=$1
    local source=$2
    local tag="${PREFIX}_${label}"

    echo "${label}"
    echo "files 10"

    echo "max_d"
    run "${tag}_max_d" "SELECT max(d) FROM ${source}"

    echo "min_d"
    run "${tag}_min_d" "SELECT min(d) FROM ${source}"

    # The predicate matches every partition. It must not force the data scan back on.
    echo "max_d_filter"
    run "${tag}_max_d_filter" "SELECT max(d) FROM ${source} WHERE d >= '2026-01-01'"

    # Two dates, 2026-01-09 and 2026-01-10. Two files, not the 2000 rows inside them.
    echo "max_d_range"
    run "${tag}_max_d_range" "SELECT max(d) FROM ${source} WHERE d >= '2026-01-09'"

    echo "groupby_d"
    run "${tag}_groupby_d" "SELECT concat(toString(d), ' ', toString(count())) FROM ${source} GROUP BY d ORDER BY d"

    echo "groupby_d_range"
    run "${tag}_groupby_d_range" "SELECT concat(toString(d), ' ', toString(count())) FROM ${source} WHERE d >= '2026-01-09' GROUP BY d ORDER BY d"

    echo "max_of_groupby_count"
    run "${tag}_max_of_groupby_count" "SELECT max(d) FROM (SELECT d, count(*) FROM ${source} GROUP BY d)"

    echo "max_of_groupby"
    run "${tag}_max_of_groupby" "SELECT max(d) FROM (SELECT d FROM ${source} GROUP BY d)"

    echo "groupby_d_only"
    run "${tag}_groupby_d_only" "SELECT d FROM ${source} GROUP BY d ORDER BY d"

    echo "count"
    run "${tag}_count" "SELECT count() FROM ${source}"

    echo "max_k"
    run "${tag}_max_k" "SELECT max(k) FROM ${source}"

    ${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log"

    echo "reads"
    # `log_comment` is stable across runs in the same database. Take the latest
    # `QueryFinish` for each statement. `max(read_rows)` would keep a scan from
    # an earlier run and report this run as a data read.
    ${CLICKHOUSE_CLIENT} --format TabSeparated --query "
        WITH entries AS
        (
            SELECT
                replaceOne(log_comment, '${tag}_', '') AS name,
                argMax(read_rows, event_time_microseconds) AS read_rows,
                argMax(ProfileEvents['IcebergTrivialCountOptimizationApplied'], event_time_microseconds) AS trivial_count
            FROM system.query_log
            WHERE type = 'QueryFinish'
                AND is_initial_query
                AND current_database = currentDatabase()
                AND startsWith(log_comment, '${tag}_')
            GROUP BY name
        ),
        expected AS
        (
            SELECT 'max_d' AS name, 10 AS lim, 0 AS ord
            UNION ALL SELECT 'min_d', 10, 1
            UNION ALL SELECT 'max_d_filter', 10, 2
            UNION ALL SELECT 'max_d_range', 2, 3
            UNION ALL SELECT 'groupby_d', 10, 4
            UNION ALL SELECT 'groupby_d_range', 2, 5
            UNION ALL SELECT 'max_of_groupby_count', 10, 6
            UNION ALL SELECT 'max_of_groupby', 10, 7
            UNION ALL SELECT 'groupby_d_only', 10, 8
            UNION ALL SELECT 'count', 10, 9
            UNION ALL SELECT 'max_k', 0, 10
        )
        SELECT concat(e.name, ' ', multiIf(
            e.name = 'max_k',
                if(q.read_rows >= 10000, 'data', concat('metadata read_rows=', toString(q.read_rows))),
            e.name = 'count',
                if(
                    q.read_rows <= e.lim AND q.trivial_count = 1,
                    'metadata',
                    concat(
                        'data read_rows=', toString(q.read_rows),
                        ' limit=', toString(e.lim),
                        ' trivial_count=', toString(q.trivial_count))),
            if(
                q.read_rows <= e.lim,
                'metadata',
                concat(
                    'data read_rows=', toString(q.read_rows),
                    ' limit=', toString(e.lim)))))
        FROM expected AS e
        INNER JOIN entries AS q ON e.name = q.name
        ORDER BY e.ord
        SETTINGS enable_parallel_replicas = 0"
}

create_table "${LOCAL_TABLE}" "IcebergLocal('${LOCAL_PATH}')"
exercise "IcebergLocal" "${LOCAL_TABLE}"

# `optimize_trivial_count_query = 0` must scan. Manifest `min` / `max` / grouped `count` stay enabled.
echo "count_setting_off"
${CLICKHOUSE_CLIENT} --format TabSeparated --query "
    SELECT count() FROM ${LOCAL_TABLE}
    SETTINGS
        log_comment = '${PREFIX}_count_setting_off',
        log_queries = 1,
        optimize_trivial_count_query = 0,
        use_iceberg_metadata_files_cache = 0,
        enable_parallel_replicas = 0"
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log"
${CLICKHOUSE_CLIENT} --format TabSeparated --query "
    SELECT if(
        read_rows >= 10000 AND ProfileEvents['IcebergTrivialCountOptimizationApplied'] = 0,
        'data',
        concat('metadata read_rows=', toString(read_rows), ' trivial_count=', toString(ProfileEvents['IcebergTrivialCountOptimizationApplied'])))
    FROM system.query_log
    WHERE type = 'QueryFinish'
        AND is_initial_query
        AND current_database = currentDatabase()
        AND log_comment = '${PREFIX}_count_setting_off'
    ORDER BY event_time_microseconds DESC
    LIMIT 1"

# `rand()` differs per row. A match must not keep the whole file `record_count`.
echo "rand_filter"
${CLICKHOUSE_CLIENT} --query "
    SELECT count() FROM ${LOCAL_TABLE}
    WHERE rand() % 2 = 0
    SETTINGS
        log_comment = '${PREFIX}_rand_filter',
        log_queries = 1,
        optimize_trivial_count_query = 1,
        use_iceberg_metadata_files_cache = 0,
        enable_parallel_replicas = 0" > /dev/null
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log"
${CLICKHOUSE_CLIENT} --format TabSeparated --query "
    SELECT if(
        read_rows >= 10000,
        'data',
        concat('metadata read_rows=', toString(read_rows)))
    FROM system.query_log
    WHERE type = 'QueryFinish'
        AND is_initial_query
        AND current_database = currentDatabase()
        AND log_comment = '${PREFIX}_rand_filter'
    ORDER BY event_time_microseconds DESC
    LIMIT 1"

# `toYear(d)` is the same for every row of an identity partition file.
echo "to_year"
${CLICKHOUSE_CLIENT} --format TabSeparated --query "
    SELECT count() FROM ${LOCAL_TABLE}
    WHERE toYear(d) = 2026
    SETTINGS
        log_comment = '${PREFIX}_to_year',
        log_queries = 1,
        optimize_trivial_count_query = 1,
        use_iceberg_metadata_files_cache = 0,
        enable_parallel_replicas = 0"
${CLICKHOUSE_CLIENT} --query "SYSTEM FLUSH LOGS query_log"
${CLICKHOUSE_CLIENT} --format TabSeparated --query "
    SELECT if(
        read_rows <= 10,
        'metadata',
        concat('data read_rows=', toString(read_rows)))
    FROM system.query_log
    WHERE type = 'QueryFinish'
        AND is_initial_query
        AND current_database = currentDatabase()
        AND log_comment = '${PREFIX}_to_year'
    ORDER BY event_time_microseconds DESC
    LIMIT 1"

# `max_rows_to_group_by` must use the scan. The manifest path would return every partition.
echo "group_by_row_limit"
set +e
limit_output=$(${CLICKHOUSE_CLIENT} --query "
    SELECT d, count() FROM ${LOCAL_TABLE}
    GROUP BY d
    SETTINGS
        max_rows_to_group_by = 1,
        group_by_overflow_mode = 'throw',
        optimize_trivial_count_query = 1,
        use_iceberg_metadata_files_cache = 0,
        enable_parallel_replicas = 0" 2>&1)
limit_status=$?
set -e
if [ "${limit_status}" -eq 0 ] || [[ "${limit_output}" != *"TOO_MANY_ROWS"* ]]; then
    echo "group_by_row_limit expected TOO_MANY_ROWS, status=${limit_status}"
    printf '%s\n' "${limit_output}"
    exit 1
fi
echo "TOO_MANY_ROWS"

# `any` stops after the key that crosses `max_rows_to_group_by`, so limit 1 keeps two groups.
echo "group_by_row_limit_any"
${CLICKHOUSE_CLIENT} --format TabSeparated --query "
    SELECT count() FROM
    (
        SELECT d, count() FROM ${LOCAL_TABLE}
        GROUP BY d
        SETTINGS
            max_rows_to_group_by = 1,
            group_by_overflow_mode = 'any',
            optimize_trivial_count_query = 1,
            use_iceberg_metadata_files_cache = 0,
            enable_parallel_replicas = 0
    )"

exercise "icebergLocal" "icebergLocal('${LOCAL_PATH}')"

create_table "${S3_TABLE}" "IcebergS3(s3_conn, filename = '${S3_KEY}')"
exercise "IcebergS3" "${S3_TABLE}"
exercise "icebergS3" "icebergS3(s3_conn, filename = '${S3_KEY}')"
exercise "icebergS3Cluster" "icebergS3Cluster('test_cluster_two_shards_localhost', s3_conn, filename = '${S3_KEY}')"

create_table "${ICEBERG_TABLE}" "Iceberg('${ICEBERG_URL}', 'test', 'testtest')"
exercise "Iceberg" "${ICEBERG_TABLE}"
exercise "iceberg" "iceberg('${ICEBERG_URL}', 'test', 'testtest', storage_type = 's3')"
