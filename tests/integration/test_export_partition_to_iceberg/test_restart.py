from helpers.export_partition_helpers import (
    make_iceberg_s3,
    make_source,
    unique_suffix,
    wait_for_exception_count,
    wait_for_export_status,
)
from helpers.iceberg_export_stats import fetch_manifest_entries

from .common import (
    data_file_partition_records,
    partition_scalar,
)

CLUSTER_INSTANCES = ["replica1"]

# A pending `EXPORT PARTITION` must keep its source parts on disk across a restart, even when they
# were merged away before it. Such parts are Outdated parts, which are loaded asynchronously after
# startup, so the export machinery can only re-pin them once that loading finishes.
#
# `sleep_before_loading_outdated_parts_ms` makes the first export tick after the restart run before
# the Outdated parts are loaded; `old_parts_lifetime` and the cleanup delays make an unpinned
# Outdated part removed as soon as it is loaded.
RESTART_SOURCE_SETTINGS = (
    "old_parts_lifetime = 1, "
    "sleep_before_loading_outdated_parts_ms = 10000, "
    "cleanup_delay_period = 1, "
    "max_cleanup_delay_period = 1, "
    "cleanup_delay_period_random_add = 0"
)


def test_export_resumes_after_restart_with_merged_source_parts(cluster, source_engine):
    """The source parts are merged away while their export keeps failing with a retryable error,
    then the server restarts. The export must read the merged-away (Outdated) parts once they are
    loaded and complete, instead of failing or losing them to the cleanup thread."""
    node = cluster.instances["replica1"]
    uid = unique_suffix()
    mt_table = f"mt_restart_export_{uid}"
    iceberg_table = f"iceberg_restart_export_{uid}"

    make_source(
        node, mt_table, "id Int64, year Int32", "year",
        engine=source_engine, replica_name="replica1", extra_settings=RESTART_SOURCE_SETTINGS,
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, 2020), (2, 2020)")
    node.query(f"INSERT INTO {mt_table} VALUES (3, 2020)")
    make_iceberg_s3(node, iceberg_table, "id Int64, year Int32", partition_by="year")

    node.query("SYSTEM ENABLE FAILPOINT export_part_retryable_throw")
    try:
        node.query(
            f"ALTER TABLE {mt_table} EXPORT PARTITION ID '2020' TO TABLE {iceberg_table}"
            f" SETTINGS export_merge_tree_partition_retry_initial_backoff_seconds = 1,"
            f"          export_merge_tree_partition_retry_max_backoff_seconds = 2,"
            f"          allow_insert_into_iceberg = 1"
        )
        wait_for_exception_count(node, mt_table, iceberg_table, "2020", min_exception_count=1, timeout=90)

        # Both source parts become Outdated; only the in-memory pins keep them on disk until the restart.
        node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '2020' FINAL")

        # The restart also clears the failpoint.
        node.restart_clickhouse()
    finally:
        node.query("SYSTEM DISABLE FAILPOINT export_part_retryable_throw")

    wait_for_export_status(node, mt_table, iceberg_table, "2020", "COMPLETED", timeout=180)

    assert int(node.query(f"SELECT count() FROM {iceberg_table}").strip()) == 3, (
        "All three rows of the merged-away source parts must be exported."
    )


def test_export_commit_survives_restart_after_merge(cluster):
    """Every part is exported but the commit keeps failing; the exported part is merged away and the
    server restarts. The deferred commit derives the Iceberg partition value from the exported part,
    so that part must survive the restart and the commit must stamp the exported day.

    Replicated-only: the commit failpoint only exists on the ZooKeeper-coordinated commit path, see
    `test_export_partition_commit_uses_exported_parts_not_new_inserts` in `test_failures.py`."""
    node = cluster.instances["replica1"]
    uid = unique_suffix()
    mt_table = f"mt_restart_commit_{uid}"
    iceberg_table = f"iceberg_restart_commit_{uid}"

    make_source(
        node, mt_table, "id Int64, event_date Date", "toYYYYMM(event_date)",
        replica_name="replica1", extra_settings=RESTART_SOURCE_SETTINGS,
    )
    node.query(f"INSERT INTO {mt_table} VALUES (1, '2024-03-20'), (2, '2024-03-20')")
    make_iceberg_s3(node, iceberg_table, "id Int64, event_date Date",
                    partition_by="toRelativeDayNum(event_date)")

    exported_day = int(node.query("SELECT toRelativeDayNum(toDate('2024-03-20'))").strip())
    injected_day = int(node.query("SELECT toRelativeDayNum(toDate('2024-03-05'))").strip())

    node.query("SYSTEM ENABLE FAILPOINT export_partition_commit_always_throw")
    try:
        node.query(
            f"ALTER TABLE {mt_table} EXPORT PARTITION ID '202403' TO TABLE {iceberg_table}"
            f" SETTINGS allow_insert_into_iceberg = 1"
        )
        # The commit is attempted only after every part is exported, so a non-zero exception count
        # means the data files are written and the commit is now wedged by the failpoint.
        wait_for_exception_count(node, mt_table, iceberg_table, "202403", min_exception_count=1, timeout=90)

        # The merged active part spans both days with min = the injected (earlier) day, while the
        # exported part becomes Outdated.
        node.query(f"INSERT INTO {mt_table} VALUES (3, '2024-03-05')")
        node.query(f"OPTIMIZE TABLE {mt_table} PARTITION ID '202403' FINAL")

        # The restart also clears the failpoint.
        node.restart_clickhouse()
    finally:
        node.query("SYSTEM DISABLE FAILPOINT export_partition_commit_always_throw")

    wait_for_export_status(node, mt_table, iceberg_table, "202403", "COMPLETED", timeout=180)

    query_id = f"restart_commit_{uid}"
    node.query(
        f"SELECT * FROM {iceberg_table}",
        query_id=query_id,
        settings={"iceberg_metadata_log_level": "manifest_file_entry"},
    )
    entries = fetch_manifest_entries(node, query_id)
    partitions = data_file_partition_records(entries)
    assert partitions, "No data-file partition records found in manifest entries"
    meta_days = {int(partition_scalar(p, "event_date")) for p in partitions}
    assert meta_days == {exported_day}, (
        f"Metadata day {meta_days} must equal the exported day {exported_day} (2024-03-20), "
        f"not the injected day {injected_day} (2024-03-05)."
    )

    assert int(node.query(f"SELECT count() FROM {iceberg_table}").strip()) == 2, (
        "Only the two exported rows must be present in the destination."
    )
