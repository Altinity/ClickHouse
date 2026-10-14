import os
import shutil
import time
import pytest

from helpers.s3_tools import S3Uploader
from helpers.iceberg_utils import default_upload_directory, get_uuid_str
from .external_paths_utils import (
    ALL_ROWS,
    create_and_upload_table,
    _create_iceberg_s3_table,
    _distribute_table_components,
    _download_table_for_relocation,
    _external_delete_files_in_another_bucket,
    _rewrite_manifests_and_reupload,
    external_bucket,
    find_files,
    get_query_args,
    modify_avro_file,
    read_avro_records,
    relocate_data_files_to_bucket,
)


@pytest.mark.parametrize("spelling", ["s3", "url"])
def test_four_different_s3_buckets(started_cluster_iceberg_with_spark, spelling):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session

    TABLE_NAME = f"test_four_buckets_{spelling}_{get_uuid_str()}"
    buckets = [
        started_cluster_iceberg_with_spark.minio_bucket,
        external_bucket(started_cluster_iceberg_with_spark, 1),
        external_bucket(started_cluster_iceberg_with_spark, 2),
        external_bucket(started_cluster_iceberg_with_spark, 3),
    ]

    spark.sql(f"CREATE TABLE {TABLE_NAME} (id INT, name STRING, score INT) USING iceberg OPTIONS('format-version'='2')")
    spark.sql(f"INSERT INTO {TABLE_NAME} VALUES (1, 'Alice', 100), (2, 'Bob', 85), (3, 'Carol', 92)")

    default_upload_directory(started_cluster_iceberg_with_spark, "s3", f"/iceberg_data/default/{TABLE_NAME}/", f"/iceberg_data/default/{TABLE_NAME}/")

    base_path = _distribute_table_components(started_cluster_iceberg_with_spark, TABLE_NAME, f"s3:{buckets[0]}",
                                             *(f"{spelling}:{b}" for b in buckets[1:]))

    minio_url = f"http://{started_cluster_iceberg_with_spark.minio_host}:{started_cluster_iceberg_with_spark.minio_port}"
    result = instance.query(f"SELECT * FROM icebergS3(s3, filename='{base_path}/', format=Parquet, url='{minio_url}/{buckets[0]}/') ORDER BY id")

    assert result == "1\tAlice\t100\n2\tBob\t85\n3\tCarol\t92\n"


