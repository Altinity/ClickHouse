"""Integration test for Iceberg bin-packing compaction via OPTIMIZE TABLE.

Creates a table, inserts many small batches to produce many small data files,
runs OPTIMIZE TABLE, and verifies:
- fewer data files after compaction
- same row count and data integrity
- the snapshot has a `replace` operation
"""

import json
import pytest

from helpers.iceberg_utils import (
    create_iceberg_table,
    get_uuid_str,
    default_download_directory,
)


def _count_data_files(instance, table_name):
    """Return the number of DATA files via system.iceberg_files."""
    result = instance.query(
        f"SELECT count() FROM system.iceberg_files "
        f"WHERE database = 'default' AND table = '{table_name}' AND content = 'DATA'"
    ).strip()
    return int(result)


def _get_data_file_sizes(instance, table_name):
    """Return the sorted sizes of live DATA files via system.iceberg_files."""
    result = instance.query(
        f"SELECT file_size_in_bytes FROM system.iceberg_files "
        f"WHERE database = 'default' AND table = '{table_name}' AND content = 'DATA' "
        f"ORDER BY file_size_in_bytes"
    ).strip()
    return [int(line) for line in result.splitlines()] if result else []


def _get_latest_snapshot_summary(instance, table_name):
    """Return the summary of the latest snapshot as a dict."""
    raw = instance.query(
        f"SELECT toJSONString(summary) "
        f"FROM system.iceberg_history "
        f"WHERE database = 'default' AND table = '{table_name}' "
        f"ORDER BY made_current_at DESC LIMIT 1"
    ).strip()
    return json.loads(raw) if raw else {}


@pytest.mark.parametrize("format_version", [2])
def test_bin_pack_rewrite(started_cluster_iceberg_no_spark, format_version):
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_bin_pack_rewrite_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, value String)",
        format_version=format_version,
    )

    # Insert many small batches — each INSERT produces one data file.
    num_batches = 10
    rows_per_batch = 100
    total_rows = num_batches * rows_per_batch

    for batch in range(num_batches):
        values = ", ".join(
            f"({batch * rows_per_batch + i}, 'row_{batch * rows_per_batch + i}')"
            for i in range(rows_per_batch)
        )
        instance.query(
            f"INSERT INTO {table_name} VALUES {values}",
            settings={"allow_insert_into_iceberg": 1},
        )

    # Verify we have many data files.
    files_before = _count_data_files(instance, table_name)
    assert files_before == num_batches, (
        f"Expected {num_batches} data files before compaction, got {files_before}"
    )

    # Verify total row count.
    count_before = int(instance.query(f"SELECT count() FROM {table_name}").strip())
    assert count_before == total_rows

    # Capture data before compaction for integrity check.
    data_before = instance.query(
        f"SELECT id, value FROM {table_name} ORDER BY id"
    ).strip()

    # Run OPTIMIZE TABLE with tiny thresholds so all files are candidates.
    instance.query(
        f"OPTIMIZE TABLE {table_name}",
        settings={
            "allow_experimental_iceberg_compaction": 1,
            "iceberg_target_data_file_size_bytes": 10 * 1024 * 1024,  # 10 MB target
            "iceberg_min_data_file_size_bytes": 10 * 1024 * 1024,      # all files < 10 MB are candidates
        },
    )

    # Drop and recreate the table to pick up the new metadata.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
    )

    # Verify fewer data files.
    files_after = _count_data_files(instance, table_name)
    assert files_after < files_before, (
        f"Expected fewer files after compaction: before={files_before}, after={files_after}"
    )

    # Verify same row count.
    count_after = int(instance.query(f"SELECT count() FROM {table_name}").strip())
    assert count_after == total_rows, (
        f"Row count mismatch: before={total_rows}, after={count_after}"
    )

    # Verify data integrity.
    data_after = instance.query(
        f"SELECT id, value FROM {table_name} ORDER BY id"
    ).strip()
    assert data_after == data_before, "Data mismatch after compaction"

    # Verify the latest snapshot has a `replace` operation.
    summary = _get_latest_snapshot_summary(instance, table_name)
    assert summary.get("operation") == "replace", (
        f"Expected 'replace' operation in snapshot summary, got: {summary.get('operation')}"
    )

    # Verify summary counters.
    assert int(summary.get("added-data-files", 0)) > 0
    assert int(summary.get("deleted-data-files", 0)) == num_batches


