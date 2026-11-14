from helpers.export_partition_helpers import unique_suffix

from .common import (
    COLUMNS,
    DUE,
    NOT_DUE,
    assert_exactly_once,
    assert_one_snapshot_per_task,
    create_iceberg,
    create_source,
    create_source_error,
    engine_clause,
    iceberg_ids,
    optimize_partition,
    ttl_rows,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# The `EXPORT` TTL tells the exported rows of a part apart by `_block_number`, which merges keep only
# with `enable_block_number_column`. A part brought in from elsewhere gets a new block number but keeps
# the values it stores, so it is refused if it stores them.


def make_tables(node, engine, settings=None):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"bn_mt_{suffix}", f"bn_iceberg_{suffix}"
    create_iceberg(node, iceberg_table)
    create_source(node, mt_table, COLUMNS, "year", f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}", engine=engine, settings=settings)
    return mt_table, iceberg_table


def create_other(node, engine, enable_block_number_column):
    """A table without TTL whose parts can be brought into a source table."""
    table = f"bn_other_{unique_suffix()}"
    node.query(
        f"CREATE TABLE {table} ({COLUMNS}) ENGINE = {engine_clause(engine, table)}"
        f" PARTITION BY year ORDER BY tuple() SETTINGS enable_block_number_column = {enable_block_number_column}"
    )
    return table


def insert_merged_part(node, table, partition_id, rows):
    for row in rows:
        node.query(f"INSERT INTO {table} VALUES {row}")
    optimize_partition(node, table, partition_id)


def stores_block_numbers(node, table):
    return int(node.query(
        f"SELECT count() FROM system.parts_columns WHERE database = currentDatabase() AND table = '{table}'"
        f" AND active AND column = '_block_number'"
    ).strip()) > 0


def assert_refused(error):
    assert "SUPPORT_IS_DISABLED" in error and "_block_number" in error, error


def test_export_ttl_requires_the_block_number_column(cluster, source_engine):
    node = cluster.instances["replica1"]
    suffix = unique_suffix()
    mt_table, iceberg_table = f"bn_mt_{suffix}", f"bn_iceberg_{suffix}"
    create_iceberg(node, iceberg_table)
    ttl = f"t + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}"
    without_column = {"enable_block_number_column": 0}

    error = create_source_error(node, mt_table, COLUMNS, "year", ttl, engine=source_engine, settings=without_column)
    assert "SUPPORT_IS_DISABLED" in error and "enable_block_number_column" in error, error

    create_source(node, mt_table, COLUMNS, "year", "t + INTERVAL 30 DAY DELETE", engine=source_engine, settings=without_column)
    error = node.query_and_get_error(f"ALTER TABLE {mt_table} MODIFY TTL {ttl}")
    assert "SUPPORT_IS_DISABLED" in error and "enable_block_number_column" in error, error

    node.query(f"ALTER TABLE {mt_table} MODIFY SETTING enable_block_number_column = 1")
    node.query(f"ALTER TABLE {mt_table} MODIFY TTL {ttl}")

    # The default is off, so resetting the setting turns it off too.
    for alter in ["MODIFY SETTING enable_block_number_column = 0", "RESET SETTING enable_block_number_column"]:
        error = node.query_and_get_error(f"ALTER TABLE {mt_table} {alter}")
        assert "SUPPORT_IS_DISABLED" in error and "enable_block_number_column" in error, error
    assert "enable_block_number_column = 1" in node.query(f"SELECT create_table_query FROM system.tables WHERE name = '{mt_table}'")


def test_parts_that_store_block_numbers_are_refused(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    other = create_other(node, source_engine, enable_block_number_column=1)
    insert_merged_part(node, other, "2020", [f"(1, 2020, {DUE})", f"(2, 2020, {DUE})"])
    assert stores_block_numbers(node, other)

    assert_refused(node.query_and_get_error(f"ALTER TABLE {mt_table} ATTACH PARTITION 2020 FROM {other}"))
    assert_refused(node.query_and_get_error(f"ALTER TABLE {mt_table} REPLACE PARTITION 2020 FROM {other}"))
    assert_refused(node.query_and_get_error(f"ALTER TABLE {other} MOVE PARTITION 2020 TO TABLE {mt_table}"))
    assert node.query(f"SELECT count() FROM {mt_table}") == "0\n"
    assert node.query(f"SELECT count() FROM {other}") == "2\n"

    # A part of the table itself gets a new block number when it is attached again.
    insert_merged_part(node, mt_table, "2021", [f"(3, 2021, {NOT_DUE})", f"(4, 2021, {NOT_DUE})"])
    assert stores_block_numbers(node, mt_table)
    node.query(f"ALTER TABLE {mt_table} DETACH PARTITION 2021")
    assert_refused(node.query_and_get_error(f"ALTER TABLE {mt_table} ATTACH PARTITION 2021"))
    assert node.query(f"SELECT count() FROM {mt_table}") == "0\n"
    assert node.query(
        f"SELECT count() FROM system.detached_parts WHERE database = currentDatabase() AND table = '{mt_table}' AND reason = ''"
    ) == "1\n", "The refused part is not left detached as it was"

    assert iceberg_ids(node, iceberg_table) == []


def test_parts_without_stored_block_numbers_are_attached_and_exported(cluster, source_engine):
    """A part that doesn't store `_block_number` reports its new block number for every row."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    other = create_other(node, source_engine, enable_block_number_column=0)
    insert_merged_part(node, other, "2020", [f"(1, 2020, {DUE})", f"(2, 2020, {DUE})"])
    assert not stores_block_numbers(node, other)

    node.query(f"ALTER TABLE {mt_table} ATTACH PARTITION 2020 FROM {other}")
    # The part was written by a table without the `EXPORT` TTL, so it has no TTL info for it.
    row = wait_until(lambda: ttl_rows(node, mt_table).get("2020"), 30, "The partition is not shown")
    assert row["eligible_parts"] == 0, row
    node.query(f"ALTER TABLE {mt_table} MATERIALIZE TTL", settings={"mutations_sync": 2})
    wait_for_partitions_exported(node, mt_table, ["2020"], exported=1)
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    assert_one_snapshot_per_task(node, mt_table, iceberg_table)
