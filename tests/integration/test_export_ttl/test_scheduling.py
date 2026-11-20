import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    NOT_DUE,
    assert_exactly_once,
    assert_iceberg_files_partitioned,
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    create_iceberg,
    create_source,
    group_in_flight,
    iceberg_ids,
    iceberg_orphan_files,
    iceberg_snapshots,
    snapshot_refreshes,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# When the `EXPORT` TTL ships groups of parts to an Iceberg destination: every check ships the parts
# that are due by then, one group per partition, a partition has at most one group being exported,
# every group is one snapshot, and no part is exported twice however many checks go by.


def due_in(seconds):
    """A value of `t` that makes the TTL `t + INTERVAL 1 DAY` due *seconds* after the insert."""
    return f"now() - INTERVAL 1 DAY + INTERVAL {seconds} SECOND"


def make_tables(node, engine, settings=None, prefix="sched"):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"{prefix}_mt_{suffix}", f"{prefix}_iceberg_{suffix}"
    create_iceberg(node, iceberg_table)
    # No merge is assigned, so parts are shipped as inserted: a replica with stopped merges would
    # still be assigned merges that it never runs, and a part that is being merged waits for it.
    all_settings = {"max_bytes_to_merge_at_max_space_in_pool": 1}
    all_settings.update(settings or {})
    create_source(
        node, mt_table, COLUMNS, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}",
        engine=engine, settings=all_settings,
    )
    return mt_table, iceberg_table


def test_exports_due_parts_once(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2021, {NOT_DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert assert_iceberg_files_partitioned(node, iceberg_table, "year", "year") == {2020}

    # Later checks and later due inserts never export a part again.
    node.query(f"INSERT INTO {mt_table} VALUES (4, 2020, {DUE})")
    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2)
    wait_for_partitions_exported(node, mt_table, ["2020"])
    time.sleep(3)
    assert len(ttl_tasks(node, mt_table)) == 2
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 4])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
    assert iceberg_orphan_files(node, iceberg_table) == set()

    # The part that is not due stays in the source only.
    row = ttl_rows(node, mt_table)["2021"]
    assert row["eligible_parts"] == 0 and row["exported_parts"] == 0, row
    assert all(task["commit_id"] == task["transaction_id"] for task in ttl_tasks(node, mt_table)), ttl_tasks(node, mt_table)


def test_due_parts_ship_on_the_next_check(cluster, source_engine):
    """The check period is the batch interval: a check ships every part that is due by then, one
    group per partition, and a part that is not due yet waits for a later check."""
    node = cluster.instances["replica1"]
    period = 5
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_check_period_seconds": period})

    # Paused, so that no check runs between the inserts.
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    for i in range(3):
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2021, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (4, 2020, {due_in(4 * period)})")
    inserted = time.time()
    node.query(f"SYSTEM START MOVES {mt_table}")

    def started():
        tasks = ttl_tasks(node, mt_table)
        return len(tasks) == 2 and tasks

    tasks = wait_until(started, 3 * period, "The due parts were not shipped by the next check")
    assert sorted((task["partition_id"], len(task["parts"])) for task in tasks) == [("2020", 3), ("2021", 1)], tasks

    while time.time() - inserted < 3 * period:
        assert len(ttl_tasks(node, mt_table)) == 2, "The part was exported before it was due"
        time.sleep(1)

    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 3, 60, "The part was not exported once it was due")
    wait_for_partitions_exported(node, mt_table, ["2020", "2021"])
    assert len(ttl_tasks(node, mt_table)[-1]["parts"]) == 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(5))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_one_group_per_partition_at_a_time(cluster, source_engine):
    """A partition has at most one group being exported: the parts that become due meanwhile wait
    for it, and the next check after it committed ships them together."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    with group_in_flight(node) as wait_paused:
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        wait_paused()
        node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
        node.query(f"INSERT INTO {mt_table} VALUES (3, 2020, {DUE})")
        # Several checks go by while the group is in flight.
        time.sleep(4)
        tasks = ttl_tasks(node, mt_table)
        assert [(task["status"], len(task["parts"])) for task in tasks] == [("PENDING", 1)], tasks
        assert ttl_rows(node, mt_table)["2020"]["eligible_parts"] == 2

    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2, 60, "The parts that became due meanwhile were not exported")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert [len(task["parts"]) for task in ttl_tasks(node, mt_table)] == [1, 2]
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_idle_checks_change_nothing(cluster, source_engine):
    """Once everything due is exported, further checks start no task, commit nothing, do not read the
    index again, and show the same state."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2021, {NOT_DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    rows = ttl_rows(node, mt_table)
    refreshes = snapshot_refreshes(node)
    snapshots = iceberg_snapshots(node, iceberg_table)
    time.sleep(10)
    assert len(ttl_tasks(node, mt_table)) == 1
    assert snapshot_refreshes(node) == refreshes, "The index was read again although it did not change"
    assert iceberg_snapshots(node, iceberg_table) == snapshots
    assert ttl_rows(node, mt_table) == rows
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])


def test_stop_moves_pauses_and_start_moves_resumes(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")

    time.sleep(4)
    assert ttl_tasks(node, mt_table) == []
    assert ttl_rows(node, mt_table)["2020"]["eligible_parts"] == 1

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_plain_commit_is_recorded_without_waiting_for_the_period(cluster):
    """On a plain `MergeTree` table the task commits first, and the index records the parts as exported
    afterwards. The scheduler is woken by the commit, so it does not wait for the next check."""
    node = cluster.instances["replica1"]
    period = 20
    mt_table, iceberg_table = make_tables(node, "MergeTree", settings={"ttl_export_check_period_seconds": period})
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")

    completed_at = None
    start = time.time()
    while True:
        now = time.time()
        assert now - start < 3 * period, "The part was not exported"
        if completed_at is None and completed_ttl_tasks(node, mt_table):
            completed_at = now
        rows = ttl_rows(node, mt_table)
        if completed_at is not None and "2020" in rows and rows["2020"]["exported_parts"] == 1:
            break
        time.sleep(0.2)

    assert time.time() - completed_at < period / 2, "The commit was recorded only by a periodic check"
    assert rows["2020"]["claimed_parts"] == 0 and rows["2020"]["scheduler_replica"] == "", rows
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
