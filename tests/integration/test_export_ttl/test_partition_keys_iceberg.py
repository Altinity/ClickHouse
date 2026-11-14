import pytest

from helpers.export_partition_helpers import make_iceberg_s3, unique_suffix

from .common import (
    assert_exactly_once,
    assert_iceberg_files_partitioned,
    create_source,
    create_source_error,
    iceberg_ids,
    ttl_rows,
    ttl_tasks,
    wait_for_last_error,
    wait_for_partitions_exported,
)

CLUSTER_INSTANCES = ["replica1"]

# The partition spec of an Iceberg destination against the partition key of the source. The spec
# is turned into a ClickHouse partition key, which is checked like the key of an object storage
# destination: structurally, per group from the min/max values of its parts, or refused at `CREATE`
# and `ALTER` when it can never hold. Where each file lands is checked against the partition values
# recorded in the Iceberg manifests.

COLUMNS = "id Int64, t DateTime"


def make_tables(node, engine, columns, source_partition_by, spec, settings=None, ttl_column="t"):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"ice_mt_{suffix}", f"ice_{suffix}"
    make_iceberg_s3(node, iceberg_table, columns, partition_by=spec)
    create_source(
        node, mt_table, columns, source_partition_by, f"{ttl_column} + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}",
        engine=engine, settings=settings,
    )
    return mt_table, iceberg_table


def source_partitions(node, table):
    return node.query(
        f"SELECT DISTINCT partition_id FROM system.parts WHERE database = currentDatabase() AND table = '{table}' AND active"
    ).split()


REFUSED_SPECS = [
    pytest.param("icebergBucket(8, id)", id="bucket_of_a_column_outside_the_key"),
    pytest.param("icebergBucket(8, t)", id="bucket_of_a_key_column"),
    pytest.param("id", id="identity_of_a_column_outside_the_key"),
]


@pytest.mark.parametrize("spec", REFUSED_SPECS)
def test_spec_that_can_never_hold_is_refused(cluster, source_engine, spec):
    node = cluster.instances["replica1"]
    suffix = unique_suffix()
    mt_table, iceberg_table = f"ice_mt_{suffix}", f"ice_{suffix}"
    make_iceberg_s3(node, iceberg_table, COLUMNS, partition_by=spec)

    error = create_source_error(node, mt_table, COLUMNS, "toYYYYMM(t)", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}", engine=source_engine)
    assert "BAD_ARGUMENTS" in error and "partition key of the destination" in error, error

    create_source(node, mt_table, COLUMNS, "toYYYYMM(t)", "t + INTERVAL 30 DAY DELETE", engine=source_engine)
    error = node.query_and_get_error(f"ALTER TABLE {mt_table} MODIFY TTL t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}")
    assert "BAD_ARGUMENTS" in error, error


ACCEPTED_SPECS = [
    pytest.param("toYYYYMM(t)", "toYearNumSinceEpoch(t)", id="year"),
    pytest.param("toYYYYMM(t)", "toMonthNumSinceEpoch(t)", id="month"),
    pytest.param("toYYYYMM(t)", "toRelativeDayNum(t)", id="day"),
    pytest.param("toYYYYMM(t)", "", id="unpartitioned"),
    pytest.param("id", "id", id="identity"),
]


@pytest.mark.parametrize("source_partition_by, spec", ACCEPTED_SPECS)
def test_spec_that_can_hold_is_accepted(cluster, source_engine, source_partition_by, spec):
    node = cluster.instances["replica1"]
    mt_table, _ = make_tables(node, source_engine, COLUMNS, source_partition_by, spec)
    assert "EXPORT TO TABLE" in node.query(f"SELECT create_table_query FROM system.tables WHERE name = '{mt_table}'")


def test_month_transform(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, COLUMNS, "toYYYYMM(t)", "toMonthNumSinceEpoch(t)")
    node.query(
        f"INSERT INTO {mt_table} VALUES (1, '2020-01-05 10:00:00'), (2, '2020-01-20 10:00:00'),"
        f" (3, '2020-02-05 10:00:00'), (4, '2021-03-05 10:00:00')"
    )
    wait_for_partitions_exported(node, mt_table, source_partitions(node, mt_table))

    values = assert_iceberg_files_partitioned(node, iceberg_table, "t", "toMonthNumSinceEpoch(t)")
    expected = {int(x) for x in node.query(
        "SELECT toMonthNumSinceEpoch(toDateTime(x)) FROM values('x String', '2020-01-05 10:00:00', '2020-02-05 10:00:00', '2021-03-05 10:00:00')"
    ).split()}
    assert values == expected
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3, 4])


