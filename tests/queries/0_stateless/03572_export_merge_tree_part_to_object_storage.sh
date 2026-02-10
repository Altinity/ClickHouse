#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: requires s3 storage

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
tf_schema_inherit="tf_schema_inherit_${RANDOM}"
tf_schema_explicit="tf_schema_explicit_${RANDOM}"
mt_table_tf="mt_table_tf_${RANDOM}"
mt_alias="mt_alias_${RANDOM}"
mt_materialized="mt_materialized_${RANDOM}"
s3_alias_export="s3_alias_export_${RANDOM}"
s3_materialized_export="s3_materialized_export_${RANDOM}"
mt_mixed="mt_mixed_${RANDOM}"
s3_mixed_export="s3_mixed_export_${RANDOM}"
mt_complex_expr="mt_complex_expr_${RANDOM}"
s3_complex_expr_export="s3_complex_expr_export_${RANDOM}"
mt_ephemeral="mt_ephemeral_${RANDOM}"
s3_ephemeral_export="s3_ephemeral_export_${RANDOM}"
s3_mixed_export_table_function="s3_mixed_export_table_function_${RANDOM}"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip, $s3_table_wildcard, $s3_table_wildcard_partition_expression_with_function, $mt_table_partition_expression_with_function, $mt_alias, $mt_materialized, $s3_alias_export, $s3_materialized_export, $mt_mixed, $s3_mixed_export, $mt_complex_expr, $s3_complex_expr_export, $mt_ephemeral, $s3_ephemeral_export"

query "CREATE TABLE $mt_table (id UInt64, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple()"
query "CREATE TABLE $s3_table (id UInt64, year UInt16) ENGINE = S3(s3_conn, filename='$s3_table', format=Parquet, partition_strategy='hive') PARTITION BY year"

query "INSERT INTO $mt_table VALUES (1, 2020), (2, 2020), (3, 2020), (4, 2021)"
echo "---- Export 2020_1_1_0 and 2021_2_2_0"
query "ALTER TABLE $mt_table EXPORT PART '2020_1_1_0' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"
query "ALTER TABLE $mt_table EXPORT PART '2021_2_2_0' TO TABLE $s3_table SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

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

query "INSERT INTO $big_table SELECT number AS id, repeat('x', 100) AS data, 2026 AS year FROM numbers(4194304)"

# make sure we have only one part
query "OPTIMIZE TABLE $big_table FINAL"

big_part_max_bytes=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$big_table' AND partition_id = '2025' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')
big_part_max_rows=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$big_table' AND partition_id = '2026' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

# this should generate ~4 files
query "ALTER TABLE $big_table EXPORT PART '$big_part_max_bytes' TO TABLE $big_destination_max_bytes SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_part_max_bytes_per_file=3500000, output_format_parquet_row_group_size_bytes=1000000"
# export_merge_tree_part_max_rows_per_file = 1048576 (which is 4194304/4) to generate 4 files
query "ALTER TABLE $big_table EXPORT PART '$big_part_max_rows' TO TABLE $big_destination_max_rows SETTINGS allow_experimental_export_merge_tree_part = 1, export_merge_tree_part_max_rows_per_file=1048576"

# sleeping a little longer because it will write multiple files, trying not be flaky
sleep 20

echo "---- Count files in big_destination_max_bytes, should be 5 (4 parquet, 1 commit)"
query "SELECT count(_file) FROM s3(s3_conn, filename='$big_destination_max_bytes/**', format='One')"

echo "---- Count rows in big_table and big_destination_max_bytes"
query "SELECT COUNT() from $big_table WHERE year = 2025"
query "SELECT COUNT() from $big_destination_max_bytes"

echo "---- Count files in big_destination_max_rows, should be 5 (4 parquet, 1 commit)"
query "SELECT count(_file) FROM s3(s3_conn, filename='$big_destination_max_rows/**', format='One')"

echo "---- Count rows in big_table and big_destination_max_rows"
query "SELECT COUNT() from $big_table WHERE year = 2026"
query "SELECT COUNT() from $big_destination_max_rows"

echo "---- Table function with schema inheritance (no schema specified)"
query "CREATE TABLE $mt_table_tf (id UInt64, value String, year UInt16) ENGINE = MergeTree() PARTITION BY year ORDER BY tuple()"
query "INSERT INTO $mt_table_tf VALUES (100, 'test1', 2022), (101, 'test2', 2022), (102, 'test3', 2023)"

