#!/usr/bin/env bash
# Tags: no-fasttest
# `allow_aggregate_function_states_in_open_formats` in a table's `SETTINGS` is stored with the table and decides
# for every query on it, also after the table is re-attached. Without it, the table has the server's value, disabled by
# default, whatever the query sets: the recorded types are not applied, and a table with aggregate function states is refused.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

set -eu

SETTING="allow_aggregate_function_states_in_open_formats"
OFF="--${SETTING}=0"
ICEBERG="t_${CLICKHOUSE_DATABASE}_${RANDOM}_states"
ICEBERG_PATH="${USER_FILES_PATH}/${ICEBERG}/"
ICEBERG_NOT_STORED="${ICEBERG}_not_stored"
ICEBERG_NOT_STORED_PATH="${USER_FILES_PATH}/${ICEBERG_NOT_STORED}/"
ICEBERG_ENABLED="${ICEBERG}_enabled"

cleanup()
{
    for table in "${ICEBERG}" "${ICEBERG_NOT_STORED}" "${ICEBERG_ENABLED}" file_states file_states_not_stored; do
        ${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${table}"
    done
    rm -rf "${ICEBERG_PATH}" "${ICEBERG_NOT_STORED_PATH}"
}
trap cleanup EXIT

CLIENT="${CLICKHOUSE_CLIENT} --allow_insert_into_iceberg=1"
COLUMNS="k Int32, u AggregateFunction(uniq, UInt64), s SimpleAggregateFunction(sum, Int64)"

insert_states()
{
    local table=$1
    shift
    ${CLIENT} "$@" --query "
        INSERT INTO ${table} SELECT toInt32(number % 2), uniqState(toUInt64(number % 23)), sumSimpleState(toInt64(number))
        FROM numbers(200) GROUP BY 1" 2>&1
}

select_states()
{
    local table=$1
    shift
    ${CLIENT} "$@" --query "SELECT k, uniqMerge(u), sum(s) FROM ${table} GROUP BY k ORDER BY k" 2>&1
}

run()
{
    ${CLIENT} "$@" 2>&1
}

reattach()
{
    ${CLIENT} --query "DETACH TABLE $1"
    ${CLIENT} --query "ATTACH TABLE $1"
}

# Each gate names what it refuses, which the query text does not contain.
CREATE_OR_ALTER_REFUSED="has type .*, which Iceberg cannot express natively"
READ_REFUSED="Iceberg field with id [0-9]* records ClickHouse type"
PARQUET_WRITE_REFUSED="Writing aggregate function states of column 'u' into Parquet data format is disabled"

expect_refusal()
{
    local message=$1
    shift
    if "$@" | grep -q "${message}"; then echo "refused"; else echo "not refused"; fi
}

echo "-- Iceberg, setting stored in the table, queries disable it"
${CLIENT} ${OFF} --query "CREATE TABLE ${ICEBERG} (${COLUMNS}) ENGINE = IcebergLocal('${ICEBERG_PATH}') SETTINGS ${SETTING} = 1"
${CLIENT} --query "SHOW CREATE TABLE ${ICEBERG}" | grep -c "${SETTING} = 1"
reattach "${ICEBERG}"
insert_states "${ICEBERG}" ${OFF}
insert_states "${ICEBERG}" ${OFF}
select_states "${ICEBERG}" ${OFF}
${CLIENT} ${OFF} --query "SELECT uniqExact(_path) FROM ${ICEBERG}"
${CLIENT} ${OFF} --allow_experimental_iceberg_compaction=1 --query "OPTIMIZE TABLE ${ICEBERG}"
${CLIENT} ${OFF} --query "SELECT uniqExact(_path) FROM ${ICEBERG}"
select_states "${ICEBERG}" ${OFF}
${CLIENT} ${OFF} --query "ALTER TABLE ${ICEBERG} UPDATE k = k + 10 WHERE k = 0"
select_states "${ICEBERG}" ${OFF}

echo "-- Iceberg, setting not stored in the table, queries enable it"
ON="--${SETTING}=1"
expect_refusal "${CREATE_OR_ALTER_REFUSED}" run ${ON} --query "CREATE TABLE ${ICEBERG_NOT_STORED} (${COLUMNS}) ENGINE = IcebergLocal('${ICEBERG_NOT_STORED_PATH}')"
# The same data as `${ICEBERG}`, through a table without the setting.
expect_refusal "${READ_REFUSED}" run ${ON} --query "CREATE TABLE ${ICEBERG_NOT_STORED} ENGINE = IcebergLocal('${ICEBERG_PATH}')"
# A `SimpleAggregateFunction` holds ordinary values of its storage type, so it needs no setting. Its type is recorded,
# but only a table with the setting applies it.
${CLIENT} ${ON} --query "
    CREATE TABLE ${ICEBERG_NOT_STORED} (k Int32, s SimpleAggregateFunction(sum, Int64)) ENGINE = IcebergLocal('${ICEBERG_NOT_STORED_PATH}')"
${CLIENT} --query "SHOW CREATE TABLE ${ICEBERG_NOT_STORED}" | grep -c "${SETTING}" || true
reattach "${ICEBERG_NOT_STORED}"
run ${ON} --query "INSERT INTO ${ICEBERG_NOT_STORED} SELECT toInt32(number % 2), toInt64(number) FROM numbers(10)"
run ${ON} --query "ALTER TABLE ${ICEBERG_NOT_STORED} ADD COLUMN simple SimpleAggregateFunction(anyLast, Nullable(String))"
run ${ON} --query "SELECT toTypeName(s), toTypeName(simple), sum(s) FROM ${ICEBERG_NOT_STORED} GROUP BY ALL"
${CLIENT} --query "CREATE TABLE ${ICEBERG_ENABLED} ENGINE = IcebergLocal('${ICEBERG_NOT_STORED_PATH}') SETTINGS ${SETTING} = 1"
run ${OFF} --query "SELECT toTypeName(s), toTypeName(simple), sum(s) FROM ${ICEBERG_ENABLED} GROUP BY ALL"

echo "-- File, setting stored in the table, queries disable it"
${CLIENT} --query "CREATE TABLE file_states (${COLUMNS}) ENGINE = File(Parquet) SETTINGS ${SETTING} = 1"
reattach file_states
insert_states file_states ${OFF}
select_states file_states ${OFF}

echo "-- File, setting not stored in the table, queries enable it"
${CLIENT} --query "CREATE TABLE file_states_not_stored (${COLUMNS}) ENGINE = File(Parquet)"
expect_refusal "${PARQUET_WRITE_REFUSED}" insert_states file_states_not_stored ${ON}
