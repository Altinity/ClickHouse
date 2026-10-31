#!/usr/bin/env bash
# Tags: no-fasttest
# The ClickHouse type annotation of an Iceberg field is recorded per schema id, so after `MODIFY COLUMN`
# the older schema keeps the type its data was written with.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -euo pipefail

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}_annotation"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

cleanup()
{
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
    rm -rf "${TABLE_PATH}"
}
trap cleanup EXIT

CLIENT="${CLICKHOUSE_CLIENT} --allow_experimental_aggregate_function_states_in_open_formats=1 --allow_insert_into_iceberg=1"

${CLIENT} --query "
    CREATE TABLE ${TABLE} (k Int32, x SimpleAggregateFunction(max, Int32))
    ENGINE = IcebergLocal('${TABLE_PATH}')
    SETTINGS allow_experimental_aggregate_function_states_in_open_formats = 1"
${CLIENT} --query "INSERT INTO ${TABLE} VALUES (1, 10)"
SNAPSHOT=$(${CLIENT} --query "SELECT snapshot_id FROM system.iceberg_history WHERE database = currentDatabase() AND table = '${TABLE}'")

check()
{
    ${CLIENT} --query "DESCRIBE TABLE ${TABLE}" | cut -f1,2
    ${CLIENT} --query "SELECT k, x, toTypeName(x) FROM ${TABLE} ORDER BY k"
    ${CLIENT} --query "SELECT k, x, toTypeName(x) FROM icebergLocal('${TABLE_PATH}') ORDER BY k"
    echo "time travel"
    ${CLIENT} --query "SELECT k, x, toTypeName(x) FROM ${TABLE} ORDER BY k SETTINGS iceberg_snapshot_id = ${SNAPSHOT}"
}

echo "widen"
${CLIENT} --query "ALTER TABLE ${TABLE} MODIFY COLUMN x SimpleAggregateFunction(max, Int64)"
${CLIENT} --query "INSERT INTO ${TABLE} VALUES (2, 20)"
check

echo "drop the annotation"
${CLIENT} --query "ALTER TABLE ${TABLE} MODIFY COLUMN x Int64"
check