query "ALTER TABLE $mt_table_tf EXPORT PART '2022_1_1_0' TO TABLE FUNCTION s3(s3_conn, filename='$tf_schema_inherit', format='Parquet', partition_strategy='hive') PARTITION BY year SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Data should be exported with inherited schema"
query "SELECT * FROM s3(s3_conn, filename='$tf_schema_inherit/**.parquet') ORDER BY id"

echo "---- Table function with explicit compatible schema"
query "ALTER TABLE $mt_table_tf EXPORT PART '2023_2_2_0' TO TABLE FUNCTION s3(s3_conn, filename='$tf_schema_explicit', format='Parquet', structure='id UInt64, value String, year UInt16', partition_strategy='hive') PARTITION BY year SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Data should be exported with explicit schema"
query "SELECT * FROM s3(s3_conn, filename='$tf_schema_explicit/**.parquet') ORDER BY id"

echo "---- Test ALIAS columns export"
query "CREATE TABLE $mt_alias (a UInt32, arr Array(UInt64), arr_1 UInt64 ALIAS arr[1]) ENGINE = MergeTree() PARTITION BY a ORDER BY (a, arr[1]) SETTINGS index_granularity = 1"
query "CREATE TABLE $s3_alias_export (a UInt32, arr Array(UInt64), arr_1 UInt64) ENGINE = S3(s3_conn, filename='$s3_alias_export', format=Parquet, partition_strategy='hive') PARTITION BY a"

query "INSERT INTO $mt_alias VALUES (1, [1, 2, 3]), (1, [10, 20, 30])"

alias_part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$mt_alias' AND partition_id = '1' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

query "ALTER TABLE $mt_alias EXPORT PART '$alias_part' TO TABLE $s3_alias_export SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Verify ALIAS column data in source table (arr_1 computed from arr[1])"
query "SELECT a, arr, arr_1 FROM $mt_alias ORDER BY arr"

echo "---- Verify ALIAS column data exported to S3 (should match source)"
query "SELECT a, arr, arr_1 FROM $s3_alias_export ORDER BY arr"

echo "---- Test MATERIALIZED columns export"
query "CREATE TABLE $mt_materialized (a UInt32, arr Array(UInt64), arr_1 UInt64 MATERIALIZED arr[1]) ENGINE = MergeTree() PARTITION BY a ORDER BY (a, arr_1) SETTINGS index_granularity = 1"
query "CREATE TABLE $s3_materialized_export (a UInt32, arr Array(UInt64), arr_1 UInt64) ENGINE = S3(s3_conn, filename='$s3_materialized_export', format=Parquet, partition_strategy='hive') PARTITION BY a"

query "INSERT INTO $mt_materialized VALUES (1, [1, 2, 3]), (1, [10, 20, 30])"

materialized_part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$mt_materialized' AND partition_id = '1' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

query "ALTER TABLE $mt_materialized EXPORT PART '$materialized_part' TO TABLE $s3_materialized_export SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Verify MATERIALIZED column data in source table (arr_1 computed from arr[1])"
query "SELECT a, arr, arr_1 FROM $mt_materialized ORDER BY arr"

echo "---- Verify MATERIALIZED column data exported to S3 (should match source)"
query "SELECT a, arr, arr_1 FROM $s3_materialized_export ORDER BY arr"

echo "---- Test EPHEMERAL column (not stored, ignored during export)"
query "CREATE TABLE $mt_ephemeral (
    id UInt32,
    name_input String EPHEMERAL,
    name_upper String DEFAULT upper(name_input)
) ENGINE = MergeTree() PARTITION BY id ORDER BY id SETTINGS index_granularity = 1"

query "CREATE TABLE $s3_ephemeral_export (
    id UInt32,
    name_upper String
) ENGINE = S3(s3_conn, filename='$s3_ephemeral_export', format=Parquet, partition_strategy='hive') PARTITION BY id"

query "INSERT INTO $mt_ephemeral (id, name_input) VALUES (1, 'alice'), (1, 'bob')"

ephemeral_part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$mt_ephemeral' AND partition_id = '1' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

