from typing import NamedTuple, Optional

import pytest

from helpers.export_partition_helpers import unique_suffix

from .common import (
    DUE,
    commit_files,
    create_s3_hive,
    create_s3_wildcard,
    create_source,
    create_source_error,
    s3_files,
    ttl_tasks,
    wait_for_last_error,
    wait_for_partitions_exported,
)

CLUSTER_INSTANCES = ["replica1"]

# The partition key of an object storage destination against the partition key of the source.
#
# - Structural: the destination key is a function of the source key, so every group lands in one
#   destination partition.
# - Dynamic: the destination key is monotonic in one column of the source key, so it is checked for
#   each group, from the min/max values of its parts.
# - Rejected: neither can hold, so the `CREATE` or `ALTER` that adds the expression is refused.


class Case(NamedTuple):
    columns: str
    source_partition_by: str
    destination_partition_by: str
    wildcard: bool
    # One insert per list, one part per source partition.
    inserts: list
    # Ids per destination directory, and the directory names when they are known.
    expected_files: list
    expected_directories: Optional[list] = None
    ttl_column: str = "t"


STRUCTURAL_CASES = [
    pytest.param(Case(
        "id UInt64, year UInt16, t DateTime", "year", "year", False,
        [f"(1, 2020, {DUE}), (2, 2021, {DUE})"],
        [[1], [2]], ["year=2020", "year=2021"],
    ), id="identity"),
    pytest.param(Case(
        "id UInt64, t DateTime", "toYYYYMM(t)", "toYYYYMM(t)", True,
        ["(1, '2020-01-05 10:00:00'), (2, '2020-02-05 10:00:00'), (3, '2020-02-06 10:00:00')"],
        [[1], [2, 3]], ["202001", "202002"],
    ), id="same_expression"),
    pytest.param(Case(
        "id UInt64, region String, t DateTime", "(region, toYYYYMM(t))", "region", False,
        ["(1, 'eu', '2020-01-05 10:00:00'), (2, 'eu', '2020-02-05 10:00:00'), (3, 'us', '2020-01-05 10:00:00')"],
        [[1, 2], [3]], ["region=eu", "region=us"],
    ), id="subset_of_a_tuple"),
    pytest.param(Case(
        "id UInt64, a UInt8, b UInt8, t DateTime", "(a, b)", "(b, a)", False,
        [f"(1, 1, 2, {DUE}), (2, 1, 3, {DUE})"],
        [[1], [2]], ["b=2/a=1", "b=3/a=1"],
    ), id="reordered_tuple"),
    pytest.param(Case(
        "id UInt64, year UInt16, t DateTime", "year", "year % 10", True,
        [f"(1, 2020, {DUE}), (2, 2021, {DUE}), (3, 2031, {DUE})"],
        [[1], [2, 3]], ["0", "1"],
    ), id="function_of_the_key"),
    pytest.param(Case(
        "id UInt64, year UInt16, t DateTime", "year", "cityHash64(year) % 4", True,
        [f"(1, 2020, {DUE}), (2, 2021, {DUE}), (3, 2022, {DUE}), (4, 2023, {DUE})"],
        None,
    ), id="hash_of_the_key"),
    pytest.param(Case(
        "id UInt64, year UInt16, t DateTime", "toString(year)", "year", False,
        [f"(1, 2020, {DUE}), (2, 2021, {DUE})"],
        [[1], [2]], ["year=2020", "year=2021"],
    ), id="injective_wrapper"),
    pytest.param(Case(
        "id UInt64, t DateTime", "toYYYYMM(t)", "toYear(t)", True,
        ["(1, '2020-01-05 10:00:00'), (2, '2020-02-05 10:00:00'), (3, '2021-03-05 10:00:00')"],
        [[1, 2], [3]], ["2020", "2021"],
    ), id="dynamic_month_into_year"),
]


def make_tables(node, case, engine):
    suffix = unique_suffix()
    mt_table, s3_table = f"pkey_mt_{suffix}", f"pkey_s3_{suffix}"
    if case.wildcard:
        create_s3_wildcard(node, s3_table, case.columns, case.destination_partition_by)
    else:
        create_s3_hive(node, s3_table, case.columns, case.destination_partition_by)
    create_source(
        node, mt_table, case.columns, case.source_partition_by,
        f"{case.ttl_column} + INTERVAL 1 DAY EXPORT TO TABLE {s3_table}", engine=engine,
    )
    return mt_table, s3_table


def source_partitions(node, table):
    return node.query(
        f"SELECT DISTINCT partition_id FROM system.parts WHERE database = currentDatabase() AND table = '{table}' AND active"
    ).split()


@pytest.mark.parametrize("case", STRUCTURAL_CASES)
def test_every_group_lands_in_one_destination_partition(cluster, source_engine, case):
    node = cluster.instances["replica1"]
    mt_table, s3_table = make_tables(node, case, source_engine)
    for values in case.inserts:
        node.query(f"INSERT INTO {mt_table} VALUES {values}")

    wait_for_partitions_exported(node, mt_table, source_partitions(node, mt_table))
    files = s3_files(node, s3_table)

    if case.expected_files is None:
        expected = {}
        for row_id, year in [(1, 2020), (2, 2021), (3, 2022), (4, 2023)]:
            directory = node.query(f"SELECT cityHash64(toUInt16({year})) % 4").strip()
            expected.setdefault(directory, []).append(row_id)
        assert files == expected, files
    else:
        assert sorted(files.values()) == sorted(case.expected_files), files
        if case.expected_directories is not None:
            assert sorted(files) == sorted(case.expected_directories), files
    tasks = ttl_tasks(node, mt_table)
    assert all(task["status"] == "COMPLETED" for task in tasks)
    # Every task committed with a commit file in the directory of the table, also for wildcards.
    assert commit_files(node, s3_table) == len(tasks)


