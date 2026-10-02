import time

from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    assert_exactly_once,
    assert_one_snapshot_per_task,
    create_iceberg,
    create_source,
    iceberg_ids,
    ttl_rows,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# While a table has an `EXPORT` TTL, TTL that deletes or rewrites rows waits until the part is
# exported, so no row is lost before it reaches the Iceberg destination.


def make_tables(node, engine, extra_ttl, columns=COLUMNS, destination_columns=None, order_by="tuple()"):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"gate_mt_{suffix}", f"gate_iceberg_{suffix}"
    create_iceberg(node, iceberg_table, columns=destination_columns or columns)
    create_source(
        node, mt_table, columns, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}{extra_ttl}",
        engine=engine, order_by=order_by,
    )
    return mt_table, iceberg_table


def held_parts_metric(node):
    return int(node.query("SELECT value FROM system.metrics WHERE metric = 'ExportTTLPartsHeldByDeleteGate'").strip() or 0)


def try_to_apply_ttl(node, table):
    node.query(f"OPTIMIZE TABLE {table} FINAL")
    node.query(f"ALTER TABLE {table} MATERIALIZE TTL", settings={"mutations_sync": 2})


def wait_for_ttl_applied(node, table, query, expected, timeout=60):
    def applied():
        node.query(f"OPTIMIZE TABLE {table} FINAL")
        return node.query(query) == expected

    wait_until(applied, timeout, f"The TTL was not applied to {table} after the export", interval=1)


def test_delete_waits_for_the_export(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, ", t + INTERVAL 2 DAY DELETE")

    # Stopped moves pause the TTL export.
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2020, {DUE})")
    try_to_apply_ttl(node, mt_table)
    assert node.query(f"SELECT count() FROM {mt_table}") == "2\n", "Rows were deleted before they were exported"

    wait_until(lambda: ttl_rows(node, mt_table).get("2020", {}).get("parts_held_by_delete_gate") == 1, 30,
               "The held part is not shown")
    assert held_parts_metric(node) == 1

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])

    wait_for_ttl_applied(node, mt_table, f"SELECT count() FROM {mt_table}", "0\n")
    wait_until(lambda: held_parts_metric(node) == 0, 30, "The metric still counts held parts")
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_delete_where_waits_for_the_export(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, ", t + INTERVAL 2 DAY DELETE WHERE id % 2 = 0")

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} SELECT number, 2020, {DUE} FROM numbers(4)")
    try_to_apply_ttl(node, mt_table)
    assert node.query(f"SELECT count() FROM {mt_table}") == "4\n"

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    wait_for_ttl_applied(node, mt_table, f"SELECT groupArray(id) FROM (SELECT id FROM {mt_table} ORDER BY id)", "[1,3]\n")
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(4))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_group_by_waits_for_the_export(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, ", t + INTERVAL 2 DAY GROUP BY year SET id = max(id)", order_by="year"
    )

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} SELECT number, 2020, {DUE} FROM numbers(3)")
    try_to_apply_ttl(node, mt_table)
    assert node.query(f"SELECT count() FROM {mt_table}") == "3\n", "Rows were rolled up before they were exported"

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    wait_for_ttl_applied(node, mt_table, f"SELECT groupArray(id) FROM {mt_table}", "[2]\n")
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(3))
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_column_ttl_waits_for_the_export(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, "",
        columns="id Int64, year Int32, t DateTime, v String TTL t + INTERVAL 2 DAY",
        destination_columns="id Int64, year Int32, t DateTime, v String",
    )

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}, 'kept')")
    try_to_apply_ttl(node, mt_table)
    assert node.query(f"SELECT v FROM {mt_table}") == "kept\n", "The column was reset before it was exported"

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    wait_for_ttl_applied(node, mt_table, f"SELECT v FROM {mt_table}", "\n")
    assert node.query(f"SELECT v FROM {iceberg_table}") == "kept\n"
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_removing_the_export_releases_held_rows(cluster, source_engine):
    """Rows held for the export are deleted as soon as the `EXPORT` TTL is removed, without being exported."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, ", t + INTERVAL 2 DAY DELETE")

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    try_to_apply_ttl(node, mt_table)
    assert node.query(f"SELECT count() FROM {mt_table}") == "1\n"

    node.query(f"ALTER TABLE {mt_table} MODIFY TTL t + INTERVAL 2 DAY DELETE", settings={"mutations_sync": 2})
    wait_for_ttl_applied(node, mt_table, f"SELECT count() FROM {mt_table}", "0\n")
    time.sleep(2)
    assert iceberg_ids(node, iceberg_table) == []
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
