#!/usr/bin/env bash
# Tags: no-fasttest

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

mt_table="mt_table_${RANDOM}"
s3_table="s3_table_${RANDOM}"
mt_table_roundtrip="mt_table_roundtrip_${RANDOM}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip"

query "CREATE TABLE $mt_table (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $s3_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table', format=Parquet, partition_strategy='hive') PARTITION BY year"

query "INSERT INTO $mt_table VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)"
echo "---- Export 2020_1_1_0 and 2021_2_2_0"
query "ALTER TABLE $mt_table EXPORT PART '2020_1_1_0' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"
query "ALTER TABLE $mt_table EXPORT PART '2021_2_2_0' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"

echo "---- Both data parts should appear"
query "SELECT * FROM $s3_table ORDER BY id"

echo "---- Export the same part again, it should be idempotent"
query "ALTER TABLE $mt_table EXPORT PART '2020_1_1_0' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"

query "SELECT * FROM $s3_table ORDER BY id"

query "CREATE TABLE $mt_table_roundtrip ENGINE = MergeTree() PARTITION BY year ORDER BY tuple() AS SELECT * FROM $s3_table"

echo "---- Data in roundtrip MergeTree table (should match s3_table)"
query "SELECT * FROM $s3_table ORDER BY id"

query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip"