@pytest.mark.parametrize("format_version", [2])
def test_bin_pack_noop_when_no_small_files(
    started_cluster_iceberg_no_spark, format_version
):
    """When there are no small files, OPTIMIZE should be a no-op."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_bin_pack_noop_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64)",
        format_version=format_version,
    )

    # Insert a single batch.
    values = ", ".join(f"({i})" for i in range(100))
    instance.query(
        f"INSERT INTO {table_name} VALUES {values}",
        settings={"allow_insert_into_iceberg": 1},
    )

    files_before = _count_data_files(instance, table_name)
    assert files_before == 1

    # OPTIMIZE with a very small threshold — but only 1 file, so nothing to merge.
    instance.query(
        f"OPTIMIZE TABLE {table_name}",
        settings={
            "allow_experimental_iceberg_compaction": 1,
            "iceberg_target_data_file_size_bytes": 1,
            "iceberg_min_data_file_size_bytes": 1024 * 1024 * 1024,  # 1 GB — everything is "small"
        },
    )

    # With only 1 file, there's nothing to bin-pack. File count stays.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
    )

    files_after = _count_data_files(instance, table_name)
    assert files_after == files_before, (
        f"Expected same number of files (nothing to compact): before={files_before}, after={files_after}"
    )


@pytest.mark.parametrize("format_version", [2])
def test_bin_pack_keeps_single_file_last_bin(
    started_cluster_iceberg_no_spark, format_version
):
    """A bin that ends up with a single file must not drop that file from the snapshot."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_bin_pack_single_last_bin_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, value String)",
        format_version=format_version,
    )

    num_batches = 5
    rows_per_batch = 100
    total_rows = num_batches * rows_per_batch

    for batch in range(num_batches):
        values = ", ".join(
            f"({batch * rows_per_batch + i}, 'row_{batch * rows_per_batch + i}')"
            for i in range(rows_per_batch)
        )
        instance.query(
            f"INSERT INTO {table_name} VALUES {values}",
            settings={"allow_insert_into_iceberg": 1},
        )

    files_before = _count_data_files(instance, table_name)
    assert files_before == num_batches

    data_before = instance.query(
        f"SELECT id, value FROM {table_name} ORDER BY id"
    ).strip()

    # Size the target so each bin holds at most 2 files: with 5 files that yields
    # bins of 2, 2 and a leftover single file that must be kept, not dropped.
    sizes = _get_data_file_sizes(instance, table_name)
    assert len(sizes) == num_batches
    target = 2 * sizes[-1] + 1
    assert 3 * sizes[0] > target, "File sizes vary too much for a deterministic bin split"

    instance.query(
        f"OPTIMIZE TABLE {table_name}",
        settings={
            "allow_experimental_iceberg_compaction": 1,
            "iceberg_target_data_file_size_bytes": target,
            "iceberg_min_data_file_size_bytes": target,
        },
    )

    # Drop and recreate the table to pick up the new metadata.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
    )

    # Two merged files plus the kept single file.
    files_after = _count_data_files(instance, table_name)
    assert files_after == 3, (
        f"Expected 3 files after compaction (2 merged + 1 kept), got {files_after}"
    )

    count_after = int(instance.query(f"SELECT count() FROM {table_name}").strip())
    assert count_after == total_rows, (
        f"Row count mismatch: before={total_rows}, after={count_after}"
    )

    data_after = instance.query(
        f"SELECT id, value FROM {table_name} ORDER BY id"
    ).strip()
    assert data_after == data_before, "Data mismatch after compaction"


