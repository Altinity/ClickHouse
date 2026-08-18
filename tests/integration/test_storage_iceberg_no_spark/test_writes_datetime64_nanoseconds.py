#!/usr/bin/env python3
"""ClickHouse Iceberg writes of DateTime64(9) must use timestamp_ns, not timestamp.

Mapping DateTime64(9) to Iceberg `timestamp` while dumping unscaled nanosecond
min/max ticks recreates the customer over-prune (schema microseconds, stats
nanoseconds).
"""

import pytest

from helpers.iceberg_timestamp_ns import (
    FILTER_LITERAL_IDS,
    FILTER_LITERALS,
    PRUNE_OFF,
    PRUNE_ON,
    ROWS,
    check_ns_pruning,
)
from helpers.iceberg_utils import (
    check_validity_and_get_prunned_files_general,
    create_iceberg_table,
    get_creation_expression,
    get_uuid_str,
)


def _insert_ns_rows(instance, table_name):
    for ts_ns, row_id, _utc in ROWS:
        instance.query(
            f"INSERT INTO {table_name} VALUES "
            f"(fromUnixTimestamp64Nano({ts_ns}), fromUnixTimestamp64Nano({ts_ns}), {row_id})"
        )


def _assert_written_as_datetime64_9(instance, table_name, timezone=None):
    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    assert "DateTime64(9" in describe, describe
    assert "DateTime64(6" not in describe, describe
    if timezone == "UTC":
        assert "UTC" in describe, describe


@pytest.mark.parametrize("make_literal", FILTER_LITERALS, ids=FILTER_LITERAL_IDS)
def test_writes_minmax_pruning_datetime64_nanoseconds(
    started_cluster_iceberg_no_spark, make_literal
):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_minmax_ns_" + get_uuid_str()
    create_iceberg_table(
        "s3",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(ts DateTime64(9), value DateTime64(9), id Int32)",
        format_version=3,
    )
    _assert_written_as_datetime64_9(instance, table_name)
    _insert_ns_rows(instance, table_name)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            instance, table_name, PRUNE_OFF, PRUNE_ON, "IcebergMinMaxIndexPrunedFiles", select_expression
        )

    check_ns_pruning(instance, table_name, "value", pruned_files, make_literal)


@pytest.mark.parametrize("make_literal", FILTER_LITERALS, ids=FILTER_LITERAL_IDS)
def test_writes_partition_pruning_datetime64_nanoseconds(
    started_cluster_iceberg_no_spark, make_literal
):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_part_ns_" + get_uuid_str()
    create_iceberg_table(
        "s3",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(ts DateTime64(9), value DateTime64(9), id Int32)",
        format_version=3,
        partition_by="ts",
    )
    _assert_written_as_datetime64_9(instance, table_name)
    _insert_ns_rows(instance, table_name)

    def pruned_files(select_expression):
        return check_validity_and_get_prunned_files_general(
            instance, table_name, PRUNE_OFF, PRUNE_ON, "IcebergPartitionPrunedFiles", select_expression
        )

    check_ns_pruning(instance, table_name, "ts", pruned_files, make_literal)


def test_writes_datetime64_nanoseconds_with_timezone(started_cluster_iceberg_no_spark):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_ns_tz_" + get_uuid_str()
    create_iceberg_table(
        "s3",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(ts DateTime64(9, 'UTC'), id Int32)",
        format_version=3,
    )
    _assert_written_as_datetime64_9(instance, table_name, timezone="UTC")
    instance.query(
        f"INSERT INTO {table_name} VALUES (fromUnixTimestamp64Nano({ROWS[0][0]}), 1)"
    )
    assert instance.query(f"SELECT id FROM {table_name}") == "1\n"


def _assert_timestamp_ns_requires_v3(error):
    assert "BAD_ARGUMENTS" in error, error
    assert "LOGICAL_ERROR" not in error, error
    assert "format version 3" in error, error


@pytest.mark.parametrize("format_version", [1, 2])
@pytest.mark.parametrize(
    "schema",
    [
        "(ts DateTime64(9), id Int32)",
        "(ts DateTime64(9, 'UTC'), id Int32)",
        "(ts Array(DateTime64(9)), id Int32)",
    ],
    ids=["timestamp_ns", "timestamptz_ns", "nested"],
)
def test_writes_datetime64_nanoseconds_rejected_on_v1_v2(
    started_cluster_iceberg_no_spark, format_version, schema
):
    """DateTime64(9) is Iceberg v3 timestamp_ns; v1/v2 (default is 2) must not emit it."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_ns_v2_reject_" + get_uuid_str()
    error = instance.query_and_get_error(
        get_creation_expression(
            "s3",
            table_name,
            started_cluster_iceberg_no_spark,
            schema,
            format_version,
        )
    )
    _assert_timestamp_ns_requires_v3(error)


def test_writes_datetime64_nanoseconds_rejected_on_default_format_version(
    started_cluster_iceberg_no_spark,
):
    """CREATE without iceberg_format_version uses default 2 and must reject DateTime64(9)."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_ns_default_ver_" + get_uuid_str()
    bucket = started_cluster_iceberg_no_spark.minio_bucket
    error = instance.query_and_get_error(
        f"""
        DROP TABLE IF EXISTS {table_name};
        CREATE TABLE {table_name} (ts DateTime64(9), id Int32)
        ENGINE = IcebergS3(s3, filename = 'var/lib/clickhouse/user_files/iceberg_data/default/{table_name}/', format=Parquet, url = 'http://minio1:9001/{bucket}/');
        """
    )
    _assert_timestamp_ns_requires_v3(error)


def test_writes_datetime64_nanoseconds_alter_rejected_on_v2(started_cluster_iceberg_no_spark):
    """ALTER ADD / MODIFY must not write timestamp_ns into existing v2 metadata."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_ns_alter_v2_" + get_uuid_str()
    create_iceberg_table(
        "s3",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(ts DateTime64(6), id Int32)",
        format_version=2,
    )
    alter_settings = {"allow_insert_into_iceberg": 1}

    add_error = instance.query_and_get_error(
        f"ALTER TABLE {table_name} ADD COLUMN ts_ns Nullable(DateTime64(9))",
        settings=alter_settings,
    )
    _assert_timestamp_ns_requires_v3(add_error)

    modify_error = instance.query_and_get_error(
        f"ALTER TABLE {table_name} MODIFY COLUMN ts DateTime64(9)",
        settings=alter_settings,
    )
    _assert_timestamp_ns_requires_v3(modify_error)

    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    assert "DateTime64(6)" in describe, describe
    assert "DateTime64(9" not in describe, describe


def test_writes_datetime64_nanoseconds_alter_allowed_on_v3(started_cluster_iceberg_no_spark):
    """ALTER ADD Nullable(DateTime64(9)) is valid on Iceberg v3."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_writes_ns_alter_v3_" + get_uuid_str()
    create_iceberg_table(
        "s3",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int32)",
        format_version=3,
    )
    instance.query(
        f"ALTER TABLE {table_name} ADD COLUMN ts_ns Nullable(DateTime64(9))",
        settings={"allow_insert_into_iceberg": 1},
    )
    describe = instance.query(f"DESCRIBE TABLE {table_name}")
    assert "DateTime64(9" in describe, describe
    instance.query(
        f"INSERT INTO {table_name} VALUES (1, fromUnixTimestamp64Nano({ROWS[0][0]}))"
    )
    assert instance.query(f"SELECT id FROM {table_name}") == "1\n"
