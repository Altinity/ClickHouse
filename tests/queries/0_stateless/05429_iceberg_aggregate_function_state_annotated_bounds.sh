#!/usr/bin/env bash
# Tags: no-fasttest
# Iceberg bounds of an annotated `DateTime64(3)` field are microseconds and must not be read at scale 3.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -euo pipefail

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}_annotated_bounds"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

cleanup()
{
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
    rm -rf "${TABLE_PATH}"
}
trap cleanup EXIT

CLIENT="${CLICKHOUSE_CLIENT} --allow_experimental_aggregate_function_states_in_open_formats=1 --allow_insert_into_iceberg=1"

${CLIENT} --query "
    CREATE TABLE ${TABLE} (x SimpleAggregateFunction(max, DateTime64(3, 'UTC')))
    ENGINE = IcebergLocal('${TABLE_PATH}')
    SETTINGS allow_experimental_aggregate_function_states_in_open_formats = 1"
${CLIENT} --query "INSERT INTO ${TABLE} VALUES ('2026-01-01 00:00:00.123')"
${CLIENT} --query "SELECT x FROM ${TABLE}"
${CLIENT} --query "SELECT count() FROM ${TABLE} WHERE x = toDateTime64('2026-01-01 00:00:00.123', 3, 'UTC')"
