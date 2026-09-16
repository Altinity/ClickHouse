#!/usr/bin/env python3
"""Iceberg identity-partition and min/max pruning for timestamp_ns (DateTime64(9)).

pyiceberg 0.11 cannot write Iceberg v3 (`timestamp_ns`), and the REST catalog
image used here also rejects that type. The table is therefore written as
Iceberg v2 with `long` columns holding epoch nanoseconds, then patched to
`timestamp_ns` and Parquet `timestamp[ns]` before ClickHouse reads it.
"""

import shutil
import tempfile
import uuid
from pathlib import Path

import pyarrow as pa
import pytest
from pyiceberg.catalog.sql import SqlCatalog
from pyiceberg.partitioning import PartitionField, PartitionSpec
from pyiceberg.schema import NestedField, Schema
from pyiceberg.transforms import IdentityTransform
from pyiceberg.types import IntegerType, LongType, TimestampType

from helpers.iceberg_timestamp_ns import (
    DATETIME64_MAX_US,
    FILTER_LITERAL_IDS,
    FILTER_LITERALS,
    PRUNE_OFF,
    PRUNE_ON,
    ROWS,
    SPARK_TIMESTAMP_SENTINEL_US,
    check_minmax_far_future_us_upper_bound,
    check_minmax_ns_bounds_on_timestamp_schema,
    check_ns_pruning,
    check_ns_pruning_datetime64_microseconds,
    patch_iceberg_table_long_to_timestamp_ns,
    patch_iceberg_timestamp_bounds_us_to_ns,
    patch_iceberg_timestamp_upper_bounds,
)
from helpers.iceberg_utils import check_validity_and_get_prunned_files_general


def _ns_schema():
    return Schema(
        NestedField(field_id=1, name="ts", field_type=LongType(), required=False),
        NestedField(field_id=2, name="value", field_type=LongType(), required=False),
        NestedField(field_id=3, name="id", field_type=IntegerType(), required=False),
    )


def _arrow_row(ts_ns, row_id):
    return pa.Table.from_pydict(
        {
            "ts": pa.array([ts_ns], type=pa.int64()),
            "value": pa.array([ts_ns], type=pa.int64()),
            "id": pa.array([row_id], type=pa.int32()),
        }
    )


def _timestamp_schema():
    return Schema(
        NestedField(field_id=1, name="ts", field_type=TimestampType(), required=False),
        NestedField(field_id=2, name="value", field_type=TimestampType(), required=False),
        NestedField(field_id=3, name="id", field_type=IntegerType(), required=False),
    )


def _arrow_timestamp_row(ts_ns, row_id):
    ts_us = pa.array([ts_ns // 1000], type=pa.timestamp("us"))
    return pa.Table.from_pydict(
        {
            "ts": ts_us,
            "value": ts_us,
            "id": pa.array([row_id], type=pa.int32()),
        }
    )


def _create_timestamp_table(started_cluster, table_name, patch):
    """Iceberg v2 `timestamp` (microseconds parquet) with a custom manifest-bound patch."""
    work = Path(tempfile.mkdtemp(prefix="iceberg_ns_bounds_"))
    try:
        catalog = SqlCatalog(
            "ns_bounds",
            **{
                "uri": f"sqlite:///{work}/catalog.db",
                "warehouse": f"file://{work}/warehouse",
            },
        )
        catalog.create_namespace("default")
        identifier = f"default.{table_name}"
        table = catalog.create_table(
            identifier=identifier,
            schema=_timestamp_schema(),
            partition_spec=PartitionSpec(),
        )
        table.append(
            pa.concat_tables(
                [_arrow_timestamp_row(ts_ns, row_id) for ts_ns, row_id, _utc in ROWS]
            )
        )
        table = catalog.load_table(identifier)

        old_prefix = table.location()
        table_location = Path(old_prefix.removeprefix("file://"))
        s3_key = f"iceberg_ns_bounds_{uuid.uuid4().hex}"
        new_prefix = f"s3://{started_cluster.minio_bucket}/{s3_key}"

        patch(table_location, old_prefix, new_prefix)

        uploader = started_cluster.default_s3_uploader
        for path in table_location.rglob("*"):
            if path.is_file():
                rel = path.relative_to(table_location).as_posix()
                uploader.upload_file(str(path), f"{s3_key}/{rel}")

        return f"{s3_key}/"
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _create_timestamp_table_with_ns_bounds(started_cluster, table_name):
    """Iceberg v2 `timestamp` (microseconds) whose manifest min/max bytes are nanoseconds."""
    return _create_timestamp_table(
        started_cluster, table_name, patch_iceberg_timestamp_bounds_us_to_ns
    )


def _create_timestamp_table_with_upper_bound_us(started_cluster, table_name, micros):
    """Iceberg v2 `timestamp` whose upper bounds are spec-correct far-future microseconds."""
    def patch(table_location, old_prefix, new_prefix):
        patch_iceberg_timestamp_upper_bounds(
            table_location, micros, old_prefix, new_prefix
        )

    return _create_timestamp_table(started_cluster, table_name, patch)


def _create_v3_table(
    started_cluster, table_name, partition_spec, iceberg_type="timestamp_ns", parquet_unit="ns"
):
    work = Path(tempfile.mkdtemp(prefix="iceberg_ns_pruning_"))
    try:
        catalog = SqlCatalog(
            "ns_pruning",
            **{
                "uri": f"sqlite:///{work}/catalog.db",
                "warehouse": f"file://{work}/warehouse",
            },
        )
        catalog.create_namespace("default")
        identifier = f"default.{table_name}"
        table = catalog.create_table(
            identifier=identifier,
            schema=_ns_schema(),
            partition_spec=partition_spec,
        )
        for ts_ns, row_id, _utc in ROWS:
            table.append(_arrow_row(ts_ns, row_id))
            table = catalog.load_table(identifier)

        old_prefix = table.location()
        table_location = Path(old_prefix.removeprefix("file://"))
        s3_key = f"iceberg_ns_pruning_{uuid.uuid4().hex}"
        new_prefix = f"s3://{started_cluster.minio_bucket}/{s3_key}"

        patch_iceberg_table_long_to_timestamp_ns(
            table_location,
            old_prefix,
            new_prefix,
            iceberg_type=iceberg_type,
            parquet_unit=parquet_unit,
        )

        uploader = started_cluster.default_s3_uploader
        for path in table_location.rglob("*"):
            if path.is_file():
                rel = path.relative_to(table_location).as_posix()
                uploader.upload_file(str(path), f"{s3_key}/{rel}")

        return f"{s3_key}/"
    finally:
        shutil.rmtree(work, ignore_errors=True)


def _iceberg_table(s3_filename):
    return (
        f"icebergS3(s3, filename = '{s3_filename}', format=Parquet, "
        f"url = 'http://minio1:9001/root/')"
    )


@pytest.mark.parametrize("make_literal", FILTER_LITERALS, ids=FILTER_LITERAL_IDS)
def test_partition_pruning_datetime64_nanoseconds(started_cluster_iceberg_no_spark, make_literal):
    """Identity partition pruning on Iceberg timestamp_ns must not drop matching rows."""
    node = started_cluster_iceberg_no_spark.instances["node1"]
    partition_spec = PartitionSpec(
        PartitionField(source_id=1, field_id=1000, transform=IdentityTransform(), name="ts")
    )
    s3_filename = _create_v3_table(started_cluster_iceberg_no_spark, "part_ns", partition_spec)
    table = _iceberg_table(s3_filename)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, "part_ns", PRUNE_OFF, PRUNE_ON, "IcebergPartitionPrunedFiles", select_expression
        )

    check_ns_pruning(node, table, "ts", pruned_files, make_literal)


@pytest.mark.parametrize("make_literal", FILTER_LITERALS, ids=FILTER_LITERAL_IDS)
def test_minmax_pruning_datetime64_nanoseconds(started_cluster_iceberg_no_spark, make_literal):
    """Min/max file pruning on Iceberg timestamp_ns must not drop matching rows."""
    node = started_cluster_iceberg_no_spark.instances["node1"]
    s3_filename = _create_v3_table(started_cluster_iceberg_no_spark, "minmax_ns", PartitionSpec())
    table = _iceberg_table(s3_filename)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, "minmax_ns", PRUNE_OFF, PRUNE_ON, "IcebergMinMaxIndexPrunedFiles", select_expression
        )

    check_ns_pruning(node, table, "value", pruned_files, make_literal)


