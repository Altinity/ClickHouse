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
