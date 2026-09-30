import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    NOT_DUE,
    active_parts,
    assert_exactly_once,
    assert_never_merged,
    assert_one_snapshot_per_task,
    completed_ttl_tasks,
    create_iceberg,
    create_source,
    group_in_flight,
    iceberg_ids,
    optimize_final_error,
    scheduler_holder,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1", "replica2"]

# Parts in different export states are never merged, so an exported range never covers rows that
# were not exported: a part is exported, being exported (claimed), or not exported, and merges only
# combine parts of the same state, never claimed ones, and never across blocks of another state.
# The destination is an Iceberg table.


def due_in(seconds):
    return f"now() - INTERVAL 1 DAY + INTERVAL {seconds} SECOND"


def make_tables(node, engine, settings=None, columns=COLUMNS):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"fence_mt_{suffix}", f"fence_iceberg_{suffix}"
    create_iceberg(node, iceberg_table, columns=columns)
    create_source(
        node, mt_table, columns, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}", engine=engine, settings=settings,
    )
    return mt_table, iceberg_table


def insert_parts(node, table, rows):
    """One part per row, with the scheduler paused so they are shipped in one group."""
    node.query(f"SYSTEM STOP MOVES {table}")
    for row in rows:
        node.query(f"INSERT INTO {table} VALUES {row}")
    node.query(f"SYSTEM START MOVES {table}")


def test_parts_in_different_states_never_merge(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    exported_part = active_parts(node, mt_table)[0]
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {NOT_DUE})")
    not_exported_part = [name for name in active_parts(node, mt_table) if name != exported_part][0]

    node.query(f"SYSTEM START MERGES {mt_table}")
    assert "different export states" in optimize_final_error(node, mt_table, "2020")
    assert_never_merged(node, mt_table, [exported_part, not_exported_part])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_claimed_parts_never_merge(cluster, source_engine):
    """The parts of a group being exported are not merged, not even with each other, until it commits."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    with group_in_flight(node) as wait_paused:
        insert_parts(node, mt_table, [f"(1, 2020, {DUE})", f"(2, 2020, {DUE})"])
        wait_paused()
        claimed = active_parts(node, mt_table)
        assert len(claimed) == 2
        assert ttl_rows(node, mt_table)["2020"]["claimed_parts"] == 2

        node.query(f"SYSTEM START MERGES {mt_table}")
        assert "are being exported" in optimize_final_error(node, mt_table, "2020")
        assert_never_merged(node, mt_table, claimed, seconds=6)
        assert [task["status"] for task in ttl_tasks(node, mt_table)] == ["PENDING"]
        assert iceberg_ids(node, iceberg_table) == []

    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)

    # Once exported, they are in the same state and merge.
    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
    assert len(active_parts(node, mt_table)) == 1


def test_parts_do_not_merge_across_exported_blocks(cluster, source_engine):
    """Two parts that are not exported, around a dropped exported part, are not merged: the merged
    part would cover the exported block and would look exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {due_in(40)})")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2020, {due_in(40)})")
    wait_until(lambda: ttl_rows(node, mt_table).get("2020", {}).get("exported_parts") == 1, 60, "The middle part was not exported")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    first, middle, last = active_parts(node, mt_table)
    node.query(f"ALTER TABLE {mt_table} DROP PART '{middle}'")
    node.query(f"SYSTEM START MERGES {mt_table}")

    assert "export states than blocks between them" in optimize_final_error(node, mt_table, "2020")
    assert_never_merged(node, mt_table, [first, last])

    # Once due, both are exported.
    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 2, 3], 90, "The outer parts were not exported")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert sorted(part for task in ttl_tasks(node, mt_table)[1:] for part in task["parts"]) == [first, last]
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3])
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


def test_eligible_parts_merge_while_the_group_is_collected(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_batch_window_seconds": 8, "ttl_export_batch_max_delay_seconds": 600})
    node.query(f"SYSTEM STOP MERGES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {DUE})")
    node.query(f"SYSTEM START MERGES {mt_table}")
    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
    merged = active_parts(node, mt_table)
    assert len(merged) == 1

    wait_for_partitions_exported(node, mt_table, ["2020"])
    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 1 and tasks[0]["parts"] == merged, tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_every_replica_enforces_the_fence(cluster):
    replicas = [cluster.instances["replica1"], cluster.instances["replica2"]]
    suffix = unique_suffix()
    mt_table, iceberg_table = f"fence_mt_{suffix}", f"fence_iceberg_{suffix}"
    create_iceberg(replicas, iceberg_table)
    for replica in replicas:
        create_source(
            replica, mt_table, COLUMNS, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}",
            engine="ReplicatedMergeTree", replica_name=replica.name,
        )
        replica.query(f"SYSTEM STOP MERGES {mt_table}")

    replicas[0].query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    for replica in replicas:
        replica.query(f"SYSTEM SYNC REPLICA {mt_table}")
    wait_for_partitions_exported(replicas[0], mt_table, ["2020"])
    replicas[0].query(f"INSERT INTO {mt_table} VALUES (2, 2020, {NOT_DUE})")
    for replica in replicas:
        replica.query(f"SYSTEM SYNC REPLICA {mt_table}")

    parts = active_parts(replicas[0], mt_table)
    assert len(parts) == 2
    holder = scheduler_holder(replicas[0], mt_table)
    other = next(replica for replica in replicas if replica.name != holder)

    for replica in replicas:
        replica.query(f"SYSTEM START MERGES {mt_table}")
    assert "different export states" in optimize_final_error(other, mt_table, "2020")
    for replica in replicas:
        assert_never_merged(replica, mt_table, parts)
        assert active_parts(replica, mt_table) == parts
        assert_exactly_once(iceberg_ids(replica, iceberg_table), [1])
    assert_one_snapshot_per_task(replicas[0], mt_table, iceberg_table)


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
    """A part that a mutation emptied, kept by `remove_empty_parts = 0`, has nothing to export, but its
    group records it as exported: otherwise it would stay apart from the exported parts around it, and
    the partition could never be merged into one part. A group of such parts only has no task."""
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
    assert len(tasks) == 1 and len(tasks[0]["parts"]) == 2, tasks
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 3])

    node.query(f"INSERT INTO {mt_table} VALUES (4, 2020, {NOT_DUE})")
    node.query(f"ALTER TABLE {mt_table} DELETE WHERE id = 4", settings={"mutations_sync": 2})
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=4)
    assert len(ttl_tasks(node, mt_table)) == 1

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