def test_group_on_one_day_is_exported_to_a_daily_destination(cluster, source_engine):
    """A monthly source into a daily destination: a group whose rows are on one day is exported, a
    group spanning two days fails its check and is not exported."""
    node = cluster.instances["replica1"]
    case = Case("id UInt64, d Date", "toYYYYMM(d)", "d", False, [], [], ttl_column="d")
    mt_table, s3_table = make_tables(node, case, source_engine)

    node.query(f"INSERT INTO {mt_table} VALUES (1, '2020-01-01'), (2, '2020-01-01')")
    node.query(f"INSERT INTO {mt_table} VALUES (3, '2020-02-01'), (4, '2020-02-02')")

    wait_for_partitions_exported(node, mt_table, ["202001"])
    row = wait_for_last_error(node, mt_table, "202002", "multiple destination partitions")
    assert row["eligible_parts"] == 1 and row["claimed_parts"] == 0, row
    assert s3_files(node, s3_table) == {"d=2020-01-01": [1, 2]}
    assert s3_files(node, s3_table, committed_only=False) == {"d=2020-01-01": [1, 2]}, "A failed group wrote files"


def test_integer_ranges_are_checked_per_group(cluster, source_engine):
    node = cluster.instances["replica1"]
    case = Case("id UInt64, t DateTime", "intDiv(id, 1000)", "intDiv(id, 100)", True, [], [])
    mt_table, s3_table = make_tables(node, case, source_engine)

    node.query(f"INSERT INTO {mt_table} VALUES (1000, {DUE}), (1050, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (2000, {DUE}), (2150, {DUE})")

    wait_for_partitions_exported(node, mt_table, ["1"])
    wait_for_last_error(node, mt_table, "2", "multiple destination partitions")
    assert s3_files(node, s3_table) == {"10": [1000, 1050]}


def test_cast_of_the_partition_column_is_checked_per_group(cluster, source_engine):
    """The partition column is a `UInt32` in the source and a `UInt64` in the destination: the cast is
    monotonic, so the values of a group are cast and checked like the others."""
    node = cluster.instances["replica1"]
    suffix = unique_suffix()
    mt_table, s3_table = f"pkey_mt_{suffix}", f"pkey_s3_{suffix}"
    create_s3_wildcard(node, s3_table, "id UInt64, k UInt64, t DateTime", "intDiv(k, 100)")
    create_source(
        node, mt_table, "id UInt64, k UInt32, t DateTime", "intDiv(k, 1000)",
        f"t + INTERVAL 1 DAY EXPORT TO TABLE {s3_table}", engine=source_engine,
    )

    node.query(f"INSERT INTO {mt_table} VALUES (1, 1000, {DUE}), (2, 1050, {DUE})")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2000, {DUE}), (4, 2150, {DUE})")

    wait_for_partitions_exported(node, mt_table, ["1"])
    wait_for_last_error(node, mt_table, "2", "multiple destination partitions")
    assert s3_files(node, s3_table) == {"10": [1, 2]}


REJECTED_CASES = [
    pytest.param(
        ("id UInt64, year UInt16, t DateTime", "year", "id", False, None),
        "not part of the source MergeTree partition key", id="column_outside_the_key",
    ),
    pytest.param(
        ("id UInt64, t DateTime", "toYYYYMM(t)", "toDayOfWeek(t)", True, None),
        "is not monotonic", id="not_monotonic",
    ),
    pytest.param(
        ("id UInt64, k Nullable(UInt32), t DateTime", "intDiv(k, 100)", "intDiv(k, 10)", True, {"allow_nullable_key": 1}),
        "Nullable", id="nullable_column",
    ),
    pytest.param(
        ("id UInt64, year UInt16, t DateTime", "", "year", False, None),
        "not part of the source MergeTree partition key", id="unpartitioned_source",
    ),
]


@pytest.mark.parametrize("definition, message", REJECTED_CASES)
def test_incompatible_partition_key_is_refused(cluster, source_engine, definition, message):
    node = cluster.instances["replica1"]
    columns, source_partition_by, destination_partition_by, wildcard, settings = definition
    suffix = unique_suffix()
    mt_table, s3_table = f"pkey_mt_{suffix}", f"pkey_s3_{suffix}"
    if wildcard:
        create_s3_wildcard(node, s3_table, columns, destination_partition_by)
    else:
        create_s3_hive(node, s3_table, columns, destination_partition_by)

    error = create_source_error(
        node, mt_table, columns, source_partition_by, f"t + INTERVAL 1 DAY EXPORT TO TABLE {s3_table}",
        engine=source_engine, settings=settings,
    )
    assert "BAD_ARGUMENTS" in error and message in error, error

    # The same through `ALTER`.
    create_source(node, mt_table, columns, source_partition_by, "t + INTERVAL 30 DAY DELETE", engine=source_engine, settings=settings)
    error = node.query_and_get_error(f"ALTER TABLE {mt_table} MODIFY TTL t + INTERVAL 1 DAY EXPORT TO TABLE {s3_table}")
    assert "BAD_ARGUMENTS" in error and message in error, error
    assert "EXPORT TO TABLE" not in node.query(f"SELECT create_table_query FROM system.tables WHERE name = '{mt_table}'")
