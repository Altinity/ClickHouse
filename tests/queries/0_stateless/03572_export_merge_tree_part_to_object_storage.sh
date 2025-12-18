#!/usr/bin/env bash

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

mt_table="mt_table_${RANDOM}"
mt_table_partition_expression_with_function="mt_table_partition_expression_with_function_${RANDOM}"
s3_table="s3_table_${RANDOM}"
s3_table_wildcard="s3_table_wildcard_${RANDOM}"
s3_table_wildcard_partition_expression_with_function="s3_table_wildcard_partition_expression_with_function_${RANDOM}"
mt_table_roundtrip="mt_table_roundtrip_${RANDOM}"
big_table="big_table_${RANDOM}"
big_destination_max_bytes="big_destination_max_bytes_${RANDOM}"
big_destination_max_rows="big_destination_max_rows_${RANDOM}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip, $s3_table_wildcard, $s3_table_wildcard_partition_expression_with_function, $mt_table_partition_expression_with_function"

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

query "CREATE TABLE $s3_table_wildcard (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table_wildcard/{_partition_id}/{_file}.parquet', format=Parquet, partition_strategy='wildcard') PARTITION BY year"

echo "---- Export 2020_1_1_0 and 2021_2_2_0 to wildcard table"
query "ALTER TABLE $mt_table EXPORT PART '2020_1_1_0' TO TABLE $s3_table_wildcard SETTINGS allow_experimental_export_merge_tree_part = 1"
query "ALTER TABLE $mt_table EXPORT PART '2021_2_2_0' TO TABLE $s3_table_wildcard SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Both data parts should appear"
query "SELECT * FROM s3(s3_conn, filename='$s3_table_wildcard/**.parquet') ORDER BY id"

echo "---- Export the same part again, it should be idempotent"
query "ALTER TABLE $mt_table EXPORT PART '2020_1_1_0' TO TABLE $s3_table_wildcard SETTINGS allow_experimental_export_merge_tree_part = 1"

query "SELECT * FROM s3(s3_conn, filename='$s3_table_wildcard/**.parquet') ORDER BY id"

query "CREATE TABLE $mt_table_partition_expression_with_function (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY toString(year) ORDER BY tuple()"
query "CREATE TABLE $s3_table_wildcard_partition_expression_with_function (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table_wildcard_partition_expression_with_function/{_partition_id}/{_file}.parquet', format=Parquet, partition_strategy='wildcard') PARTITION BY toString(year)"

query "INSERT INTO $mt_table_partition_expression_with_function VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)"

echo "---- Export 2020_1_1_0 and 2021_2_2_0 to wildcard table with partition expression with function"
query "ALTER TABLE $mt_table_partition_expression_with_function EXPORT PART 'cb217c742dc7d143b61583011996a160_1_1_0' TO TABLE $s3_table_wildcard_partition_expression_with_function SETTINGS allow_experimental_export_merge_tree_part = 1"
query "ALTER TABLE $mt_table_partition_expression_with_function EXPORT PART '3be6d49ecf9749a383964bc6fab22d10_2_2_0' TO TABLE $s3_table_wildcard_partition_expression_with_function SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 1

echo "---- Both data parts should appear"
query "SELECT * FROM s3(s3_conn, filename='$s3_table_wildcard_partition_expression_with_function/**.parquet') ORDER BY id"

echo "---- Test max_bytes and max_rows per file"

query "CREATE TABLE $big_table (id UInt64, data String, year UInt16) Engine=MergeTree() order by id partition by year"

query "CREATE TABLE $big_destination_max_bytes(id UInt64, data String, year UInt16) engine=S3(s3_conn, filename='$big_destination_max_bytes', partition_strategy='hive', format=Parquet) partition by year"

query "CREATE TABLE $big_destination_max_rows(id UInt64, data String, year UInt16) engine=S3(s3_conn, filename='$big_destination_max_rows', partition_strategy='hive', format=Parquet) partition by year"

# 4194304 is a number that came up during multiple iterations, it does not really mean anything (aside from the fact that the below numbers depend on it)
query "INSERT INTO $big_table SELECT number AS id, repeat('x', 100) AS data, 2025 AS year FROM numbers(4194304)"

# make sure we have only one part
query "OPTIMIZE TABLE $big_table FINAL"

big_part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$big_table' AND partition_id = '2025' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

# this should generate ~4 files
query "ALTER TABLE $big_table EXPORT PART '$big_part' TO TABLE $big_destination_max_bytes SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_part_max_bytes_per_file=3500000, output_format_parquet_row_group_size_bytes=1000000"
# export_merge_tree_part_max_rows_per_file = 1048576 (which is 4194304/4) to generate 4 files
query "ALTER TABLE $big_table EXPORT PART '$big_part' TO TABLE $big_destination_max_rows SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_part_max_rows_per_file=1048576"

# sleeping a little longer because it will write multiple files, trying not be flaky
sleep 20

echo "---- Count files in big_destination_max_bytes, should be 5 (4 parquet, 1 commit)"
query "SELECT count(_file) FROM s3(s3_conn, filename='$big_destination_max_bytes/**', format='One')"

echo "---- Count rows in big_table and big_destination_max_bytes"
query "SELECT COUNT() from $big_table"
query "SELECT COUNT() from $big_destination_max_bytes"

echo "---- Count files in big_destination_max_rows, should be 5 (4 parquet, 1 commit)"
query "SELECT count(_file) FROM s3(s3_conn, filename='$big_destination_max_rows/**', format='One')"

echo "---- Count rows in big_table and big_destination_max_rows"
query "SELECT COUNT() from $big_table"
query "SELECT COUNT() from $big_destination_max_rows"

query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip, $s3_table_wildcard, $s3_table_wildcard_partition_expression_with_function, $mt_table_partition_expression_with_function, $big_table, $big_destination_max_bytes, $big_destination_max_rows"