def test_day_transform_is_checked_per_group(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, COLUMNS, "toYYYYMM(t)", "toRelativeDayNum(t)")
    node.query(f"INSERT INTO {mt_table} VALUES (1, '2020-01-05 01:00:00'), (2, '2020-01-05 23:00:00')")
    node.query(f"INSERT INTO {mt_table} VALUES (3, '2020-02-05 10:00:00'), (4, '2020-02-06 10:00:00')")

    wait_for_partitions_exported(node, mt_table, ["202001"])
    wait_for_last_error(node, mt_table, "202002", "multiple destination partitions")
    assert assert_iceberg_files_partitioned(node, iceberg_table, "t", "toRelativeDayNum(t)") == {
        int(node.query("SELECT toRelativeDayNum(toDateTime('2020-01-05 10:00:00'))").strip())
    }
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert all(task["partition_id"] == "202001" for task in ttl_tasks(node, mt_table))


def test_bucket_transform_matching_the_source_key(cluster, source_engine):
    node = cluster.instances["replica1"]
    columns = "id Int64, user_id Int64, t DateTime"
    mt_table, iceberg_table = make_tables(node, source_engine, columns, "icebergBucket(8, user_id)", "icebergBucket(8, user_id)")
    node.query(f"INSERT INTO {mt_table} SELECT number, number * 7, now() - INTERVAL 10 DAY FROM numbers(12)")
    wait_for_partitions_exported(node, mt_table, source_partitions(node, mt_table))

    values = assert_iceberg_files_partitioned(node, iceberg_table, "user_id", "icebergBucket(8, user_id)")
    assert values == {int(x) for x in node.query("SELECT DISTINCT icebergBucket(8, number * 7) FROM numbers(12)").split()}
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(12))


def test_truncate_transform(cluster, source_engine):
    """A source partitioned by tens into a destination truncated to tens always holds; a source
    partitioned by hundreds holds only for groups within one ten."""
    node = cluster.instances["replica1"]
    columns = "id Int64, k Int64, t DateTime"
    mt_table, iceberg_table = make_tables(node, source_engine, columns, "intDiv(k, 10)", "icebergTruncate(10, k)")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 11, now() - INTERVAL 10 DAY), (2, 15, now() - INTERVAL 10 DAY), (3, 23, now() - INTERVAL 10 DAY)")
    wait_for_partitions_exported(node, mt_table, ["1", "2"])
    assert assert_iceberg_files_partitioned(node, iceberg_table, "k", "icebergTruncate(10, k)") == {10, 20}
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3])

    mt_table, iceberg_table = make_tables(node, source_engine, columns, "intDiv(k, 100)", "icebergTruncate(10, k)")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 105, now() - INTERVAL 10 DAY), (2, 107, now() - INTERVAL 10 DAY)")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 210, now() - INTERVAL 10 DAY), (4, 250, now() - INTERVAL 10 DAY)")
    wait_for_partitions_exported(node, mt_table, ["1"])
    wait_for_last_error(node, mt_table, "2", "multiple destination partitions")
    assert assert_iceberg_files_partitioned(node, iceberg_table, "k", "icebergTruncate(10, k)") == {100}
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])


def test_spec_of_several_fields(cluster, source_engine):
    node = cluster.instances["replica1"]
    columns = "id Int64, year Int32, t DateTime"
    mt_table, iceberg_table = make_tables(node, source_engine, columns, "(year, toYYYYMM(t))", "(year, toMonthNumSinceEpoch(t))")
    node.query(
        f"INSERT INTO {mt_table} VALUES (1, 1, '2020-01-05 10:00:00'), (2, 2, '2020-01-05 10:00:00'), (3, 1, '2020-02-05 10:00:00')"
    )
    wait_for_partitions_exported(node, mt_table, source_partitions(node, mt_table))

    assert assert_iceberg_files_partitioned(node, iceberg_table, "year", "year") == {1, 2}
    assert len(assert_iceberg_files_partitioned(node, iceberg_table, "t", "toMonthNumSinceEpoch(t)")) == 2
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3])


@pytest.mark.parametrize("profile, exported", [pytest.param("", True, id="utc"), pytest.param("ttl_export_tokyo", False, id="tokyo")])
def test_day_transform_uses_the_partition_time_zone(cluster, profile, exported):
    """14:00 and 16:00 UTC are on one day in UTC, and on two days in Tokyo. The check of a group uses
    the time zone the destination partitions are computed in."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, "MergeTree", COLUMNS, "toYYYYMM(t)", "toRelativeDayNum(t)",
        settings={"ttl_export_settings_profile": profile} if profile else None,
    )
    node.query(
        f"INSERT INTO {mt_table} VALUES (1, toDateTime('2020-01-01 14:00:00', 'UTC')), (2, toDateTime('2020-01-01 16:00:00', 'UTC'))"
    )
    if exported:
        wait_for_partitions_exported(node, mt_table, ["202001"])
        assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    else:
        wait_for_last_error(node, mt_table, "202001", "multiple destination partitions")
        assert ttl_tasks(node, mt_table) == []
        assert ttl_rows(node, mt_table)["202001"]["eligible_parts"] == 1
