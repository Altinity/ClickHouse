#!/usr/bin/env bash
# Tags: no-fasttest
# `MODIFY COLUMN` of an Iceberg table cannot change the state version of an `AggregateFunction` column,
# because the existing data files would be read with the new state layout.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -euo pipefail

TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}_state_version"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"

cleanup()
{
    ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE}"
    rm -rf "${TABLE_PATH}"
}
trap cleanup EXIT

CLIENT="${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1"

${CLIENT} --query "
    CREATE TABLE ${TABLE} (k Int32, q AggregateFunction(quantileDeterministic(0.5), Float64, UInt64))
    ENGINE = IcebergLocal('${TABLE_PATH}')
    SETTINGS allow_aggregate_function_states_in_open_formats = 1"
${CLIENT} --query "
    INSERT INTO ${TABLE} SELECT 1, quantileDeterministicState(0.5)(toFloat64(number), number) FROM numbers(100)"

check()
{
    ${CLIENT} --query "SELECT toTypeName(q), quantileDeterministicMerge(0.5)(q) FROM ${TABLE} GROUP BY 1"
}

check

echo "same type without the version"
(${CLIENT} --query "
    ALTER TABLE ${TABLE} MODIFY COLUMN q AggregateFunction(quantileDeterministic(0.5), Float64, UInt64)" 2>&1 || true) \
    | grep -q "BAD_ARGUMENTS" && echo "BAD_ARGUMENTS"
check

echo "explicit version 0"
(${CLIENT} --query "
    ALTER TABLE ${TABLE} MODIFY COLUMN q AggregateFunction(0, quantileDeterministic(0.5), Float64, UInt64)" 2>&1 || true) \
    | grep -q "BAD_ARGUMENTS" && echo "BAD_ARGUMENTS"
check

echo "same version"
${CLIENT} --query "
    ALTER TABLE ${TABLE} MODIFY COLUMN q AggregateFunction(1, quantileDeterministic(0.5), Float64, UInt64)"
check