def test_num_rows_cache_no_collision_across_buckets(started_cluster_iceberg_with_spark):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session

    minio_url = f"http://{started_cluster_iceberg_with_spark.minio_host}:{started_cluster_iceberg_with_spark.minio_port}"
    base_bucket = started_cluster_iceberg_with_spark.minio_bucket
    shared_key = f"shared_count_cache_{get_uuid_str()}/data/part-0.parquet"

    def prepare_table(table_name, kept_values_sql, kept_rows, data_bucket):
        # A second append writes a second data file, and deleting one of its two rows merge-on-read
        # leaves a live v2 position delete file referencing only that file. Live position deletes make
        # `count()` scan the data files instead of answering from the manifests, so the first data
        # file -- relocated below, with no deletes applied -- goes through the num-rows cache. The
        # second file keeps a surviving row: deleting every row of a file drops it from the manifest
        # without writing a delete file.
        spark.sql(
            f"""
                CREATE TABLE {table_name} (id INT, value STRING)
                USING iceberg
                TBLPROPERTIES (
                    'format-version' = '2',
                    'write.delete.mode' = 'merge-on-read'
                )
            """
        )
        spark.sql(f"INSERT INTO {table_name} VALUES {kept_values_sql}")
        spark.sql(f"INSERT INTO {table_name} VALUES (998, 'survivor'), (999, 'deleted')")
        spark.sql(f"DELETE FROM {table_name} WHERE id = 999")

        default_upload_directory(started_cluster_iceberg_with_spark, "s3", f"/iceberg_data/default/{table_name}/", f"/iceberg_data/default/{table_name}/")

        temp_dir, host_path, base_path = _download_table_for_relocation(started_cluster_iceberg_with_spark, table_name)
        metadata_dir = os.path.join(host_path, "metadata")
        data_dir = os.path.join(host_path, "data")

        manifest_files = [f for f in find_files(metadata_dir, ".avro") if not os.path.basename(f).startswith("snap-")]

        # `content` 0 is DATA and 1 is POSITION DELETES; `status` 2 is DELETED. Pick the live data
        # file holding the kept rows -- the delete file references the other one.
        live_entries = [
            r["data_file"] for mf in manifest_files for r in read_avro_records(mf) if r["status"] != 2
        ]
        assert any(e["content"] == 1 for e in live_entries), \
            "Expected a live position delete file; is `write.delete.mode` still merge-on-read?"
        kept_paths = {e["file_path"] for e in live_entries if e["content"] == 0 and e["record_count"] == kept_rows}
        assert len(kept_paths) == 1, f"Expected one data file with {kept_rows} records, got: {live_entries}"
        relocated_path = next(iter(kept_paths))

        # Point that data file -- and only it -- at the same object key in a different bucket.
        for mf in manifest_files:
            modify_avro_file(
                mf,
                ["data_file", "file_path"],
                lambda p: f"s3a://{data_bucket}/{shared_key}" if p == relocated_path else p,
            )
            # Without `value_counts` the per-file row count is not answered from the manifest, which
            # would bypass the num-rows cache under test.
            modify_avro_file(mf, ["data_file", "value_counts"], lambda _: None)

        for f in manifest_files:
            rel = os.path.relpath(f, host_path)
            started_cluster_iceberg_with_spark.default_s3_uploader.upload_file(f, f"{base_path}/{rel}")

        relocated_basename = os.path.basename(relocated_path)
        local_relocated = [f for f in find_files(data_dir, ".parquet") if os.path.basename(f) == relocated_basename]
        assert len(local_relocated) == 1, f"Could not find {relocated_basename} under {data_dir}"
        S3Uploader(started_cluster_iceberg_with_spark.minio_client, data_bucket).upload_file(local_relocated[0], shared_key)

        shutil.rmtree(temp_dir)
        return base_path

    # Upload both files before populating the cache: entries are reused only for older files.
    base_path_a = prepare_table(
        f"test_count_cache_a_{get_uuid_str()}", "(1, 'a'), (2, 'b'), (3, 'c')", 3, external_bucket(started_cluster_iceberg_with_spark, 1)
    )
    base_path_b = prepare_table(
        f"test_count_cache_b_{get_uuid_str()}", "(1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e')", 5, external_bucket(started_cluster_iceberg_with_spark, 2)
    )
    # Allow for the second-resolution `last_modified` comparison.
    time.sleep(3)

    def count(base_path, marker):
        result = instance.query(
            f"SELECT /* {marker} */ count() FROM icebergS3(s3, filename='{base_path}/', format=Parquet, url='{minio_url}/{base_bucket}/') "
            "SETTINGS optimize_trivial_count_query = 1, optimize_count_from_files = 1, use_cache_for_count_from_files = 1"
        ).strip()
        instance.query("SYSTEM FLUSH LOGS")
        cache_lookups = int(instance.query(
            "SELECT ProfileEvents['SchemaInferenceCacheHits'] + ProfileEvents['SchemaInferenceCacheMisses'] "
            f"FROM system.query_log WHERE type = 'QueryFinish' AND query LIKE '%{marker}%' AND query NOT LIKE '%query_log%' "
            "ORDER BY event_time_microseconds DESC LIMIT 1"
        ).strip())
        return result, cache_lookups

    # Each table holds its relocated file plus the surviving row of the second file, so a reused
    # entry from table `a` would show up as 4 rather than 6 for table `b`.
    count_a, cache_lookups_a = count(base_path_a, "count_cache_marker_a")
    count_b, cache_lookups_b = count(base_path_b, "count_cache_marker_b")
    assert count_a == "4"
    assert count_b == "6"
    assert cache_lookups_a >= 1
    assert cache_lookups_b >= 1


def test_external_path_virtual_column_filter(started_cluster_iceberg_with_spark):
    instance = started_cluster_iceberg_with_spark.instances["node1"]

    TABLE_NAME = f"test_path_filter_{get_uuid_str()}"
    data_bucket = external_bucket(started_cluster_iceberg_with_spark)

    create_and_upload_table(started_cluster_iceberg_with_spark, TABLE_NAME)

    base_path = relocate_data_files_to_bucket(started_cluster_iceberg_with_spark, TABLE_NAME, data_bucket)

    minio_url = f"http://{started_cluster_iceberg_with_spark.minio_host}:{started_cluster_iceberg_with_spark.minio_port}"
    args = f"s3, filename='{base_path}/', format=Parquet, url='{minio_url}/{started_cluster_iceberg_with_spark.minio_bucket}/'"

    paths = instance.query(f"SELECT DISTINCT _path FROM icebergS3({args})").strip().splitlines()
    assert len(paths) == 1
    external_path = paths[0]
    assert data_bucket in external_path

    assert instance.query(
        f"SELECT count() FROM icebergS3Cluster('cluster_simple', {args}) WHERE _path = '{external_path}' "
        "SETTINGS cluster_table_function_split_granularity = 'bucket'"
    ).strip() == "3"


