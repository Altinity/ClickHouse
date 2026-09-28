import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    active_parts,
    assert_exactly_once,
    assert_iceberg_files_partitioned,
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    create_iceberg,
    create_source,
    iceberg_ids,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# `EXPORT` TTL expressions other than a constant interval after a `DateTime` column, against the
# partition key of the source and the partition spec of the Iceberg destination: an interval read
# from a column, a TTL that does not follow the partition key, functions of one or several columns,
# `Date`, `DateTime64` and `MATERIALIZED` columns, a `DELETE` TTL of another expression, and a
# change of the expression.

# No merge is assigned, so parts are exported as inserted.
NO_MERGES = {"max_bytes_to_merge_at_max_space_in_pool": 1}


def make_tables(node, engine, columns, partition_by, spec, ttl, extra_ttl="", destination_columns=None, settings=None):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"expr_mt_{suffix}", f"expr_iceberg_{suffix}"
    create_iceberg(node, iceberg_table, columns=destination_columns or columns, partition_by=spec)
    create_source(
        node, mt_table, columns, partition_by, f"{ttl} EXPORT TO TABLE {iceberg_table}{extra_ttl}",
        engine=engine, settings=settings,
    )
    return mt_table, iceberg_table


def partitions_where(node, table, condition):
    return node.query(f"SELECT DISTINCT _partition_id FROM {table} WHERE {condition} ORDER BY 1").split()


def values_of(node, table, expression, condition):
    return {int(x) for x in node.query(f"SELECT DISTINCT {expression} FROM {table} WHERE {condition}").split()}


def assert_nothing_exported(node, table, partition_ids):
    rows = ttl_rows(node, table)
    for partition_id in partition_ids:
        row = rows.get(partition_id)
        assert row is None or (row["exported_parts"] == 0 and row["eligible_parts"] == 0), row


