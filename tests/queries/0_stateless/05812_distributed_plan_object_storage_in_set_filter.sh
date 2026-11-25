#!/usr/bin/env bash
# Tags: no-fasttest, no-old-analyzer
# - no-fasttest: reads Parquet files from Minio
# - no-old-analyzer: make_distributed_plan requires the analyzer

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# A task of a distributed plan rebuilds its object storage read from the serialized plan. The files are pruned by the
# filter of the PREWHERE, and the sets of an `IN` in it are only placeholders until the whole plan is deserialized:
# building them while the step was being deserialized dereferenced a null set and killed the server.

URL="http://localhost:11111/test/${CLICKHOUSE_DATABASE}_distributed_in_set"
AUTH="'test', 'testtest'"

${CLICKHOUSE_CLIENT} --query "
    INSERT INTO FUNCTION s3('${URL}/data_1.parquet', ${AUTH}, 'Parquet')
    SELECT number AS k, concat('val_', toString(number)) AS s
    FROM numbers(0, 1000)
    SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 100;

    INSERT INTO FUNCTION s3('${URL}/data_2.parquet', ${AUTH}, 'Parquet')
    SELECT number AS k, concat('val_', toString(number)) AS s
    FROM numbers(1000, 1000)
    SETTINGS s3_truncate_on_insert = 1, output_format_parquet_row_group_size = 100;

    DROP TABLE IF EXISTS t_distributed_in_set;
    CREATE TABLE t_distributed_in_set (k UInt64, s String) ENGINE = S3('${URL}/data_{1,2}.parquet', ${AUTH}, 'Parquet');
"

# The worker finds the table by its name, so the read must be of a table, not of a table function.
SETTINGS_COMMON=(
    --enable_analyzer=1
    --enable_parallel_replicas=0
    --optimize_move_to_prewhere=1
    --distributed_plan_default_reader_bucket_count=4
)

for filter in "k IN (1, 5, 100, 1500, 1999)" "k IN (SELECT number FROM numbers(10))" "k NOT IN (SELECT number FROM numbers(1990))" "k IN (1, 5) OR s IN ('val_1500')"
do
    echo "-- ${filter}: make_distributed_plan = 0"
    ${CLICKHOUSE_CLIENT} "${SETTINGS_COMMON[@]}" --make_distributed_plan=0 \
        --query "SELECT count(), sum(k) FROM t_distributed_in_set WHERE ${filter}"
    echo "-- ${filter}: make_distributed_plan = 1"
    ${CLICKHOUSE_CLIENT} "${SETTINGS_COMMON[@]}" --make_distributed_plan=1 \
        --query "SELECT count(), sum(k) FROM t_distributed_in_set WHERE ${filter}"
done

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_distributed_in_set"
