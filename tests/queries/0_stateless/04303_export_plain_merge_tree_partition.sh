#!/usr/bin/env bash
# Tags: no-fasttest, no-shared-merge-tree
# no-fasttest: requires S3 / MinIO.
# no-shared-merge-tree: this test exercises EXPORT PARTITION on a plain (non-replicated) MergeTree.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

mt_table="mt_table_${CLICKHOUSE_DATABASE}"
s3_table="s3_table_${CLICKHOUSE_DATABASE}"
mt_roundtrip="mt_roundtrip_${CLICKHOUSE_DATABASE}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

# Poll system.partition_exports until the given partition reaches the expected status (or timeout).
wait_for_status() {
    local partition_id="$1"
    local expected="$2"
    local i=0
    while [ "$i" -lt 120 ]; do
        status=$(query "SELECT status FROM system.partition_exports WHERE source_table = '$mt_table' AND destination_table = '$s3_table' AND partition_id = '$partition_id'")
        if [ "$status" = "$expected" ]; then
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    echo "TIMEOUT waiting for partition $partition_id to reach $expected (last: '$status')"
    return 1
}

query "DROP TABLE IF EXISTS $mt_table"
query "DROP TABLE IF EXISTS $s3_table"
query "DROP TABLE IF EXISTS $mt_roundtrip"

query "CREATE TABLE $mt_table (id UInt64, year UInt16) ENGINE = MergeTree PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $s3_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table', format=Parquet, partition_strategy='hive') PARTITION BY year"

# Stop merges so the number of parts per partition stays stable for the assertions below.
query "SYSTEM STOP MERGES $mt_table"

query "INSERT INTO $mt_table VALUES (1, 2020), (2, 2020), (4, 2021)"
query "INSERT INTO $mt_table VALUES (3, 2020), (5, 2021)"
query "INSERT INTO $mt_table VALUES (6, 2022), (7, 2022)"

echo "Export partition 2020"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table"
wait_for_status "2020" "COMPLETED"

echo "Export partition 2021"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2021' TO TABLE $s3_table"
wait_for_status "2021" "COMPLETED"

echo "Select from destination table (2020, 2021)"
query "SELECT * FROM $s3_table ORDER BY id"

echo "Re-exporting 2020 without force is rejected"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table" 2>&1 | grep -o "EXPORT_PARTITION_ALREADY_EXPORTED" | head -1

echo "Export remaining partitions with EXPORT PARTITION ALL (skip existing)"
query "ALTER TABLE $mt_table EXPORT PARTITION ALL TO TABLE $s3_table SETTINGS export_merge_tree_partition_all_on_error = 'skip_conflicts'"
wait_for_status "2022" "COMPLETED"

echo "Select from destination table (all partitions)"
query "SELECT * FROM $s3_table ORDER BY id"

echo "Roundtrip: create a MergeTree table from the exported S3 data"
query "CREATE TABLE $mt_roundtrip ENGINE = MergeTree PARTITION BY year ORDER BY tuple() AS SELECT * FROM $s3_table"
query "SELECT * FROM $mt_roundtrip ORDER BY id"

echo "system.partition_exports statuses"
query "SELECT partition_id, status, parts_count, parts_to_do FROM system.partition_exports WHERE source_table = '$mt_table' AND destination_table = '$s3_table' ORDER BY partition_id"

query "DROP TABLE IF EXISTS $mt_table"
query "DROP TABLE IF EXISTS $s3_table"
query "DROP TABLE IF EXISTS $mt_roundtrip"
