#!/usr/bin/env bash

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

query "SYSTEM STOP MERGES"

query "INSERT INTO $mt_table VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)"
query "INSERT INTO $mt_table VALUES (5, 2020), (6, 2020)"

query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- Querying merge tree for comparison. It should include both partitions (2020 and 2021)"
query "SELECT * FROM $mt_table ORDER BY id"

echo "---- Make sure only the partition 2020 has been exported"
query "SELECT DISTINCT ON (id) replaceRegexpAll(replaceRegexpAll(_path, '$s3_table', 's3_table_NAME'), '[^/]+\\.parquet', 'SNOWFLAKE_ID.parquet'), * FROM $s3_table ORDER BY id"

echo "---- It should not be allowed to export the same partition twice"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1 -- {serverError PART_IS_LOCKED}"

echo "---- Check for commit file for partition 2020"
$CLICKHOUSE_CLIENT --query "SELECT replaceRegexpAll(replaceRegexpAll(remote_file_path, '$s3_table', 's3_table_NAME'), '[^/]+\\.parquet', 'SNOWFLAKE_ID.parquet') FROM s3(s3_conn, filename='$s3_table/commit_2020_*', format='LineAsString', structure='remote_file_path String')"

echo "---- Finally, export the other partition (2021)"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2021' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- Assert both partitions are there"
query "SELECT DISTINCT ON (id) replaceRegexpAll(replaceRegexpAll(_path, '$s3_table', 's3_table_NAME'), '[^/]+\\.parquet', 'SNOWFLAKE_ID.parquet'), * FROM $s3_table ORDER BY id"

echo "---- Round trip check: create a new MergeTree table as SELECT * from s3_table"

query "CREATE TABLE $mt_table_roundtrip ENGINE = MergeTree() PARTITION BY year ORDER BY tuple() AS SELECT * FROM $s3_table"

echo "---- Data in roundtrip MergeTree table (should match s3_table)"
query "SELECT DISTINCT ON (id) * FROM $mt_table_roundtrip ORDER BY id"

query "SYSTEM START MERGES"
query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip"