def test_retention_from_a_column(cluster, source_engine):
    """Each row is kept for the number of days in its `retention` column. The source is partitioned by
    it, so the rows of a part are due together: the partitions that are due are exported, the others
    are not. The destination then declares `eventDate` as `Date32`, read back from the Iceberg
    metadata, and a part inserted later is exported to it too."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, "id Int64, eventDate Date, retention Int32",
        "(eventDate, retention)", "(eventDate, retention)", "eventDate + toIntervalDay(retention)",
    )
    due = "eventDate + toIntervalDay(retention) <= now()"

    node.query(
        f"INSERT INTO {mt_table} VALUES (1, today() - 10, 5), (2, today() - 10, 30), (3, today() - 40, 30),"
        f" (4, today() - 2, 1), (5, today(), 7)"
    )
    due_partitions = partitions_where(node, mt_table, due)
    assert len(due_partitions) == 3, due_partitions
    wait_for_partitions_exported(node, mt_table, due_partitions)
    time.sleep(3)
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3, 4])
    assert_nothing_exported(node, mt_table, partitions_where(node, mt_table, f"NOT ({due})"))

    node.query(f"INSERT INTO {mt_table} VALUES (6, today() - 20, 10)")
    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 3, 4, 6], 60, "The part inserted later was not exported")
    wait_for_partitions_exported(node, mt_table, partitions_where(node, mt_table, due))

    assert assert_iceberg_files_partitioned(node, iceberg_table, "retention", "retention") == {1, 5, 10, 30}
    assert assert_iceberg_files_partitioned(node, iceberg_table, "eventDate", "toRelativeDayNum(eventDate)") == values_of(
        node, mt_table, "toRelativeDayNum(eventDate)", due
    )
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_part_is_exported_once_all_its_rows_are_due(cluster, source_engine):
    """With a TTL that does not follow the partition key, a part holds rows that are due at different
    times, and it is exported once the last of them is due."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, "id Int64, customer_id Int64, eventDate Date",
        "customer_id", "customer_id", "eventDate + INTERVAL 30 DAY", settings=NO_MERGES,
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, 1, today() - 40), (2, 1, today() - 35)")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 1, today() - 50), (4, 1, today())")
    node.query(f"INSERT INTO {mt_table} VALUES (5, 2, today() - 45), (6, 3, today() - 10)")
    wait_for_partitions_exported(node, mt_table, ["1", "2"])
    time.sleep(3)

    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 5])
    row = ttl_rows(node, mt_table)["1"]
    assert row["exported_parts"] == 1 and row["eligible_parts"] == 0, row
    assert len(active_parts(node, mt_table, "1")) == 2
    assert_nothing_exported(node, mt_table, ["3"])
    assert assert_iceberg_files_partitioned(node, iceberg_table, "customer_id", "customer_id") == {1, 2}
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_due_part_merged_with_a_part_not_due_waits_for_it(cluster, source_engine):
    """A due part that no group has claimed yet, e.g. while its group is collected, may merge with a
    part of the same partition that is not due. The merged part is exported only once all its rows are
    due, so the rows of the due part wait for those of the other one. This pins the current behaviour."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, "id Int64, customer_id Int64, eventDate Date",
        "customer_id", "customer_id", "eventDate + INTERVAL 30 DAY",
    )

    # Stopped moves pause the TTL export, so the due part is not claimed before the merge.
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 1, today() - 40)")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 1, today())")
    def merged():
        node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '1' FINAL")
        return len(active_parts(node, mt_table, "1")) == 1

    wait_until(merged, 60, "The parts were not merged", interval=1)

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_until(lambda: "1" in ttl_rows(node, mt_table), 30, "The partition is not shown")
    time.sleep(3)

    assert_nothing_exported(node, mt_table, ["1"])
    assert ttl_tasks(node, mt_table) == []
    assert iceberg_ids(node, iceberg_table) == []


def test_ttl_of_the_start_of_the_month(cluster):
    """A month is due as a whole, one month after it starts."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, "MergeTree", "id Int64, eventDate Date",
        "toYYYYMM(eventDate)", "toMonthNumSinceEpoch(eventDate)", "toStartOfMonth(eventDate) + INTERVAL 1 MONTH",
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2020-01-05'), (2, '2020-01-31'), (3, '2020-02-01'), (4, today())")
    wait_for_partitions_exported(node, mt_table, ["202001", "202002"])
    time.sleep(3)

    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3])
    assert assert_iceberg_files_partitioned(node, iceberg_table, "eventDate", "toMonthNumSinceEpoch(eventDate)") == values_of(
        node, mt_table, "toMonthNumSinceEpoch(eventDate)", "id != 4"
    )
    assert_nothing_exported(node, mt_table, partitions_where(node, mt_table, "id = 4"))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_datetime64_column(cluster):
    """A `DateTime64(3)` column is exported with its milliseconds to the `timestamp` column of Iceberg,
    which holds microseconds and is declared as `DateTime64(6)`."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, "MergeTree", "id Int64, ts DateTime64(3)",
        "toDate(ts)", "toRelativeDayNum(ts)", "ts + INTERVAL 1 HOUR",
        destination_columns="id Int64, ts DateTime64(6)",
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2020-01-05 10:20:30.123'), (2, '2020-01-05 23:59:59.999'), (3, now64(3))")
    wait_for_partitions_exported(node, mt_table, ["20200105"])
    node.query(f"INSERT INTO {mt_table} VALUES (4, '2020-01-06 00:00:00.001')")
    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 2, 4], 60, "The part inserted later was not exported")
    wait_for_partitions_exported(node, mt_table, ["20200105", "20200106"])

    assert node.query(f"SELECT id, ts FROM {iceberg_table} ORDER BY id") == (
        "1\t2020-01-05 10:20:30.123000\n2\t2020-01-05 23:59:59.999000\n4\t2020-01-06 00:00:00.001000\n"
    )
    assert assert_iceberg_files_partitioned(node, iceberg_table, "ts", "toRelativeDayNum(ts)") == values_of(
        node, mt_table, "toRelativeDayNum(ts)", "id != 3"
    )
    assert_nothing_exported(node, mt_table, partitions_where(node, mt_table, "id = 3"))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_ttl_of_a_materialized_column(cluster):
    """The TTL and the partition key use a `MATERIALIZED` column, which is exported as an ordinary one."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, "MergeTree", "id Int64, ts DateTime, eventDate Date MATERIALIZED toDate(ts)",
        "toYYYYMM(eventDate)", "toMonthNumSinceEpoch(eventDate)", "eventDate + INTERVAL 1 DAY",
        destination_columns="id Int64, ts DateTime, eventDate Date",
    )

    node.query(f"INSERT INTO {mt_table} (id, ts) VALUES (1, '2020-03-10 10:00:00'), (2, now())")
    wait_for_partitions_exported(node, mt_table, ["202003"])
    time.sleep(3)

    assert node.query(f"SELECT id, toDateTime(ts), eventDate FROM {iceberg_table} ORDER BY id") == "1\t2020-03-10 10:00:00\t2020-03-10\n"
    assert_nothing_exported(node, mt_table, partitions_where(node, mt_table, "id = 2"))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_ttl_of_several_columns(cluster):
    """A row is due a week after the later of its creation and its last update."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, "MergeTree", "id Int64, created DateTime, updated DateTime",
        "toYYYYMM(created)", "toMonthNumSinceEpoch(created)", "greatest(created, updated) + INTERVAL 7 DAY",
    )

    node.query(
        f"INSERT INTO {mt_table} VALUES (1, '2020-01-05 10:00:00', '2020-01-06 10:00:00'),"
        f" (2, '2020-02-05 10:00:00', now()), (3, '2020-03-05 10:00:00', '2020-03-06 10:00:00')"
    )
    wait_for_partitions_exported(node, mt_table, ["202001", "202003"])
    time.sleep(3)

    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3])
    assert_nothing_exported(node, mt_table, ["202002"])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_delete_ttl_waits_for_the_rows_of_its_part_not_due_for_export(cluster, source_engine):
    """With a `DELETE` TTL of another expression, a row due for deletion stays while its part holds
    rows that are not due for export yet: the delete gate holds the part, and it is neither exported
    nor are its rows deleted. A part whose rows are all due is exported, and then deleted."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, "id Int64, customer_id Int64, eventDate Date",
        "customer_id", "customer_id", "eventDate + INTERVAL 30 DAY", extra_ttl=", eventDate + INTERVAL 90 DAY DELETE",
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, 1, today() - 100), (2, 1, today()), (3, 2, today() - 100)")
    wait_for_partitions_exported(node, mt_table, ["2"])

    def deleted():
        node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2' FINAL")
        return node.query(f"SELECT count() FROM {mt_table} WHERE customer_id = 2") == "0\n"

    wait_until(deleted, 60, "The exported rows were not deleted", interval=1)

    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '1' FINAL")
    node.query(f"ALTER TABLE {mt_table} MATERIALIZE TTL", settings={"mutations_sync": 2})
    assert node.query(f"SELECT groupArray(id) FROM (SELECT id FROM {mt_table} ORDER BY id)") == "[1,2]\n"
    wait_until(
        lambda: ttl_rows(node, mt_table).get("1", {}).get("parts_held_by_delete_gate") == 1, 30,
        "The held part is not shown",
    )
    assert_nothing_exported(node, mt_table, ["1"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [3])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_changing_the_ttl_to_a_retention_column(cluster, source_engine):
    """`MODIFY TTL` recalculates when each part is due under the new expression. A part exported under
    the previous expression is not exported again, as the destination is the same."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, "id Int64, eventDate Date, retention Int32", "id", "id", "eventDate + INTERVAL 30 DAY",
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, today() - 40, 5), (2, today() - 10, 5), (3, today() - 10, 30)")
    wait_for_partitions_exported(node, mt_table, ["1"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])

    node.query(
        f"ALTER TABLE {mt_table} MODIFY TTL eventDate + toIntervalDay(retention) EXPORT TO TABLE {iceberg_table}",
        settings={"mutations_sync": 2},
    )
    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 2], 60, "The part due under the new expression was not exported")
    wait_for_partitions_exported(node, mt_table, ["1", "2"])
    time.sleep(3)

    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert [len(task["parts"]) for task in completed_ttl_tasks(node, mt_table)] == [1, 1]
    assert_nothing_exported(node, mt_table, ["3"])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
