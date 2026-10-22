#!/usr/bin/env bash
# Tags: no-fasttest
# Tag no-fasttest: requires Iceberg

# EXPORT PART of a part with sparse columns into an Iceberg table:
# - cast to a wider type, the granule without zeros becomes a full chunk and the others stay sparse,
#   which the Parquet writer must not concatenate into one row group as they are;
# - the null counts written to the manifest must count the NULLs of a sparse `Nullable` column.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

# shellcheck source=./export_part.lib
. "$CUR_DIR"/export_part.lib

src="src_${RANDOM}"
dst="dst_${RANDOM}"
src_nullable="src_nullable_${RANDOM}"
dst_nullable="dst_nullable_${RANDOM}"
dst_path="${USER_FILES_PATH}/${CLICKHOUSE_DATABASE}_${dst}/"
dst_nullable_path="${USER_FILES_PATH}/${CLICKHOUSE_DATABASE}_${dst_nullable}/"

query() {
    $CLICKHOUSE_CLIENT --query "$1"
}

source_settings="ratio_of_defaults_for_sparse_serialization = 0.9, min_bytes_for_wide_part = 0, min_rows_for_wide_part = 0, index_granularity = 8192, index_granularity_bytes = '10Mi'"
insert_settings="max_insert_threads = 1, max_block_size = 300000, max_insert_block_size = 300000, min_insert_block_size_rows = 300000, min_insert_block_size_bytes = 0"
# One row group holds every chunk of the part.
export_settings="allow_experimental_export_merge_tree_part = 1, output_format_parquet_row_group_size = 1000000, output_format_parquet_row_group_size_bytes = 536870912"

echo "---- Sparse column cast to a wider type, first granule without zeros"
query "CREATE TABLE $src (id Int64, f UInt8) ENGINE = MergeTree ORDER BY id SETTINGS $source_settings, nullable_serialization_version = 'basic'"
query "INSERT INTO $src SELECT number, number < 10000 OR number % 997 = 0 FROM numbers(300000) SETTINGS $insert_settings"
query "SELECT serialization_kind FROM system.parts_columns WHERE database = currentDatabase() AND table = '$src' AND column = 'f' AND active"
query "CREATE TABLE $dst (id Int64, f Int32) ENGINE = IcebergLocal('$dst_path', 'Parquet')"

part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$src' AND active")
query "ALTER TABLE $src EXPORT PART '$part' TO TABLE $dst SETTINGS $export_settings"
wait_for_exports 1

query "SELECT count(), countIf(f != (id < 10000 OR id % 997 = 0)) FROM $dst"

echo "---- Sparse Nullable column, null counts"
query "CREATE TABLE $src_nullable (id Int64, n Nullable(Int32)) ENGINE = MergeTree ORDER BY id SETTINGS $source_settings, nullable_serialization_version = 'allow_sparse'"
query "INSERT INTO $src_nullable SELECT number, if(number % 997 = 0, toInt32(number), NULL) FROM numbers(300000) SETTINGS $insert_settings"
query "SELECT serialization_kind FROM system.parts_columns WHERE database = currentDatabase() AND table = '$src_nullable' AND column = 'n' AND active"
query "CREATE TABLE $dst_nullable (id Int64, n Nullable(Int32)) ENGINE = IcebergLocal('$dst_nullable_path', 'Parquet')"

part=$(query "SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '$src_nullable' AND active")
query "ALTER TABLE $src_nullable EXPORT PART '$part' TO TABLE $dst_nullable SETTINGS $export_settings"
wait_for_exports 2

query "SELECT countIf(n IS NULL), countIf(isNull(n) != (id % 997 != 0)) FROM $dst_nullable"
# Files are pruned by the null counts of the manifest.
query "SELECT count() FROM $dst_nullable WHERE n IS NULL SETTINGS use_iceberg_partition_pruning = 1"

query "DROP TABLE $src, $dst, $src_nullable, $dst_nullable"
rm -rf "$dst_path" "$dst_nullable_path"