@pytest.mark.parametrize("timezone", [None, "UTC"], ids=["toDateTime64_6", "toDateTime64_6_UTC"])
def test_minmax_pruning_datetime64_nanoseconds_microsecond_predicate(
    started_cluster_iceberg_no_spark, timezone
):
    """DateTime64(6) predicate vs timestamp_ns min/max (customer over-prune)."""
    node = started_cluster_iceberg_no_spark.instances["node1"]
    s3_filename = _create_v3_table(started_cluster_iceberg_no_spark, "minmax_ns_us", PartitionSpec())
    table = _iceberg_table(s3_filename)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node, "minmax_ns_us", PRUNE_OFF, PRUNE_ON, "IcebergMinMaxIndexPrunedFiles", select_expression
        )

    check_ns_pruning_datetime64_microseconds(node, table, "value", pruned_files, timezone=timezone)


def test_minmax_pruning_ns_bounds_on_timestamp_schema(started_cluster_iceberg_no_spark):
    """Iceberg `timestamp` (`DateTime64(6)`) with nanosecond min/max bytes must not over-prune.

    Customer: `WHERE time_unix_nano <= toDateTime64('2026-08-11 20:02:07.897356', 6)`
    returns 0 with min/max pruning and the correct row count without it.
    """
    node = started_cluster_iceberg_no_spark.instances["node1"]
    s3_filename = _create_timestamp_table_with_ns_bounds(
        started_cluster_iceberg_no_spark, "minmax_ns_on_ts"
    )
    table = _iceberg_table(s3_filename)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node,
            "minmax_ns_on_ts",
            PRUNE_OFF,
            PRUNE_ON,
            "IcebergMinMaxIndexPrunedFiles",
            select_expression,
        )

    check_minmax_ns_bounds_on_timestamp_schema(node, table, "value", pruned_files)


@pytest.mark.parametrize(
    "upper_bound_us",
    [SPARK_TIMESTAMP_SENTINEL_US, DATETIME64_MAX_US],
    ids=["spark_9999_12_31", "datetime64_2299_12_31"],
)
def test_minmax_pruning_far_future_us_upper_bound_on_timestamp_schema(
    started_cluster_iceberg_no_spark, upper_bound_us
):
    """Spec-correct far-future Iceberg `timestamp` upper bounds must not be treated as ns."""
    node = started_cluster_iceberg_no_spark.instances["node1"]
    s3_filename = _create_timestamp_table_with_upper_bound_us(
        started_cluster_iceberg_no_spark, "minmax_far_future_us", upper_bound_us
    )
    table = _iceberg_table(s3_filename)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            node,
            "minmax_far_future_us",
            PRUNE_OFF,
            PRUNE_ON,
            "IcebergMinMaxIndexPrunedFiles",
            select_expression,
        )

    check_minmax_far_future_us_upper_bound(node, table, "value", pruned_files)
