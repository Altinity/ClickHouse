import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    NOT_DUE,
    active_parts,
    assert_exactly_once,
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    completed_ttl_tasks_with_files,
    create_iceberg,
    create_source,
    failpoint,
    group_in_flight,
    iceberg_ids,
    optimize_partition,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# Nothing restricts merges: parts merge whatever their export state. Merges keep the number of the
# block every row was inserted in (`enable_block_number_column`), so an export task reads only the
# rows of the blocks it exports, and a part merged from exported and other parts is exported later
# with only its other rows. The destination is an Iceberg table.


def due_in(seconds):
    """A value of `t` that makes the `EXPORT` TTL `t + INTERVAL 1 DAY` due *seconds* after the insert."""
    return f"now() - INTERVAL 1 DAY + INTERVAL {seconds} SECOND"


def make_tables(node, engine, settings=None, columns=COLUMNS, extra_ttl=""):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"merges_mt_{suffix}", f"merges_iceberg_{suffix}"
    create_iceberg(node, iceberg_table, columns=columns)
    create_source(
        node, mt_table, columns, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}{extra_ttl}",
        engine=engine, settings=settings,
    )
    return mt_table, iceberg_table


def insert_parts(node, table, rows):
    """One part per row, with the scheduler paused so they are shipped in one group."""
    node.query(f"SYSTEM STOP MOVES {table}")
    for row in rows:
        node.query(f"INSERT INTO {table} VALUES {row}")
    node.query(f"SYSTEM START MOVES {table}")