@pytest.mark.parametrize("format_version", [2])
def test_bin_pack_keeps_single_file_partition(
    started_cluster_iceberg_no_spark, format_version
):
    """A partition with a single small file must keep that file in the new snapshot."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_bin_pack_single_partition_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, part String, value String)",
        format_version=format_version,
        partition_by="part",
    )

    # Partition 'a': three files. Partition 'b': one file.
    for batch in range(3):
        values = ", ".join(
            f"({batch * 100 + i}, 'a', 'row_{batch * 100 + i}')" for i in range(100)
        )
        instance.query(
            f"INSERT INTO {table_name} VALUES {values}",
            settings={"allow_insert_into_iceberg": 1},
        )
    instance.query(
        f"INSERT INTO {table_name} VALUES (10000, 'b', 'row_10000')",
        settings={"allow_insert_into_iceberg": 1},
    )

    files_before = _count_data_files(instance, table_name)
    assert files_before == 4

    data_before = instance.query(
        f"SELECT id, part, value FROM {table_name} ORDER BY id"
    ).strip()

    instance.query(
        f"OPTIMIZE TABLE {table_name}",
        settings={
            "allow_experimental_iceberg_compaction": 1,
            "iceberg_target_data_file_size_bytes": 10 * 1024 * 1024,
            "iceberg_min_data_file_size_bytes": 10 * 1024 * 1024,
        },
    )

    # Drop and recreate the table to pick up the new metadata.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
    )

    # Partition 'a' merged into one file; the single file of partition 'b' is kept.
    files_after = _count_data_files(instance, table_name)
    assert files_after == 2, (
        f"Expected 2 files after compaction (1 merged + 1 kept), got {files_after}"
    )

    count_after = int(instance.query(f"SELECT count() FROM {table_name}").strip())
    assert count_after == 301, f"Row count mismatch: expected 301, got {count_after}"

    data_after = instance.query(
        f"SELECT id, part, value FROM {table_name} ORDER BY id"
    ).strip()
    assert data_after == data_before, "Data mismatch after compaction"


@pytest.mark.parametrize("format_version", [2])
def test_bin_pack_preserves_position_deletes(
    started_cluster_iceberg_no_spark, format_version
):
    """Files referenced by live position deletes must not be rewritten: rewriting changes
    the file path and re-stamps sequence numbers, so carried-forward deletes would stop
    applying and deleted rows would resurface."""
    instance = started_cluster_iceberg_no_spark.instances["node1"]
    table_name = "test_bin_pack_deletes_" + get_uuid_str()

    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
        "(id Int64, value String)",
        format_version=format_version,
    )

    num_batches = 4
    rows_per_batch = 100
    for batch in range(num_batches):
        values = ", ".join(
            f"({batch * rows_per_batch + i}, 'row_{batch * rows_per_batch + i}')"
            for i in range(rows_per_batch)
        )
        instance.query(
            f"INSERT INTO {table_name} VALUES {values}",
            settings={"allow_insert_into_iceberg": 1},
        )

    # Delete rows from the first batch, producing a position-delete file.
    instance.query(
        f"DELETE FROM {table_name} WHERE id < 10",
        settings={"allow_insert_into_iceberg": 1},
    )

    expected_count = num_batches * rows_per_batch - 10
    count_before = int(instance.query(f"SELECT count() FROM {table_name}").strip())
    assert count_before == expected_count

    data_before = instance.query(
        f"SELECT id, value FROM {table_name} ORDER BY id"
    ).strip()

    delete_files_before = int(
        instance.query(
            f"SELECT count() FROM system.iceberg_files "
            f"WHERE database = 'default' AND table = '{table_name}' AND content = 'POSITION_DELETE'"
        ).strip()
    )
    assert delete_files_before > 0, "Expected a position-delete file after DELETE FROM"

    instance.query(
        f"OPTIMIZE TABLE {table_name}",
        settings={
            "allow_experimental_iceberg_compaction": 1,
            "iceberg_target_data_file_size_bytes": 10 * 1024 * 1024,
            "iceberg_min_data_file_size_bytes": 10 * 1024 * 1024,
        },
    )

    # Drop and recreate the table to pick up the new metadata.
    instance.query(f"DROP TABLE IF EXISTS {table_name}")
    create_iceberg_table(
        "local",
        instance,
        table_name,
        started_cluster_iceberg_no_spark,
    )

    # Deleted rows must not resurface and no live row may be lost.
    count_after = int(instance.query(f"SELECT count() FROM {table_name}").strip())
    assert count_after == expected_count, (
        f"Row count mismatch after compaction: expected {expected_count}, got {count_after}"
    )

    data_after = instance.query(
        f"SELECT id, value FROM {table_name} ORDER BY id"
    ).strip()
    assert data_after == data_before, "Data mismatch after compaction"
