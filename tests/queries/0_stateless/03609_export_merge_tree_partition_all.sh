#!/usr/bin/env bash
# Tags: no-fasttest, replica, no-parallel, no-replicated-database

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

rmt_table="rmt_all_${RANDOM}"
s3_table="s3_all_${RANDOM}"
empty_rmt="empty_rmt_${RANDOM}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

expect_error() {
    local code_name="$1"
    local sql="$2"
    $CLICKHOUSE_CLIENT --query "$sql" 2>&1 | tr '\n' ' ' | grep -oF "$code_name" > /dev/null \
        && echo "OK" || echo "FAIL"
}

query "DROP TABLE IF EXISTS $rmt_table, $s3_table, $empty_rmt"

query "CREATE TABLE $rmt_table (id UInt64, year UInt16) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/$rmt_table', 'replica1') PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $s3_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table', format=Parquet, partition_strategy='hive') PARTITION BY year"

query "INSERT INTO $rmt_table VALUES (1, 2020), (2, 2021), (3, 2022)"
query "SYSTEM SYNC REPLICA $rmt_table"

echo "---- Happy path: EXPORT PARTITION ALL schedules every active partition (default throw_first)"
query "ALTER TABLE $rmt_table EXPORT PARTITION ALL TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"

# Wait for the async per-partition exports to complete.
sleep 15

echo "---- Source"
query "SELECT * FROM $rmt_table ORDER BY id"
echo "---- Destination"
query "SELECT * FROM $s3_table ORDER BY id"

echo "---- Empty table: throws BAD_ARGUMENTS"
query "CREATE TABLE $empty_rmt (id UInt64, year UInt16) ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/$empty_rmt', 'replica1') PARTITION BY year ORDER BY tuple()"
expect_error "BAD_ARGUMENTS" \
    "ALTER TABLE $empty_rmt EXPORT PARTITION ALL TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"

echo "---- throw_first conflict: re-running aborts on first already-exported partition"
expect_error "EXPORT_PARTITION_ALREADY_EXPORTED" \
    "ALTER TABLE $rmt_table EXPORT PARTITION ALL TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_partition_all_on_error = 'throw_first'"

echo "---- collect conflict: aggregated PARTITION_EXPORT_FAILED at the end"
expect_error "PARTITION_EXPORT_FAILED" \
    "ALTER TABLE $rmt_table EXPORT PARTITION ALL TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_partition_all_on_error = 'collect'"

echo "---- skip_conflicts: re-run succeeds silently"
query "ALTER TABLE $rmt_table EXPORT PARTITION ALL TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_partition_all_on_error = 'skip_conflicts'"
echo "OK"

query "DROP TABLE IF EXISTS $rmt_table, $s3_table, $empty_rmt"
