#!/usr/bin/env bash

mt_table="mt_table_${RANDOM}"
s3_table="s3_table_${RANDOM}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

query "DROP TABLE IF EXISTS $mt_table, $s3_table"

query "CREATE TABLE $mt_table (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $s3_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table', format=Parquet, partition_strategy='hive') PARTITION BY year"

query "SYSTEM STOP MERGES"

query "INSERT INTO $mt_table VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- Insert some more data in the already exported partition"
query "SELECT DISTINCT ON (id) replaceRegexpAll(_path, '$s3_table', 's3_table_NAME') FROM $s3_table ORDER BY id"

echo "---- Export the parts, only the diff should be exported"
query "INSERT INTO $mt_table VALUES (5, 2020), (6, 2020)"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- New data part should appear (2020_2_2_0.parquet) with id 5 and 6"
query "SELECT DISTINCT ON (id) replaceRegexpAll(_path, '$s3_table', 's3_table_NAME') FROM $s3_table ORDER BY id"

echo "---- Merge all parts"
query "SYSTEM START MERGES"
query "OPTIMIZE TABLE $mt_table FINAL"
query "SYSTEM STOP MERGES"

echo "---- Nothing should be exported even though the parts in the merge tree table have been merged"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- Check the parts in the remote storage have not been touched"
query "SELECT DISTINCT ON (id) replaceRegexpAll(_path, '$s3_table', 's3_table_NAME') FROM $s3_table ORDER BY id"

echo "---- Yet another part"
query "INSERT INTO $mt_table VALUES (7, 2020)"

echo "---- Merge the new part"
query "SYSTEM START MERGES"
query "OPTIMIZE TABLE $mt_table FINAL"
query "SYSTEM STOP MERGES"

echo "---- The new part that cover everything should be exported"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2020' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- Assert there is only one data part"
query "SELECT DISTINCT ON (id) replaceRegexpAll(_path, '$s3_table', 's3_table_NAME') FROM $s3_table ORDER BY id"

echo "---- Finally, export the other partition (2021)"
query "ALTER TABLE $mt_table EXPORT PARTITION ID '2021' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_partition = 1, export_merge_tree_partition_background_execution = 0"

echo "---- Assert both partitions are there"
query "SELECT DISTINCT ON (id) replaceRegexpAll(_path, '$s3_table', 's3_table_NAME') FROM $s3_table ORDER BY id"

echo "---- Selecting from merge tree to have a sample of what the data in remote storage should look like"
query "SELECT DISTINCT ON (id) id, year FROM $mt_table ORDER BY id"

query "SYSTEM START MERGES"
query "DROP TABLE IF EXISTS $mt_table, $s3_table"

