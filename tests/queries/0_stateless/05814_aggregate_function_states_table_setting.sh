#!/usr/bin/env bash
# Tags: no-fasttest
# `allow_aggregate_function_states_in_open_formats` in a table's `SETTINGS` is stored with the table and decides
# for every query on it, also after the table is re-attached. Without it in the table, the query setting decides.

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
ICEBERG_REFUSED="${ICEBERG}_refused"

cleanup()
{
    for table in "${ICEBERG}" "${ICEBERG_NOT_STORED}" "${ICEBERG_REFUSED}" file_states file_states_not_stored \
        file_states_refused; do
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

echo "-- Iceberg, setting not stored in the table"
expect_refusal "${CREATE_OR_ALTER_REFUSED}" run ${OFF} --query "CREATE TABLE ${ICEBERG_NOT_STORED} (${COLUMNS}) ENGINE = IcebergLocal('${ICEBERG_NOT_STORED_PATH}')"
${CLIENT} "--${SETTING}=1" --query "CREATE TABLE ${ICEBERG_NOT_STORED} (${COLUMNS}) ENGINE = IcebergLocal('${ICEBERG_NOT_STORED_PATH}')"
${CLIENT} --query "SHOW CREATE TABLE ${ICEBERG_NOT_STORED}" | grep -c "${SETTING}" || true
reattach "${ICEBERG_NOT_STORED}"
expect_refusal "Column 'extra' ${CREATE_OR_ALTER_REFUSED}" run ${OFF} --query "ALTER TABLE ${ICEBERG_NOT_STORED} ADD COLUMN extra AggregateFunction(max, UInt64)"
# A `SimpleAggregateFunction` holds ordinary values of its storage type, so it needs no setting.
run ${OFF} --query "ALTER TABLE ${ICEBERG_NOT_STORED} ADD COLUMN simple SimpleAggregateFunction(anyLast, Nullable(String))"
expect_refusal "${READ_REFUSED}" select_states "${ICEBERG_NOT_STORED}" ${OFF}

echo "-- File, setting stored in the table, queries disable it"
${CLIENT} --query "CREATE TABLE file_states (${COLUMNS}) ENGINE = File(Parquet) SETTINGS ${SETTING} = 1"
reattach file_states
insert_states file_states ${OFF}
select_states file_states ${OFF}

echo "-- File, setting not stored in the table"
${CLIENT} --query "CREATE TABLE file_states_not_stored (${COLUMNS}) ENGINE = File(Parquet)"
expect_refusal "${PARQUET_WRITE_REFUSED}" insert_states file_states_not_stored ${OFF}

echo "-- Iceberg, setting 0 stored in the table, queries enable it"
expect_refusal "${READ_REFUSED}" run "--${SETTING}=1" --query "
    CREATE TABLE ${ICEBERG_REFUSED} ENGINE = IcebergLocal('${ICEBERG_NOT_STORED_PATH}') SETTINGS ${SETTING} = 0"

echo "-- File, setting 0 stored in the table, queries enable it"
${CLIENT} --query "CREATE TABLE file_states_refused (${COLUMNS}) ENGINE = File(Parquet) SETTINGS ${SETTING} = 0"
expect_refusal "${PARQUET_WRITE_REFUSED}" insert_states file_states_refused "--${SETTING}=1"
