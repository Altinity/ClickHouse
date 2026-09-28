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
    failpoint,
    first_eligible_time,
    iceberg_ids,
    iceberg_orphan_files,
    iceberg_snapshots,
    pending_ttl_tasks,
    snapshot_refreshes,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# When the `EXPORT` TTL ships groups of parts to an Iceberg destination: on the checks after their
# parts are due, batched by the window, the maximum delay and the size threshold, limited in size and
# concurrency, one snapshot per group, and never twice however many checks go by.


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
    assert [task["retry_of"] for task in ttl_tasks(node, mt_table)] == [[], []]


def test_part_becoming_due_wakes_the_scheduler(cluster, source_engine):
    """A check computes when the next part becomes due, and the scheduler wakes up then instead of
    waiting for the check period."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_check_period_seconds": 120})

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {due_in(12)})")
    inserted = time.time()
    # The table starts with a check, which sees the part that is not due yet.
    node.query(f"DETACH TABLE {mt_table}")
    node.query(f"ATTACH TABLE {mt_table}")

    while time.time() - inserted < 9:
        assert ttl_tasks(node, mt_table) == [], "The part was exported before it was due"
        time.sleep(1)

    wait_for_partitions_exported(node, mt_table, ["2020"], timeout=40)
    assert time.time() - inserted < 40, "The part was exported only by a periodic check"
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_batching_window_spans_checks(cluster, source_engine):
    """Parts that keep becoming due within the window are collected over several checks into one
    group, which is committed as one snapshot once no new part came for the window."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine,
        settings={"ttl_export_batch_window_seconds": 45, "ttl_export_batch_max_delay_seconds": 600, "ttl_export_batch_min_bytes": 0},
    )

    first_eligible = set()
    for i in range(5):
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")
        assert ttl_tasks(node, mt_table) == [], "A group was shipped while parts kept coming"
        first_eligible.add(first_eligible_time(node, mt_table, "2020"))
    first_eligible.discard(0)

    assert len(first_eligible) == 1, f"The first eligible time of the group changed across checks: {first_eligible}"

    wait_for_partitions_exported(node, mt_table, ["2020"], timeout=120)
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 1 and len(tasks[0]["parts"]) == 5, tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(5))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_maximum_delay_ships_while_parts_keep_coming(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine,
        settings={"ttl_export_batch_window_seconds": 15, "ttl_export_batch_max_delay_seconds": 25, "ttl_export_batch_min_bytes": 0},
    )

    start = time.time()
    shipped_at = None
    i = 0
    while time.time() - start < 45:
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")
        i += 1
        if shipped_at is None and ttl_tasks(node, mt_table):
            shipped_at = time.time() - start

    assert shipped_at is not None, "No group was shipped although parts waited longer than the maximum delay"
    assert shipped_at >= 20, f"A group was shipped after {shipped_at:.1f} s, before the maximum delay"

    wait_for_partitions_exported(node, mt_table, ["2020"], timeout=90)
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(i))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_size_threshold_ships_before_the_window(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine,
        settings={"ttl_export_batch_window_seconds": 600, "ttl_export_batch_max_delay_seconds": 600, "ttl_export_batch_min_bytes": 1},
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"], timeout=30)
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_group_size_limits_over_successive_checks(cluster, source_engine):
    """A group is limited to `ttl_export_max_parts_per_group` parts; the rest of the partition is
    shipped by the next groups, one at a time, each its own snapshot."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine,
        settings={"ttl_export_batch_window_seconds": 15, "ttl_export_batch_max_delay_seconds": 600,
                  "ttl_export_batch_min_bytes": 0, "ttl_export_max_parts_per_group": 2},
    )

    for i in range(3):
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")

    # Within the window nothing is exported.
    time.sleep(4)
    assert ttl_tasks(node, mt_table) == []
    assert ttl_rows(node, mt_table)["2020"]["eligible_parts"] == 3

    in_flight = []
    def settled():
        in_flight.append(pending_ttl_tasks(node, mt_table))
        return len(completed_ttl_tasks(node, mt_table)) == 2
    wait_until(settled, 120, "The partition was not exported in two groups")
    assert max(in_flight) <= 1, f"Two groups of one partition were in flight at once: {in_flight}"

    assert sorted(len(task["parts"]) for task in ttl_tasks(node, mt_table)) == [1, 2]
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(3))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_group_bytes_limit_ships_parts_one_by_one(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_max_bytes_per_group": 1})
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    for i in range(3):
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")
    node.query(f"SYSTEM START MOVES {mt_table}")

    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 3, 120, "The parts were not exported one by one")
    assert [len(task["parts"]) for task in ttl_tasks(node, mt_table)] == [1, 1, 1]
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(3))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_concurrent_groups_are_capped(cluster, source_engine):
    """At most `ttl_export_max_concurrent_groups` groups of the table are in flight, whatever the
    number of partitions, and their commits to the one Iceberg table do not get lost."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine,
        settings={"ttl_export_max_concurrent_groups": 2, "ttl_export_settings_profile": "ttl_export_quick_retry"},
    )

    with failpoint([node], "export_part_retryable_throw"):
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2021, {DUE}), (3, 2022, {DUE}), (4, 2023, {DUE})")
        wait_until(lambda: pending_ttl_tasks(node, mt_table) == 2, 60, "Two groups were not started")
        samples = []
        for _ in range(8):
            samples.append(pending_ttl_tasks(node, mt_table))
            time.sleep(0.5)
        assert max(samples) == 2, f"More groups than the limit were in flight: {samples}"
        assert len(ttl_tasks(node, mt_table)) == 2, "A group was started while the limit was reached"
        assert iceberg_ids(node, iceberg_table) == []

    wait_for_partitions_exported(node, mt_table, ["2020", "2021", "2022", "2023"], timeout=120)
    assert len(completed_ttl_tasks(node, mt_table)) == 4
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3, 4])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
    assert assert_iceberg_files_partitioned(node, iceberg_table, "year", "year") == {2020, 2021, 2022, 2023}


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


def test_modified_settings_apply_on_the_next_check(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, settings={"ttl_export_batch_window_seconds": 600, "ttl_export_batch_max_delay_seconds": 600}
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    time.sleep(4)
    assert ttl_tasks(node, mt_table) == []

    node.query(f"ALTER TABLE {mt_table} MODIFY SETTING ttl_export_batch_window_seconds = 0, ttl_export_batch_max_delay_seconds = 0")
    wait_for_partitions_exported(node, mt_table, ["2020"], timeout=30)
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