def test_exported_and_unexported_parts_merge(cluster, source_engine):
    """An exported part merges with a part that is not due yet. Once the merged part is due, only the
    rows of the other part are exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {due_in(30)})")

    node.query(f"SYSTEM START MERGES {mt_table}")
    optimize_partition(node, mt_table, "2020")
    merged = active_parts(node, mt_table)
    assert len(merged) == 1
    assert node.query(f"SELECT uniqExact(_block_number) FROM {mt_table}") == "2\n"
    assert ttl_rows(node, mt_table)["2020"]["exported_parts"] == 0, "A part with rows that are not exported is shown as exported"

    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 2], 90, "The rows of the new part were not exported")
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 2 and tasks[1]["parts"] == merged, tasks

    time.sleep(3)
    assert len(ttl_tasks(node, mt_table)) == 2
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_claimed_parts_merge_while_exporting(cluster, source_engine):
    """The parts of a group being exported merge as usual. The task reads the parts it lists, which
    are kept until it ends, and once it commits the merged part is exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    with group_in_flight(node) as wait_paused:
        insert_parts(node, mt_table, [f"(1, 2020, {DUE})", f"(2, 2020, {DUE})"])
        wait_paused()
        claimed = active_parts(node, mt_table)
        assert len(claimed) == 2

        node.query(f"SYSTEM START MERGES {mt_table}")
        optimize_partition(node, mt_table, "2020")
        merged = active_parts(node, mt_table)
        assert len(merged) == 1
        assert ttl_rows(node, mt_table)["2020"]["claimed_parts"] == 1
        assert [task["status"] for task in ttl_tasks(node, mt_table)] == ["PENDING"]
        assert iceberg_ids(node, iceberg_table) == []

    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 1 and sorted(tasks[0]["parts"]) == claimed, tasks

    time.sleep(3)
    assert len(ttl_tasks(node, mt_table)) == 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_retry_after_a_merge_exports_the_same_rows(cluster, source_engine):
    """A failed task is retried with its blocks, under its commit id. After its part merged with a part
    that is not due, the retry reads the merged part, and only the rows of those blocks."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    with failpoint([node], "export_part_non_retryable_throw"):
        node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
        wait_until(lambda: any(task["status"] == "FAILED" for task in ttl_tasks(node, mt_table)), 60, "The task did not fail")
        node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {NOT_DUE})")

        node.query(f"SYSTEM START MERGES {mt_table}")
        optimize_partition(node, mt_table, "2020")
        merged = active_parts(node, mt_table)
        assert len(merged) == 1

        # A retry starts only after the previous one failed, so every later one reads the merged part.
        wait_until(
            lambda: any(task["status"] == "FAILED" and task["parts"] == merged for task in ttl_tasks(node, mt_table)),
            60, "No retry read the merged part",
        )

    wait_until(lambda: completed_ttl_tasks(node, mt_table), 60, "The retry did not complete")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    tasks = ttl_tasks(node, mt_table)
    assert len({task["commit_id"] for task in tasks}) == 1, tasks
    assert [task["parts"] for task in completed_ttl_tasks(node, mt_table)] == [merged], tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_part_merged_across_a_dropped_block(cluster, source_engine):
    """A part merged across the block of a dropped part that was never exported is not exported as a
    whole. Once due, that block is exported without rows, and the delete gate releases the part."""
    node = cluster.instances["replica1"]
    # A plain `MergeTree` replaces the dropped part with an empty one, of a higher level, and drops that
    # one too. The parts around it are not merged until it is removed, which happens soon with these.
    fast_cleanup = {"old_parts_lifetime": 1, "cleanup_delay_period": 1, "max_cleanup_delay_period": 2}
    mt_table, iceberg_table = make_tables(node, source_engine, settings=fast_cleanup, extra_ttl=", t + INTERVAL 2 DAY DELETE")
    node.query(f"SYSTEM STOP MERGES {mt_table}")
    # A merge would apply the due `DELETE` TTL to the exported parts.
    node.query(f"SYSTEM STOP TTL MERGES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    first = active_parts(node, mt_table)

    # The paused scheduler must not export the empty part that a plain `MergeTree` has for a moment.
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {NOT_DUE})")
    dropped = node.query(f"SELECT _part FROM {mt_table} WHERE id = 2").strip()
    node.query(f"ALTER TABLE {mt_table} DROP PART '{dropped}'")
    assert active_parts(node, mt_table) == first
    node.query(f"SYSTEM START MOVES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (3, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=2)

    node.query(f"SYSTEM START MERGES {mt_table}")

    def merged_around_the_dropped_part():
        node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
        return len(active_parts(node, mt_table)) == 1

    wait_until(merged_around_the_dropped_part, 60, "The parts around the dropped part were not merged", interval=1)
    merged = active_parts(node, mt_table)
    assert node.query(f"SELECT groupArray(id) FROM (SELECT id FROM {mt_table} ORDER BY id)") == "[1,3]\n"

    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 3 and tasks[2]["parts"] == merged, tasks
    assert tasks[2]["transaction_id"] not in completed_ttl_tasks_with_files(node, mt_table), "The dropped block was exported with rows"
    assert ttl_rows(node, mt_table)["2020"]["parts_held_by_delete_gate"] == 0
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3])

    node.query(f"SYSTEM START TTL MERGES {mt_table}")

    def deleted():
        node.query(f"OPTIMIZE TABLE {mt_table} FINAL")
        return node.query(f"SELECT count() FROM {mt_table}") == "0\n"

    wait_until(deleted, 60, "The `DELETE` TTL was not applied once the part was exported", interval=1)
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_exported_parts_merge_with_each_other(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")
    insert_parts(node, mt_table, [f"(1, 2020, {DUE})", f"(2, 2020, {DUE})"])
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert len(active_parts(node, mt_table)) == 2

    node.query(f"SYSTEM START MERGES {mt_table}")
    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
    assert len(active_parts(node, mt_table)) == 1

    # The merged part is exported, so nothing is exported again.
    time.sleep(3)
    assert len(ttl_tasks(node, mt_table)) == 1
    assert ttl_rows(node, mt_table)["2020"]["exported_parts"] == 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_eligible_parts_merge_before_they_are_shipped(cluster, source_engine):
    """Due parts that are not claimed merge as usual, and the merged part is shipped."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"SYSTEM STOP MERGES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    node.query(f"SYSTEM START MERGES {mt_table}")
    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
    merged = active_parts(node, mt_table)
    assert len(merged) == 1

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 1 and tasks[0]["parts"] == merged, tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_mutation_keeps_parts_exported(cluster, source_engine):
    """A mutation rewrites a part under the same block range, so an exported part stays exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, columns="id Int64, year Int32, t DateTime, v Int32")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}, 7)")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    before = active_parts(node, mt_table)

    node.query(f"ALTER TABLE {mt_table} UPDATE v = 8 WHERE 1", settings={"mutations_sync": 2})
    assert active_parts(node, mt_table) != before

    time.sleep(3)
    assert len(ttl_tasks(node, mt_table)) == 1
    assert ttl_rows(node, mt_table)["2020"]["exported_parts"] == 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert node.query(f"SELECT v FROM {iceberg_table}") == "7\n", "The destination has the rows as they were exported"
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_parts_without_rows_are_recorded_as_exported(cluster, source_engine):
    """A part that a mutation emptied, kept by `remove_empty_parts = 0`, has nothing to export, but it
    is exported with its group, writing nothing, so it is recorded as exported: a part merged from it
    and the exported parts around it is then exported too. A group of such parts only is a task that
    commits nothing."""
    node = cluster.instances["replica1"]
    # No merge is assigned until the end, so the emptied part stays between the others.
    mt_table, iceberg_table = make_tables(
        node, source_engine, settings={"remove_empty_parts": 0, "max_bytes_to_merge_at_max_space_in_pool": 1}
    )
    rows_of_parts = f"SELECT rows FROM system.parts WHERE database = currentDatabase() AND table = '{mt_table}' AND active ORDER BY name"

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    for row in [f"(1, 2020, {DUE})", f"(2, 2020, {NOT_DUE})", f"(3, 2020, {DUE})"]:
        node.query(f"INSERT INTO {mt_table} VALUES {row}")
    node.query(f"ALTER TABLE {mt_table} DELETE WHERE id = 2", settings={"mutations_sync": 2})
    assert node.query(rows_of_parts).split() == ["1", "0", "1"]
    node.query(f"SYSTEM START MOVES {mt_table}")

    wait_for_partitions_exported(node, mt_table, ["2020"], exported=3)
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 1 and len(tasks[0]["parts"]) == 3, tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3])

    node.query(f"INSERT INTO {mt_table} VALUES (4, 2020, {NOT_DUE})")
    node.query(f"ALTER TABLE {mt_table} DELETE WHERE id = 4", settings={"mutations_sync": 2})
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=4)
    tasks = ttl_tasks(node, mt_table)
    assert sorted(len(task["parts"]) for task in tasks) == [1, 3], tasks
    assert all(task["status"] == "COMPLETED" for task in tasks), tasks
    assert completed_ttl_tasks_with_files(node, mt_table) == [task["transaction_id"] for task in tasks if len(task["parts"]) == 3]

    node.query(f"ALTER TABLE {mt_table} RESET SETTING max_bytes_to_merge_at_max_space_in_pool")
    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
    assert len(active_parts(node, mt_table)) == 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_block_numbers_are_not_reused_after_a_restart(cluster):
    """A plain `MergeTree` table allocates block numbers from its parts. The exported blocks of a
    dropped partition must not be allocated again, or a new part would look exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, "MergeTree")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    node.query(f"ALTER TABLE {mt_table} DROP PARTITION 2020")
    node.restart_clickhouse()

    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2, 60, "The new part was not exported")
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
