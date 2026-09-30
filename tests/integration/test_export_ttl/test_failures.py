import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    assert_exactly_once,
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    create_iceberg,
    create_source,
    failpoint,
    group_in_flight,
    iceberg_ids,
    iceberg_orphan_files,
    iceberg_snapshots,
    ttl_rows,
    ttl_tasks,
    wait_for_last_error,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# Failures of the tasks of the `EXPORT` TTL to an Iceberg destination: a failed group is retried as a
# new task, which lists in `retry_of` the failed ones that exported all their parts, since only those
# may have committed. Only the task that completes commits a snapshot, and no row lands twice,
# whatever fails and when.


def make_tables(node, engine, settings=None, columns=COLUMNS, source_partition_by="year", spec="year"):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"fail_mt_{suffix}", f"fail_iceberg_{suffix}"
    create_iceberg(node, iceberg_table, columns=columns, partition_by=spec)
    create_source(
        node, mt_table, columns, source_partition_by, f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}",
        engine=engine, settings=settings,
    )
    return mt_table, iceberg_table


def exception_count(node, transaction_id):
    return int(node.query(
        f"SELECT sum(exception_count) FROM system.distributed_exports WHERE transaction_id = '{transaction_id}'"
    ).strip() or 0)


def test_retryable_part_error_is_retried_by_the_same_task(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_settings_profile": "ttl_export_quick_retry"})

    with failpoint([node], "export_part_retryable_throw"):
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        tasks = wait_until(lambda: ttl_tasks(node, mt_table), 60, "No task was started")
        transaction_id = tasks[0]["transaction_id"]
        wait_until(lambda: exception_count(node, transaction_id) > 0, 60, "The part did not fail")
        assert iceberg_ids(node, iceberg_table) == []

    wait_for_partitions_exported(node, mt_table, ["2020"])
    tasks = ttl_tasks(node, mt_table)
    assert [(task["transaction_id"], task["status"]) for task in tasks] == [(transaction_id, "COMPLETED")], tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_non_retryable_part_error_is_retried_as_a_new_task(cluster, source_engine):
    """The failed task did not export its part, so it did not commit: the retry does not check it."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    with failpoint([node], "export_part_non_retryable_throw"):
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        wait_until(lambda: any(task["status"] == "FAILED" for task in ttl_tasks(node, mt_table)), 60, "The task did not fail")
        # Pause, so no retry fails once the failure is gone. On a replicated table this also pauses
        # the part exports of a retry that is already running.
        node.query(f"SYSTEM STOP MOVES {mt_table}")
        assert iceberg_ids(node, iceberg_table) == []

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    tasks = ttl_tasks(node, mt_table)
    failed = {task["transaction_id"] for task in tasks if task["status"] == "FAILED"}
    completed = completed_ttl_tasks(node, mt_table)
    assert failed and len(completed) == 1, tasks
    assert completed[0]["retry_of"] == [], (completed, failed)
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_commit_failure_is_retried_with_the_new_parts(cluster):
    """A group whose commit keeps failing times out, and is retried as a new task that also takes the
    parts that became due meanwhile, and records the failed task in `retry_of`. Only the retry commits."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, "ReplicatedMergeTree", settings={"ttl_export_settings_profile": "ttl_export_fail_fast"})
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    with failpoint([node], "export_partition_commit_always_throw"):
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        tasks = wait_until(lambda: ttl_tasks(node, mt_table), 60, "No task was started")

        # Pause the scheduler, so the retry is not started before the next part is due.
        node.query(f"SYSTEM STOP MOVES {mt_table}")
        failed_transaction_id = tasks[0]["transaction_id"]
        wait_until(
            lambda: ttl_tasks(node, mt_table)[0]["status"] == "KILLED", 120, "The failing task did not time out"
        )
        node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
        assert iceberg_ids(node, iceberg_table) == []

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    completed = completed_ttl_tasks(node, mt_table)
    assert [(len(task["parts"]), task["retry_of"]) for task in completed] == [(2, [failed_transaction_id])], completed
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_commit_that_landed_is_not_committed_again(cluster):
    """The commit to Iceberg lands, but the task is not marked as completed. The retried commit finds
    the transaction in the Iceberg metadata, so the rows are committed once."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, "ReplicatedMergeTree")
    snapshots = iceberg_snapshots(node, iceberg_table)

    with failpoint([node], "iceberg_export_after_commit_before_zk_completed"):
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2020, {DUE})")
        wait_for_partitions_exported(node, mt_table, ["2020"], timeout=120)

    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert iceberg_snapshots(node, iceberg_table) == snapshots + 1
    assert ttl_rows(node, mt_table)["2020"]["exported_parts"] == 1
    assert iceberg_orphan_files(node, iceberg_table) == set()


def test_killed_task_is_retried(cluster, source_engine):
    """The killed task never commits; the file its part export writes afterwards is not in the table.
    It was killed before exporting its part, so the retry does not check it."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    with group_in_flight(node) as wait_paused:
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        wait_paused()
        killed = ttl_tasks(node, mt_table)[0]["transaction_id"]
        node.query(f"KILL EXPORT WHERE transaction_id = '{killed}'")
        wait_until(lambda: ttl_tasks(node, mt_table)[0]["status"] == "KILLED", 60, "The task was not killed")
        # The retry starts while the part export of the killed task is still paused.
        wait_until(lambda: len(ttl_tasks(node, mt_table)) == 2, 60, "The killed task was not retried")

    wait_for_partitions_exported(node, mt_table, ["2020"])
    completed = completed_ttl_tasks(node, mt_table)
    assert len(completed) == 1 and completed[0]["retry_of"] == [], (killed, ttl_tasks(node, mt_table))
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_restart_during_a_group(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    with group_in_flight(node) as wait_paused:
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2020, {DUE})")
        wait_paused()
        transaction_id = ttl_tasks(node, mt_table)[0]["transaction_id"]
        node.restart_clickhouse(kill=True)

    wait_for_partitions_exported(node, mt_table, ["2020"], timeout=120)
    tasks = ttl_tasks(node, mt_table)
    assert [(task["transaction_id"], task["status"]) for task in tasks] == [(transaction_id, "COMPLETED")], tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_restart_before_the_first_check(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    node.query(f"INSERT INTO {mt_table} SELECT number, 2020, {DUE} FROM numbers(1000)")
    node.restart_clickhouse()

    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(1000))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_dropped_destination(cluster, source_engine):
    """Without its destination the TTL exports nothing and reports why. The destination is identified
    by name, so once it is created again, only what was not exported yet is exported to it."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    snapshots = iceberg_snapshots(node, iceberg_table)

    node.query(f"DROP TABLE {iceberg_table} SYNC")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    wait_for_last_error(node, mt_table, "2020", "does not exist")
    time.sleep(2)
    assert len(ttl_tasks(node, mt_table)) == 1

    create_iceberg(node, iceberg_table, attach=True)
    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2, 60, "Nothing was exported to the new destination")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert ttl_rows(node, mt_table)["2020"]["last_error"] == ""

    exported_again = ttl_tasks(node, mt_table)[-1]["parts"]
    assert len(exported_again) == 1, ttl_tasks(node, mt_table)
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    # One snapshot for the task that exported to the table created again.
    assert iceberg_snapshots(node, iceberg_table) == snapshots + 1


def test_partition_failing_its_check_does_not_block_the_others(cluster, source_engine):
    """A group whose rows fall into several days of a day-partitioned Iceberg table is not exported,
    writes nothing, and does not count against `ttl_export_max_concurrent_groups`."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, columns="id Int64, t DateTime", source_partition_by="toYYYYMM(t)", spec="toRelativeDayNum(t)",
        settings={"ttl_export_max_concurrent_groups": 1},
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2020-01-01 10:00:00'), (2, '2020-01-02 10:00:00')")
    node.query(f"INSERT INTO {mt_table} VALUES (3, '2020-02-01 10:00:00'), (4, '2020-03-01 10:00:00'), (5, '2020-04-01 10:00:00')")

    wait_for_partitions_exported(node, mt_table, ["202002", "202003", "202004"])
    row = wait_for_last_error(node, mt_table, "202001", "multiple destination partitions")
    assert row["eligible_parts"] == 1 and row["claimed_parts"] == 0 and row["current_transaction_id"] == "", row
    assert all(task["partition_id"] != "202001" for task in ttl_tasks(node, mt_table))
    assert_exactly_once(iceberg_ids(node, iceberg_table), [3, 4, 5])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
    assert iceberg_orphan_files(node, iceberg_table) == set(), "A group that failed its check wrote files"
