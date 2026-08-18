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