query "ALTER TABLE $mt_ephemeral EXPORT PART '$ephemeral_part' TO TABLE $s3_ephemeral_export SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Verify data in source"
query "SELECT id, name_upper FROM $mt_ephemeral ORDER BY name_upper"

echo "---- Verify exported data"
query "SELECT id, name_upper FROM $s3_ephemeral_export ORDER BY name_upper"

echo "---- Test Mixed ALIAS, MATERIALIZED, and EPHEMERAL in same table"
query "CREATE TABLE $mt_mixed (
    id UInt32,
    value UInt32,
    tag_input String EPHEMERAL,
    doubled UInt64 ALIAS value * 2,
    tripled UInt64 MATERIALIZED value * 3,
    tag String DEFAULT upper(tag_input)
) ENGINE = MergeTree() PARTITION BY id ORDER BY id SETTINGS index_granularity = 1"

query "CREATE TABLE $s3_mixed_export (
    id UInt32,
    value UInt32,
    doubled UInt64,
    tripled UInt64,
    tag String
) ENGINE = S3(s3_conn, filename='$s3_mixed_export', format=Parquet, partition_strategy='hive') PARTITION BY id"

query "INSERT INTO $mt_mixed (id, value, tag_input) VALUES (1, 5, 'test'), (1, 10, 'prod')"

mixed_part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$mt_mixed' AND partition_id = '1' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

query "ALTER TABLE $mt_mixed EXPORT PART '$mixed_part' TO TABLE $s3_mixed_export SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Verify mixed columns in source table"
query "SELECT id, value, doubled, tripled, tag FROM $mt_mixed ORDER BY value"

echo "---- Verify mixed columns exported to S3 (should match source)"
query "SELECT id, value, doubled, tripled, tag FROM $s3_mixed_export ORDER BY value"

echo "---- Test Export to Table Function with mixed columns"

query "ALTER TABLE $mt_mixed EXPORT PART '$mixed_part' TO TABLE FUNCTION s3(s3_conn, filename='$s3_mixed_export_table_function', format=Parquet, partition_strategy='hive') PARTITION BY id SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Verify mixed columns exported to S3"
query "SELECT * FROM s3(s3_conn, filename='$s3_mixed_export_table_function/**.parquet', format=Parquet) ORDER BY value"

echo "---- Test Complex Expressions in computed columns"
query "CREATE TABLE $mt_complex_expr (
    id UInt32,
    name String,
    upper_name String ALIAS upper(name),
    concat_result String MATERIALIZED concat(name, '-', toString(id))
) ENGINE = MergeTree() PARTITION BY id ORDER BY id SETTINGS index_granularity = 1"

query "CREATE TABLE $s3_complex_expr_export (
    id UInt32,
    name String,
    upper_name String,
    concat_result String
) ENGINE = S3(s3_conn, filename='$s3_complex_expr_export', format=Parquet, partition_strategy='hive') PARTITION BY id"

query "INSERT INTO $mt_complex_expr (id, name) VALUES (1, 'alice'), (1, 'bob')"

complex_expr_part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$mt_complex_expr' AND partition_id = '1' AND active = 1 ORDER BY name LIMIT 1" | tr -d '\n')

query "ALTER TABLE $mt_complex_expr EXPORT PART '$complex_expr_part' TO TABLE $s3_complex_expr_export SETTINGS allow_experimental_export_merge_tree_part = 1"

sleep 3

echo "---- Verify complex expressions in source table"
query "SELECT id, name, upper_name, concat_result FROM $mt_complex_expr ORDER BY name"

echo "---- Verify complex expressions exported to S3 (should match source)"
query "SELECT id, name, upper_name, concat_result FROM $s3_complex_expr_export ORDER BY name"

query "DROP TABLE IF EXISTS $mt_table, $s3_table, $mt_table_roundtrip, $mt_table_tf, $s3_table_wildcard, $s3_table_wildcard_partition_expression_with_function, $mt_table_partition_expression_with_function, $big_table, $big_destination_max_bytes, $big_destination_max_rows, $mt_alias, $mt_materialized, $s3_alias_export, $s3_materialized_export, $mt_ephemeral, $s3_ephemeral_export, $mt_mixed, $s3_mixed_export, $mt_complex_expr, $s3_complex_expr_export"
