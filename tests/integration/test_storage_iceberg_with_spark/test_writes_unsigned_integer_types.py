import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    default_download_directory,
    default_upload_directory,
    get_creation_expression,
    get_uuid_str,
)


@pytest.mark.parametrize("storage_type", ["s3", "local"])
def test_writes_uint32_widens_to_long(started_cluster_iceberg_with_spark, storage_type):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    table_name = "test_writes_uint32_widens_to_long_" + storage_type + "_" + get_uuid_str()

    create_iceberg_table(
        storage_type,
        instance,
        table_name,
        started_cluster_iceberg_with_spark,
        "(u32 UInt32, i32 Int32)",
    )

    instance.query(
        f"INSERT INTO {table_name} SELECT 4294967290, -5",
        settings={"allow_insert_into_iceberg": 1},
    )

    assert instance.query(f"SELECT u32, i32 FROM {table_name}") == "4294967290\t-5\n"

    path = f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}"
    default_download_directory(started_cluster_iceberg_with_spark, storage_type, f"{path}/", f"{path}/")

    table = spark.read.format("iceberg").load(path)
    assert dict(table.dtypes) == {"u32": "bigint", "i32": "int"}
    row = table.collect()[0]
    assert (row["u32"], row["i32"]) == (4294967290, -5)


def test_writes_reject_uint64(started_cluster_iceberg_with_spark):
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    table_name = "test_writes_reject_uint64_" + get_uuid_str()

    assert "BAD_ARGUMENTS" in instance.query_and_get_error(
        get_creation_expression(
            "local", table_name, started_cluster_iceberg_with_spark, "(x UInt64)"
        )
    )


def test_writes_uint32_declared_over_int_field_writes_int_bounds(started_cluster_iceberg_with_spark):
    """
    A table created by an earlier release stores a `UInt32` column as Iceberg `int`. Inserting
    into it must keep writing 4-byte `int` bounds, not the 8-byte `long` bounds that `UInt32`
    columns of tables created by this release get.

    The Iceberg table is created with Spark, so field `u` is `int`, and is then declared in
    ClickHouse as `UInt32`, the way such a table is loaded after an upgrade.
    """
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    storage_type = "local"
    table_name = "test_writes_uint32_over_int_bounds_" + storage_type + "_" + get_uuid_str()

    spark.sql(
        f"CREATE TABLE {table_name} (u int) USING iceberg TBLPROPERTIES ('format-version' = '2')"
    )
    spark.sql(f"INSERT INTO {table_name} VALUES (5)")

    default_upload_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"/iceberg_data/default/{table_name}/",
        f"/iceberg_data/default/{table_name}/",
    )
    # `IF NOT EXISTS`: a `CREATE` with explicit columns over a path that already holds an Iceberg
    # table is otherwise rejected with TABLE_ALREADY_EXISTS. The declared columns are kept until
    # the first query refreshes them from the Iceberg schema.
    create_iceberg_table(
        storage_type,
        instance,
        table_name,
        started_cluster_iceberg_with_spark,
        "(u UInt32)",
        if_not_exists=True,
    )

    metadata_dir = f"/var/lib/clickhouse/user_files/iceberg_data/default/{table_name}/metadata"

    def list_data_manifests():
        return set(
            instance.exec_in_container(
                [
                    "bash",
                    "-c",
                    f"find '{metadata_dir}' -maxdepth 1 -name '*.avro' -not -name 'snap-*.avro' -type f",
                ]
            )
            .strip()
            .splitlines()
        )

    manifests_before = list_data_manifests()

    instance.query(f"INSERT INTO {table_name} VALUES (7)", settings={"allow_insert_into_iceberg": 1})

    new_manifests = sorted(list_data_manifests() - manifests_before)
    assert new_manifests, "INSERT did not write a manifest"

    # Field id 1 is `u`. Bounds are an array of (field id, raw bytes) pairs.
    bound_lengths = set()
    for manifest in new_manifests:
        for bounds in ("lower_bounds", "upper_bounds"):
            result = instance.query(
                f"""
                SELECT length(tupleElement(bound, 2))
                FROM
                (
                    SELECT arrayJoin(tupleElement(data_file, '{bounds}')) AS bound
                    FROM file('{manifest}', Avro)
                )
                WHERE tupleElement(bound, 1) = 1
                """
            ).split()
            bound_lengths.update(int(length) for length in result)

    assert bound_lengths == {4}, f"Iceberg `int` bounds must be 4 bytes, got lengths {bound_lengths}"

    assert instance.query(f"SELECT u FROM {table_name} ORDER BY u") == "5\n7\n"
