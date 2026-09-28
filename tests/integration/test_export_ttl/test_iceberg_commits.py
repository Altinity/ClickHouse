import time

from helpers.export_partition_helpers import make_iceberg_s3, unique_suffix

from .common import (
    assert_exactly_once,
    completed_ttl_tasks,
    create_source,
    iceberg_ids,
    iceberg_snapshots,
    pending_ttl_tasks,
    ttl_rows,
    ttl_tasks,
    wait_for_partitions_exported,
    wait_until,
)

CLUSTER_INSTANCES = ["replica1"]

# Every group of the `EXPORT` TTL is committed to an Iceberg destination as one transaction, i.e.
# one snapshot, and no row is committed twice.

# An Iceberg `date` is read back as `Date32`, so the source uses it as well.
COLUMNS = "id Int32, year Int32, d Date32"
DUE = "today() - 10"


def make_tables(node, engine, settings=None, columns=COLUMNS):
    suffix = unique_suffix()
    mt_table, iceberg_table = f"commit_mt_{suffix}", f"commit_iceberg_{suffix}"
    make_iceberg_s3(node, iceberg_table, columns, partition_by="year")
    create_source(
        node, mt_table, columns, "year", f"toDate(d) + INTERVAL 1 DAY EXPORT TO TABLE {iceberg_table}",
        engine=engine, settings=settings,
    )
    return mt_table, iceberg_table


def test_due_parts_are_exported_once(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)

    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE}), (2, 2020, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2021, today())")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert node.query(f"SELECT id, year FROM {iceberg_table} ORDER BY id") == "1\t2020\n2\t2020\n"

    node.query(f"INSERT INTO {mt_table} VALUES (4, 2020, {DUE})")
    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 2, 60, "The second part was not exported")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2, 4])
    assert node.query(f"SELECT count() FROM {iceberg_table} WHERE year = 2021") == "0\n"


def test_group_is_one_snapshot(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    snapshots = iceberg_snapshots(node, iceberg_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    for i in range(3):
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")
    node.query(f"SYSTEM START MOVES {mt_table}")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    tasks = ttl_tasks(node, mt_table)
    assert len(tasks) == 1 and len(tasks[0]["parts"]) == 3, tasks
    assert iceberg_snapshots(node, iceberg_table) == snapshots + 1
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(3))


def test_groups_limited_in_size_are_separate_snapshots(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_max_parts_per_group": 2})
    snapshots = iceberg_snapshots(node, iceberg_table)

    node.query(f"SYSTEM STOP MERGES {mt_table}")
    node.query(f"SYSTEM STOP MOVES {mt_table}")
    for i in range(5):
        node.query(f"INSERT INTO {mt_table} VALUES ({i}, 2020, {DUE})")
    node.query(f"SYSTEM START MOVES {mt_table}")

    wait_until(lambda: len(completed_ttl_tasks(node, mt_table)) == 3, 120, "The partition was not exported in three groups")
    wait_for_partitions_exported(node, mt_table, ["2020"])
    assert sorted(len(task["parts"]) for task in ttl_tasks(node, mt_table)) == [1, 2, 2]
    assert iceberg_snapshots(node, iceberg_table) == snapshots + 3
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(5))


def test_concurrent_groups_commit_to_one_table(cluster, source_engine):
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine, settings={"ttl_export_max_concurrent_groups": 4})
    snapshots = iceberg_snapshots(node, iceberg_table)

    node.query(f"SYSTEM STOP MOVES {mt_table}")
    node.query(f"INSERT INTO {mt_table} SELECT number, 2020 + number % 4, {DUE} FROM numbers(40)")
    node.query(f"SYSTEM START MOVES {mt_table}")

    in_flight = []
    def settled():
        in_flight.append(pending_ttl_tasks(node, mt_table))
        return len(completed_ttl_tasks(node, mt_table)) == 4
    wait_until(settled, 120, "The four partitions were not exported")
    wait_for_partitions_exported(node, mt_table, ["2020", "2021", "2022", "2023"])

    assert iceberg_snapshots(node, iceberg_table) == snapshots + 4
    assert_exactly_once(iceberg_ids(node, iceberg_table), range(40))
    assert node.query(f"SELECT year, count() FROM {iceberg_table} GROUP BY year ORDER BY year") == "".join(
        f"{year}\t10\n" for year in range(2020, 2024)
    )


def test_destination_schema_change_between_groups(cluster, source_engine):
    """A column added to the destination after a group was exported: the next groups are checked
    against the new schema, and either export or report why."""
    node = cluster.instances["replica1"]
    mt_table, iceberg_table = make_tables(node, source_engine)
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020, {DUE})")
    wait_for_partitions_exported(node, mt_table, ["2020"])

    node.query(f"ALTER TABLE {iceberg_table} ADD COLUMN extra Nullable(String)", settings={"allow_insert_into_iceberg": 1})
    node.query(f"INSERT INTO {mt_table} VALUES (2, 2021, {DUE})")

    def outcome():
        row = ttl_rows(node, mt_table).get("2021")
        if row and row["last_error"]:
            return "error"
        if completed_ttl_tasks(node, mt_table) and len(completed_ttl_tasks(node, mt_table)) == 2:
            return "exported"
        return None

    result = wait_until(outcome, 60, "The group after the schema change neither exported nor failed")
    time.sleep(5)
    tasks = ttl_tasks(node, mt_table)
    if result == "exported":
        assert_exactly_once(iceberg_ids(node, iceberg_table), [1, 2])
    else:
        assert_exactly_once(iceberg_ids(node, iceberg_table), [1])
    # A group that cannot be exported must not start a new task on every check.
    assert len(tasks) <= 3, f"The failing group started {len(tasks)} tasks: {tasks}"