def test_gs_path_is_not_served_by_same_named_s3_bucket(started_cluster_iceberg_with_spark):
    instance = started_cluster_iceberg_with_spark.instances["node1"]

    TABLE_NAME = f"test_gs_same_bucket_{get_uuid_str()}"

    create_and_upload_table(started_cluster_iceberg_with_spark, TABLE_NAME)

    temp_dir, host_path, base_path = _download_table_for_relocation(started_cluster_iceberg_with_spark, TABLE_NAME)
    _rewrite_manifests_and_reupload(
        started_cluster_iceberg_with_spark, host_path, base_path,
        lambda p: f"gs://{started_cluster_iceberg_with_spark.minio_bucket}/{base_path}/data/{os.path.basename(p)}")
    shutil.rmtree(temp_dir)

    _create_iceberg_s3_table(started_cluster_iceberg_with_spark, TABLE_NAME, base_path)

    # GCS is unreachable here; a successful read would mean the MinIO bucket was used incorrectly.
    error = instance.query_and_get_error(
        f"SELECT * FROM {TABLE_NAME} ORDER BY id "
        f"SETTINGS s3_request_timeout_ms = 3000, s3_connect_timeout_ms = 3000")
    assert "alpha" not in error

    instance.query(f"DETACH TABLE {TABLE_NAME}")


def test_unsupported_scheme_is_still_reported_by_system_iceberg_files(started_cluster_iceberg_with_spark):
    instance = started_cluster_iceberg_with_spark.instances["node1"]

    TABLE_NAME = f"test_unsupported_scheme_{get_uuid_str()}"

    create_and_upload_table(started_cluster_iceberg_with_spark, TABLE_NAME)

    temp_dir, host_path, base_path = _download_table_for_relocation(started_cluster_iceberg_with_spark, TABLE_NAME)
    _rewrite_manifests_and_reupload(
        started_cluster_iceberg_with_spark, host_path, base_path,
        lambda p: f"viewfs://cluster/{base_path}/data/{os.path.basename(p)}")
    shutil.rmtree(temp_dir)

    _create_iceberg_s3_table(started_cluster_iceberg_with_spark, TABLE_NAME, base_path)

    assert "Unsupported storage scheme" in instance.query_and_get_error(f"SELECT * FROM {TABLE_NAME} ORDER BY id")

    paths = instance.query(
        f"SELECT file_path FROM system.iceberg_files "
        f"WHERE database = 'default' AND table = '{TABLE_NAME}' AND content = 'DATA' ORDER BY file_path").split()
    assert paths, "The unreadable table was dropped from system.iceberg_files"
    assert all(p.startswith("viewfs://cluster/") for p in paths), paths

    assert "Unsupported storage scheme" in instance.query_and_get_error(f"SELECT count() FROM {TABLE_NAME}")

    instance.query(f"DETACH TABLE {TABLE_NAME}")


def test_cluster_function_reads_external_delete_file(started_cluster_iceberg_with_spark):
    instance = started_cluster_iceberg_with_spark.instances["node1"]

    TABLE_NAME = f"test_cluster_external_delete_{get_uuid_str()}"
    base_path, _, _ = _external_delete_files_in_another_bucket(started_cluster_iceberg_with_spark, TABLE_NAME)

    args = get_query_args("s3", started_cluster_iceberg_with_spark, base_path)
    assert instance.query(f"SELECT * FROM icebergS3Cluster('cluster_simple', {args}) ORDER BY id") == "1\talpha\n3\tgamma\n"


@pytest.mark.parametrize("metadata_storage,manifest_list_storage,manifest_storage,data_storage", [
    ("s3", "local", "s3", "s3"),
    ("s3", "s3", "local", "s3"),
    ("azure", "azure", "azure", "local"),
])
def test_multi_storage_combinations(started_cluster_iceberg_with_spark, metadata_storage, manifest_list_storage, manifest_storage, data_storage):
    instance = started_cluster_iceberg_with_spark.instances["node1"]

    TABLE_NAME = f"test_combo_{get_uuid_str()}"

    create_and_upload_table(started_cluster_iceberg_with_spark, TABLE_NAME)

    base_path = _distribute_table_components(started_cluster_iceberg_with_spark, TABLE_NAME, metadata_storage,
                                             manifest_list_storage, manifest_storage, data_storage)

    func = {"s3": "icebergS3", "azure": "icebergAzure", "local": "icebergLocal"}[metadata_storage.split(":")[0]]
    args = get_query_args(metadata_storage, started_cluster_iceberg_with_spark, base_path)

    assert instance.query(f"SELECT * FROM {func}({args}) ORDER BY id") == ALL_ROWS
