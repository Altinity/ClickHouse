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
# exported, so no row is lost before it reaches the Iceberg destination: also when the deletion is
# due before the export, and when `ttl_only_drop_parts` drops whole parts.


def make_tables(node, engine, extra_ttl, columns=COLUMNS, destination_columns=None, order_by="tuple()", settings=None):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"gate_mt_{suffix}", f"gate_iceberg_{suffix}"
    create_iceberg(node, iceberg_table, columns=destination_columns or columns)
    create_source(
        node, mt_table, columns, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}{extra_ttl}",
        engine=engine, order_by=order_by, settings=settings,
    )
    return mt_table, iceberg_table


def due_in(seconds):
    """A value of `t` that makes the `EXPORT` TTL `t + INTERVAL 1 DAY` due *seconds* after the insert."""
    return f"now() - INTERVAL 1 DAY + INTERVAL {seconds} SECOND"


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


def test_delete_due_before_the_export_waits_for_it(cluster, source_engine):
    """A `DELETE` TTL due before the `EXPORT` TTL waits for the export: the rows are kept until their
    export is due and done, and only then deleted."""
    node = cluster.instances["replica1"]
    export_delay = 60
    mt_table, iceberg_table = make_tables(node, source_engine, ", t + INTERVAL 12 HOUR DELETE")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {due_in(export_delay)}), (2, 2020, {due_in(export_delay)})")
    inserted = time.time()
    try_to_apply_ttl(node, mt_table)
    assert node.query(f"SELECT count() FROM {mt_table}") == "2\n", "Rows were deleted before they were exported"

    wait_until(lambda: ttl_rows(node, mt_table).get("2020", {}).get("parts_held_by_delete_gate") == 1, 30,
               "The held part is not shown")
    assert held_parts_metric(node) == 1
    assert iceberg_ids(node, iceberg_table) == []
    assert time.time() - inserted < export_delay, "The checks took longer than the export takes to be due, so they prove nothing"

    # The part may be dropped by its TTL right after the export, so the destination is waited for.
    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 2], export_delay + 60, "The rows were not exported once due")
    wait_for_ttl_applied(node, mt_table, f"SELECT count() FROM {mt_table}", "0\n")
    wait_until(lambda: held_parts_metric(node) == 0, 30, "The metric still counts held parts")
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)


def test_delete_waits_for_every_block_of_a_merged_part(cluster, source_engine):
    """A part merged from an exported part and a part that is not exported yet is held from the
    `DELETE` TTL, although only rows of the exported block are due for deletion, until its other block
    is exported too."""
    node = cluster.instances["replica1"]
    export_delay = 40
    mt_table, iceberg_table = make_tables(node, source_engine, ", t + INTERVAL 2 DAY DELETE")
    ids = f"SELECT groupArray(id) FROM (SELECT id FROM {mt_table} ORDER BY id)"
    node.query(f"SYSTEM STOP MERGES {mt_table}")
    # A merge would apply the `DELETE` TTL to the exported part before it merges with the other one.
    node.query(f"SYSTEM STOP TTL MERGES {mt_table}")

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2020, {due_in(export_delay)})")
    inserted = time.time()

    node.query(f"SYSTEM START MERGES {mt_table}")
    node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL", settings={"optimize_throw_if_noop": 1})
    active = f"SELECT count() FROM system.parts WHERE database = currentDatabase() AND table = '{mt_table}' AND active"
    assert node.query(active) == "1\n"
    node.query(f"SYSTEM START TTL MERGES {mt_table}")

    try_to_apply_ttl(node, mt_table)
    assert node.query(ids) == "[1,2]\n", "Rows were deleted before every block of their part was exported"
    wait_until(lambda: ttl_rows(node, mt_table).get("2020", {}).get("parts_held_by_delete_gate") == 1, 30,
               "The held part is not shown")
    assert iceberg_ids(node, iceberg_table) == [1]
    assert time.time() - inserted < export_delay, "The checks took longer than the export takes to be due, so they prove nothing"

    wait_until(lambda: iceberg_ids(node, iceberg_table) == [1, 2], export_delay + 60, "The other block was not exported once due")
    wait_for_ttl_applied(node, mt_table, ids, "[2]\n")
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


def ttl_merges(node, table, merge_reason, partition_id=None):
    node.query("SYSTEM FLUSH LOGS part_log")
    condition = f" AND partition_id = '{partition_id}'" if partition_id is not None else ""
    return int(node.query(
        f"SELECT count() FROM system.part_log WHERE database = currentDatabase() AND table = '{table}'"
        f" AND event_type = 'MergeParts' AND merge_reason = '{merge_reason}'{condition}"
    ).strip())


def rows_of_partition(node, table, year):
    return int(node.query(f"SELECT count() FROM {table} WHERE year = {year}").strip())


def test_ttl_only_drop_parts_drops_parts_only_after_the_export(cluster, source_engine):
    """With `ttl_only_drop_parts`, a part whose rows are all due for deletion is dropped whole by a
    TTL merge, but only once it is exported. An exported part with rows not due for deletion yet keeps
    all its rows, since no TTL merge deletes single rows. `OPTIMIZE` without `FINAL` asks for the TTL
    merges that background merges would select."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(
        node, source_engine, ", t + INTERVAL 2 DAY DELETE", settings={"ttl_only_drop_parts": 1}
    )

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2020, {DUE})")
    # Both rows are due for export, only the first for deletion.
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2021, {DUE}), (4, 2021, now() - INTERVAL 1 DAY - INTERVAL 1 HOUR)")

    for _ in range(5):
        node.query(f"OPTIMIZE TABLE {mt_table}")
        time.sleep(1)
    assert node.query(f"SELECT count() FROM {mt_table}") == "4\n", "Rows were deleted before they were exported"
    assert ttl_merges(node, mt_table, "TTLDropMerge") == 0

    def held():
        rows = ttl_rows(node, mt_table)
        return all(rows.get(partition_id, {}).get("parts_held_by_delete_gate") == 1 for partition_id in ["2020", "2021"])

    wait_until(held, 30, "The held parts are not shown")

    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020", "2021"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3, 4])

    def dropped():
        node.query(f"OPTIMIZE TABLE {mt_table}")
        return rows_of_partition(node, mt_table, 2020) == 0

    wait_until(dropped, 60, "The exported part due for deletion was not dropped", interval=1)
    assert ttl_merges(node, mt_table, "TTLDropMerge", "2020") >= 1

    for _ in range(3):
        node.query(f"OPTIMIZE TABLE {mt_table}")
        time.sleep(1)
    assert rows_of_partition(node, mt_table, 2021) == 2, "Rows of a part not due for deletion as a whole were deleted"
    assert ttl_merges(node, mt_table, "TTLDeleteMerge") == 0
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 3, 4])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
