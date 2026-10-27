#!/usr/bin/env python3
"""Spark-written Iceberg identity-partition and min/max pruning for timestamp_ns.

Spark 3.5 + Iceberg 1.8.1 (the integration-test runtime) cannot create or write
Iceberg `timestamp_ns`: Spark TIMESTAMP is microsecond-only, and Iceberg Spark
maps it to `timestamp` / `timestamptz`. This test therefore writes epoch
nanoseconds as BIGINT via Spark, patches the table to `timestamp_ns`, then
checks ClickHouse pruning.
"""

from pathlib import Path

import pytest

from helpers.iceberg_timestamp_ns import (
    FILTER_LITERAL_IDS,
    FILTER_LITERALS,
    PRUNE_OFF,
    PRUNE_ON,
    ROWS,
    check_minmax_ns_bounds_on_timestamp_schema,
    check_ns_pruning,
    check_ns_pruning_datetime64_microseconds,
    patch_iceberg_table_long_to_timestamp_ns,
    patch_iceberg_timestamp_bounds_us_to_ns,
)
from helpers.iceberg_utils import (
    check_validity_and_get_prunned_files_general,
    default_upload_directory,
    get_creation_expression,
    get_uuid_str,
)

WAREHOUSE_PREFIX = "/var/lib/clickhouse/user_files/iceberg_data/default"


def _create_spark_ns_table(
    spark,
    started_cluster,
    table_name,
    partitioned,
    iceberg_type="timestamp_ns",
    parquet_unit="ns",
):
    partition_clause = "PARTITIONED BY (identity(ts))" if partitioned else ""
    spark.sql(
        f"""
        CREATE TABLE {table_name} (
            ts BIGINT,
            value BIGINT,
            id INT
        )
        USING iceberg
        {partition_clause}
        OPTIONS('format-version'='2')
        """
    )
    for ts_ns, row_id, _utc in ROWS:
        spark.sql(
            f"INSERT INTO {table_name} VALUES ({ts_ns}, {ts_ns}, {row_id})"
        )

    table_location = Path(f"{WAREHOUSE_PREFIX}/{table_name}")
    patch_iceberg_table_long_to_timestamp_ns(
        table_location, iceberg_type=iceberg_type, parquet_unit=parquet_unit
    )
    default_upload_directory(
        started_cluster,
        "s3",
        f"/iceberg_data/default/{table_name}/",
        f"/iceberg_data/default/{table_name}/",
    )


def _create_spark_timestamp_table_with_ns_bounds(spark, started_cluster, table_name):
    """Spark Iceberg `TIMESTAMP` (microseconds) whose manifest min/max bytes are nanoseconds."""
    spark.sql(
        f"""
        CREATE TABLE {table_name} (
            ts TIMESTAMP,
            value TIMESTAMP,
            id INT
        )
        USING iceberg
        OPTIONS('format-version'='2')
        """
    )
    values = ", ".join(
        f"(TIMESTAMP '{utc.split('.')[0]}.{utc.split('.')[1][:6]}', "
        f"TIMESTAMP '{utc.split('.')[0]}.{utc.split('.')[1][:6]}', {row_id})"
        for _ts_ns, row_id, utc in ROWS
    )
    spark.sql(f"INSERT INTO {table_name} VALUES {values}")

    table_location = Path(f"{WAREHOUSE_PREFIX}/{table_name}")
    patch_iceberg_timestamp_bounds_us_to_ns(table_location)
    default_upload_directory(
        started_cluster,
        "s3",
        f"/iceberg_data/default/{table_name}/",
        f"/iceberg_data/default/{table_name}/",
    )


@pytest.mark.parametrize("make_literal", FILTER_LITERALS, ids=FILTER_LITERAL_IDS)
def test_spark_partition_pruning_datetime64_nanoseconds(started_cluster_iceberg_with_spark, make_literal):
    """Identity partition pruning on Spark-written Iceberg timestamp_ns must not drop matching rows."""
    node = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    table_name = "test_spark_part_ns_" + get_uuid_str()
    _create_spark_ns_table(spark, started_cluster_iceberg_with_spark, table_name, partitioned=True)
    table = get_creation_expression(
        "s3", table_name, started_cluster_iceberg_with_spark, table_function=True
    )

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, table_name, PRUNE_OFF, PRUNE_ON, "IcebergPartitionPrunedFiles", select_expression
        )

    check_ns_pruning(node, table, "ts", pruned_files, make_literal)


@pytest.mark.parametrize("make_literal", FILTER_LITERALS, ids=FILTER_LITERAL_IDS)
def test_spark_minmax_pruning_datetime64_nanoseconds(started_cluster_iceberg_with_spark, make_literal):
    """Min/max file pruning on Spark-written Iceberg timestamp_ns must not drop matching rows."""
    node = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    table_name = "test_spark_minmax_ns_" + get_uuid_str()
    _create_spark_ns_table(spark, started_cluster_iceberg_with_spark, table_name, partitioned=False)
    table = get_creation_expression(
        "s3", table_name, started_cluster_iceberg_with_spark, table_function=True
    )

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, table_name, PRUNE_OFF, PRUNE_ON, "IcebergMinMaxIndexPrunedFiles", select_expression
        )

    check_ns_pruning(node, table, "value", pruned_files, make_literal)


@pytest.mark.parametrize("timezone", [None, "UTC"], ids=["toDateTime64_6", "toDateTime64_6_UTC"])
def test_spark_minmax_pruning_datetime64_nanoseconds_microsecond_predicate(
    started_cluster_iceberg_with_spark, timezone
):
    """DateTime64(6) predicate vs Spark-written timestamp_ns min/max (customer over-prune)."""
    node = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    table_name = "test_spark_minmax_ns_us_" + get_uuid_str()
    _create_spark_ns_table(spark, started_cluster_iceberg_with_spark, table_name, partitioned=False)
    table = get_creation_expression(
        "s3", table_name, started_cluster_iceberg_with_spark, table_function=True
    )

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, table_name, PRUNE_OFF, PRUNE_ON, "IcebergMinMaxIndexPrunedFiles", select_expression
        )

    check_ns_pruning_datetime64_microseconds(node, table, "value", pruned_files, timezone=timezone)


def test_spark_minmax_pruning_ns_bounds_on_timestamp_schema(started_cluster_iceberg_with_spark):
    """Nanosecond parquet/bounds with Iceberg `timestamp` (DateTime64(6)) must not over-prune."""
    node = started_cluster_iceberg_with_spark.instances["node1"]
    spark = started_cluster_iceberg_with_spark.spark_session
    table_name = "test_spark_minmax_ns_on_ts_" + get_uuid_str()
    _create_spark_timestamp_table_with_ns_bounds(
        spark, started_cluster_iceberg_with_spark, table_name
    )
    table = get_creation_expression(
        "s3", table_name, started_cluster_iceberg_with_spark, table_function=True
    )

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, table_name, PRUNE_OFF, PRUNE_ON, "IcebergMinMaxIndexPrunedFiles", select_expression
        )

    check_minmax_ns_bounds_on_timestamp_schema(node, table, "value", pruned_files)
