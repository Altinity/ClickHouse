#!/usr/bin/env bash
# Tags: no-fasttest
# An Iceberg table read on a cluster passes its own `allow_aggregate_function_states_in_open_formats` to the other nodes
# together with its other settings, such as `iceberg_metadata_file_path`, so they decide over the query's value there too.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -eu

SETTING="allow_aggregate_function_states_in_open_formats"
TABLE="t_${CLICKHOUSE_DATABASE}_${RANDOM}_states"
TABLE_PATH="${USER_FILES_PATH}/${TABLE}/"
PINNED="${TABLE}_pinned"

cleanup()
{
    for table in "${TABLE}" "${PINNED}"; do
        ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${table}"
    done
    rm -rf "${TABLE_PATH}"
}
trap cleanup EXIT

CLIENT="${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1"

${CLIENT} --query "
    CREATE TABLE ${TABLE} (k Int32, u AggregateFunction(uniq, UInt64))
    ENGINE = IcebergLocal('${TABLE_PATH}')
    SETTINGS ${SETTING} = 1"
${CLIENT} --query "INSERT INTO ${TABLE} SELECT toInt32(number % 2), uniqState(toUInt64(number % 23)) FROM numbers(200) GROUP BY 1"

METADATA_FILE=$(cd "${TABLE_PATH}" && ls metadata/*.metadata.json | sort -V | tail -n 1)

${CLIENT} --query "
    CREATE TABLE ${PINNED} ENGINE = IcebergLocal('${TABLE_PATH}')
    SETTINGS ${SETTING} = 1, iceberg_metadata_file_path = '${METADATA_FILE}'"

echo "-- the query disables the setting, the table enables it"
${CLIENT} --query "
    SELECT k, uniqMerge(u) FROM ${PINNED} GROUP BY k ORDER BY k
    SETTINGS ${SETTING} = 0, object_storage_cluster = 'test_cluster_two_shards_localhost'"
