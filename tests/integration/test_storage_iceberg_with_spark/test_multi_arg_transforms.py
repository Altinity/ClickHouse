import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    default_upload_directory,
    get_creation_expression,
    get_uuid_str,
)


@pytest.mark.parametrize("storage_type", ["s3", "local"])
def test_multi_arg_bucket_partition(
    started_cluster_iceberg_with_spark, storage_type
):
    """Iceberg v3 multi-argument partition transform: bucket over two columns.

    Verifies that ClickHouse can read a table whose partition spec uses
    ``source-ids`` (multiple source columns) instead of a single ``source-id``.
    Per the Iceberg v3 spec, readers must handle unknown transforms by ignoring
    the unsupported partition fields when filtering.
    """
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    TABLE_NAME = "test_multi_arg_bucket_" + storage_type + "_" + get_uuid_str()

    spark.sql(
        f"""
        CREATE TABLE {TABLE_NAME} (
            id INT,
            region STRING,
            value DOUBLE
        )
        USING iceberg
        PARTITIONED BY (bucket(16, id, region))
        TBLPROPERTIES ('format-version' = '3')
    """
    )

    spark.sql(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (1, 'us-east', 10.0),
        (2, 'eu-west', 20.0),
        (3, 'us-east', 30.0),
        (4, 'ap-south', 40.0)
    """
    )

    default_upload_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    create_iceberg_table(
        storage_type,
        instance,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
    )

    result = instance.query(
        f"SELECT id, region, value FROM {TABLE_NAME} ORDER BY id"
    )
    expected = "1\tus-east\t10\n2\teu-west\t20\n3\tus-east\t30\n4\tap-south\t40\n"
    assert result == expected

    file_count = int(
        instance.query(
            f"SELECT count() FROM system.iceberg_files "
            f"WHERE database = currentDatabase() AND table = '{TABLE_NAME}'"
        ).strip()
    )
    assert file_count > 0


@pytest.mark.parametrize("storage_type", ["s3", "local"])
def test_multi_arg_bucket_partition_via_table_function(
    started_cluster_iceberg_with_spark, storage_type
):
    """Same as above but reads through the table function instead of an engine table."""
    instance = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    TABLE_NAME = (
        "test_multi_arg_bucket_tf_" + storage_type + "_" + get_uuid_str()
    )

    spark.sql(
        f"""
        CREATE TABLE {TABLE_NAME} (
            id INT,
            region STRING,
            value DOUBLE
        )
        USING iceberg
        PARTITIONED BY (bucket(16, id, region))
        TBLPROPERTIES ('format-version' = '3')
    """
    )

    spark.sql(
        f"""
        INSERT INTO {TABLE_NAME} VALUES
        (10, 'us-west', 100.0),
        (20, 'eu-central', 200.0),
        (30, 'ap-east', 300.0)
    """
    )

    default_upload_directory(
        started_cluster_iceberg_with_spark,
        storage_type,
        f"/iceberg_data/default/{TABLE_NAME}/",
        f"/iceberg_data/default/{TABLE_NAME}/",
    )

    expression = get_creation_expression(
        storage_type,
        TABLE_NAME,
        started_cluster_iceberg_with_spark,
        table_function=True,
    )

    result = instance.query(
        f"SELECT id, region, value FROM {expression} ORDER BY id"
    )
    expected = "10\tus-west\t100\n20\teu-central\t200\n30\tap-east\t300\n"
    assert result == expected

    row_count = int(
        instance.query(f"SELECT count() FROM {expression}").strip()
    )
    assert row_count == 3
