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
    create_iceberg,
    create_source,
    group_in_flight,
    iceberg_ids,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
    zookeeper_path,
)

CLUSTER_INSTANCES = ["replica1"]

# Changing, removing and adding back the `EXPORT` TTL expression, and forgetting partitions.

TTL = "t + INTERVAL 1 DAY"


def make_tables(node, engine, settings=None):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"life_mt_{suffix}", f"life_iceberg_{suffix}"
    create_iceberg(node, iceberg_table)
    create_source(node, mt_table, COLUMNS, "year", f"{TTL} EXPORT TO TABLE {iceberg_table}", engine=engine, settings=settings)
    return mt_table, iceberg_table


def wait_for_no_ttl_rows(node, table):
    wait_until(lambda: ttl_rows(node, table) == {}, 60, "The export index was not removed")


def test_removing_the_ttl_lifts_the_fence(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"SYSTEM STOP MERGES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {NOT_DUE})")

    node.query(f"ALTER TABLE {mt_table} REMOVE TTL")
    node.query(f"SYSTEM START MERGES {mt_table}")

    # Once the index of the destination is removed, the exported part merges with the other one.
    def merged():
        node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")
        return len(active_parts(node, mt_table)) == 1

    wait_until(merged, 60, "Parts are still fenced after the EXPORT TTL was removed", interval=1)
    wait_for_no_ttl_rows(node, mt_table)


def test_changing_the_destination(cluster, source_engine):
    """Groups being exported to the previous destination are killed, what was exported to it is
    forgotten, and everything due is exported to the new one."""
    node = cluster.instances["replica1"]
    mt_table, first_table = make_tables(node, source_engine)
    second_table = f"life_iceberg_second_{unique_suffix()}"
    create_iceberg(node, second_table)

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    with group_in_flight(node) as wait_paused:
        node.query(f"INSERT INTO {mt_table} VALUES (2, 2021, {DUE})")
        wait_paused()
        in_flight = [task for task in ttl_tasks(node, mt_table) if task["status"] == "PENDING"]
        assert len(in_flight) == 1

        node.query(
            f"ALTER TABLE {mt_table} MODIFY TTL {TTL} EXPORT TO TABLE {second_table}",
            settings={"materialize_ttl_after_modify": 0},
        )
        wait_until(
            lambda: next(task for task in ttl_tasks(node, mt_table) if task["transaction_id"] == in_flight[0]["transaction_id"])["status"] == "KILLED",
            60, "The task exporting to the previous destination was not killed",
        )

    wait_for_partitions_exported(node, mt_table, ["2020", "2021"])
    wait_until(lambda: iceberg_ids(node, second_table) == [1, 2], 60, "Not everything was exported to the new destination")
    assert_exactly_once(iceberg_ids(node, first_table), [1])
    rows = query_destinations(node, mt_table)
    assert rows == {second_table}, rows


def query_destinations(node, table):
    return set(node.query(f"SELECT DISTINCT destination_table FROM system.ttl_exports WHERE table = '{table}'").split())


def test_changing_the_interval_exports_nothing_again(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    node.query(f"ALTER TABLE {mt_table} MODIFY TTL t + INTERVAL 2 DAY EXPORT TO TABLE {iceberg_table}", settings={"mutations_sync": 2})
    time.sleep(4)
    assert len(ttl_tasks(node, mt_table)) == 1
    assert ttl_rows(node, mt_table)["2020"]["exported_parts"] == 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_adding_the_ttl_back_exports_again(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    node.query(f"ALTER TABLE {mt_table} REMOVE TTL")
    wait_for_no_ttl_rows(node, mt_table)

    node.query(f"ALTER TABLE {mt_table} MODIFY TTL {TTL} EXPORT TO TABLE {iceberg_table}", settings={"mutations_sync": 2})
    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2, 60, "The part was not exported again")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_attached_parts_are_exported_again(cluster, source_engine):
    """Parts attached again get new block numbers, so they are not known as exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    node.query(f"ALTER TABLE {mt_table} DETACH PARTITION 2020")
    node.query(f"ALTER TABLE {mt_table} ATTACH PARTITION 2020")
    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2, 60, "The attached part was not exported")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 1])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_forget_partition(cluster):
    node = cluster.instances["replica1"]
    # The cleanup thread removes the dropped parts from memory, after which the partition can be forgotten.
    mt_table, iceberg_table = make_tables(
        node, "ReplicatedMergeTree", settings={"old_parts_lifetime": 1, "cleanup_delay_period": 1, "max_cleanup_delay_period": 2}
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    destinations_path = f"{zookeeper_path(mt_table)}/export_ttl/destinations"
    destination_key = node.query(f"SELECT name FROM system.zookeeper WHERE path = '{destinations_path}'").strip()
    partitions_path = f"{destinations_path}/{destination_key}/partitions"
    assert node.query(f"SELECT name FROM system.zookeeper WHERE path = '{partitions_path}'") == "2020\n"

    node.query(f"ALTER TABLE {mt_table} DROP PARTITION ID '2020'")
    wait_until(lambda: active_parts(node, mt_table, "2020") == [] and node.query(
        f"SELECT count() FROM system.parts WHERE database = currentDatabase() AND table = '{mt_table}' AND partition_id = '2020'"
    ) == "0\n", 120, "The dropped parts were not removed")

    node.query(f"ALTER TABLE {mt_table} FORGET PARTITION ID '2020'")
    assert node.query(f"SELECT count() FROM system.zookeeper WHERE path = '{partitions_path}'") == "0\n"
