"""
Shared helpers for export-partition and export-part integration tests.

Centralises wait-polling, table creation, and partition helpers that were
previously duplicated across multiple test modules.
"""

import time
import uuid
from typing import NamedTuple

import pytest


MINIO_USER = "minio"
MINIO_PASS = "ClickHouse_Minio_P@ssw0rd"

# The `EXPORT PARTITION` implementation is shared between the two MergeTree flavours, so the
# scenarios that do not depend on cross-replica coordination run once per engine. Suites expose
# this through the `source_engine` fixture; a test that requests the fixture runs twice.
SOURCE_ENGINES = ["MergeTree", "ReplicatedMergeTree"]
SOURCE_ENGINE_IDS = ["mt", "rmt"]


def is_replicated_engine(engine):
    return engine == "ReplicatedMergeTree"


def skip_if_remote_database_disk_enabled(cluster):
    """Skip the test if any instance in the cluster has remote database disk enabled.

    Tests that block MinIO cannot run when remote database disk is enabled, as the database
    metadata is stored on MinIO and blocking it would break the database.
    """
    for instance in cluster.instances.values():
        if instance.with_remote_database_disk:
            pytest.skip(
                "Test cannot run with remote database disk enabled (db disk), as it blocks MinIO which stores database metadata"
            )


# Every `EXPORT PARTITION` creates a new task, so a partition may have several. The newest one is
# the one a test just started; of tasks created in the same second, a pending one is preferred so
# that an older finished task does not satisfy a wait.
_NEWEST_TASK = "ORDER BY create_time DESC, status = 'PENDING' DESC LIMIT 1"


def _task_filter(source_table, dest_table, partition_id):
    dest_filter = f" AND destination_table = '{dest_table}'" if dest_table else ""
    return f"source_table = '{source_table}'{dest_filter} AND partition_id = '{partition_id}'"


def wait_for_export_status(
    node,
    source_table,
    dest_table,
    partition_id,
    expected_status="COMPLETED",
    timeout=60,
    poll_interval=0.5,
):
    """Poll `system.distributed_exports` until the status of the newest task of the partition matches.

    *dest_table* may be ``None`` to skip filtering by destination table
    (useful for catalog-based tests where the destination is a database-qualified path).
    """
    start_time = time.time()
    last_status = None
    while time.time() - start_time < timeout:
        status = node.query(
            f"SELECT status FROM system.distributed_exports"
            f" WHERE {_task_filter(source_table, dest_table, partition_id)}"
            f" {_NEWEST_TASK}"
        ).strip()

        last_status = status
        if status and status == expected_status:
            return status

        time.sleep(poll_interval)

    raise TimeoutError(
        f"Export status did not reach '{expected_status}' within {timeout}s. "
        f"Last status: '{last_status}'"
    )


def newest_active_part(node, table, partition_id):
    """Name of the active part of *partition_id* with the highest block number."""
    return node.query(
        f"SELECT name FROM system.parts"
        f" WHERE database = currentDatabase() AND table = '{table}'"
        f" AND partition_id = '{partition_id}' AND active"
        f" ORDER BY max_block_number DESC LIMIT 1"
    ).strip()


def assert_parts_not_merged_across_export_states(node, table, partition_id, part_names):
    """Trigger merges of *partition_id* and check that none of *part_names* was merged. Each of them
    must be in a different export state (exported, claimed, not exported) than its neighbours, or
    be claimed by an export task of the `EXPORT` TTL.

    Both merge paths are tried: `OPTIMIZE FINAL` merges the whole partition or nothing, a plain
    `OPTIMIZE` runs the selector background merges use and may still merge parts in the same state
    with each other, which is allowed.
    """
    error = node.query_and_get_error(
        f"OPTIMIZE TABLE {table} PARTITION ID '{partition_id}' FINAL",
        settings={"optimize_throw_if_noop": 1},
    )
    assert "export states" in error or "are being exported" in error, f"Unexpected error on {node.name}: {error}"

    node.query(f"OPTIMIZE TABLE {table} PARTITION ID '{partition_id}'")

    active_parts = node.query(
        f"SELECT name FROM system.parts"
        f" WHERE database = currentDatabase() AND table = '{table}'"
        f" AND partition_id = '{partition_id}' AND active"
    ).split()
    merged = [name for name in part_names if name not in active_parts]
    assert not merged, (
        f"Parts {merged} were merged across export states on {node.name}; active parts: {active_parts}"
    )


def export_transaction_id(
    node,
    source_table,
    dest_table,
    partition_id,
):
    """Return the transaction id of the newest export task of a partition, or an empty string if none."""
    return node.query(
        f"SELECT transaction_id FROM system.distributed_exports"
        f" WHERE {_task_filter(source_table, dest_table, partition_id)}"
        f" {_NEWEST_TASK}"
    ).strip()


def wait_for_new_export_transaction(
    node,
    source_table,
    dest_table,
    partition_id,
    previous_transaction_id,
    timeout=60,
    poll_interval=0.2,
):
    """Wait until the newest export task of the partition is not *previous_transaction_id*.

    A re-export creates a new task. Without this wait, the COMPLETED status of the previous task
    can still be the newest one visible in the in-memory mirror and satisfy a status wait
    immediately, before the new export has even started.
    """
    start_time = time.time()
    last_transaction_id = None
    while time.time() - start_time < timeout:
        last_transaction_id = export_transaction_id(
            node, source_table, dest_table, partition_id
        )
        if last_transaction_id and last_transaction_id != previous_transaction_id:
            return last_transaction_id
        time.sleep(poll_interval)

    raise TimeoutError(
        f"Export transaction id did not change from {previous_transaction_id!r} within {timeout}s. "
        f"Last seen: {last_transaction_id!r}"
    )


def commit_marker_lines(node, source_table, dest_table, partition_id):
    """Number of files committed to the plain object storage destination *dest_table* by the export
    tasks of *partition_id*: each task commits with a marker `commit_<transaction_id>` that lists them.
    """
    transaction_ids = node.query(
        f"SELECT transaction_id FROM system.distributed_exports WHERE {_task_filter(source_table, dest_table, partition_id)}"
    ).split()

    return sum(
        int(node.query(f"SELECT count() FROM s3(s3_conn, filename='{dest_table}/commit_{transaction_id}*', format=LineAsString)"))
        for transaction_id in transaction_ids
    )


def wait_for_export_to_start(
    node,
    source_table,
    dest_table,
    partition_id,
    timeout=10,
    poll_interval=0.2,
):
    """Poll until at least one row exists in `system.distributed_exports`."""
    start_time = time.time()
    while time.time() - start_time < timeout:
        count = node.query(
            f"SELECT count() FROM system.distributed_exports"
            f" WHERE source_table = '{source_table}'"
            f"   AND destination_table = '{dest_table}'"
            f"   AND partition_id = '{partition_id}'"
        ).strip()

        if count != "0":
            return True

        time.sleep(poll_interval)

    raise TimeoutError(
        f"Export of partition {partition_id!r} did not start within {timeout}s."
    )


def wait_for_exception_count(
    node,
    source_table,
    dest_table,
    partition_id,
    min_exception_count=1,
    timeout=60,
    poll_interval=0.5,
):
    """Wait for exception_count of the newest task of the partition to reach at least *min_exception_count*.

    The default timeout is intentionally larger than one manifest-updater poll
    cycle (~30s, see StorageReplicatedMergeTree::exportTaskUpdatingTask).
    For a ReplicatedMergeTree source, `system.distributed_exports` is served from the
    in-memory mirror, which is refreshed on (a) the periodic poll tick and (b)
    status changes. While the task is still PENDING (e.g. transient part-export
    failures with a generous max_retries), no status watch fires, so newly written
    per-replica exception leaves only become visible on the next poll. Allow at
    least one full cycle plus headroom so the test is not racing the cadence.
    """
    start_time = time.time()
    last_exception_count = None
    while time.time() - start_time < timeout:
        exception_count_str = node.query(
            f"SELECT exception_count FROM system.distributed_exports"
            f" WHERE {_task_filter(source_table, dest_table, partition_id)}"
            f" {_NEWEST_TASK}"
        ).strip()

        if exception_count_str:
            exception_count = int(exception_count_str)
            last_exception_count = exception_count
            if exception_count >= min_exception_count:
                return exception_count

        time.sleep(poll_interval)

    raise TimeoutError(
        f"Exception count did not reach {min_exception_count} within {timeout}s. "
        f"Last exception_count: {last_exception_count if last_exception_count is not None else 'N/A'}"
    )


# -- block-number settings are needed for patch parts support
_BLOCK_SETTINGS = (
    "enable_block_number_column = 1, enable_block_offset_column = 1"
)


def make_rmt(
    node,
    name,
    columns,
    partition_by,
    replica_name="r1",
    order_by="tuple()",
    extra_settings="",
):
    """Create a ReplicatedMergeTree table with block-number settings."""
    settings = f"{_BLOCK_SETTINGS}, {extra_settings}" if extra_settings else _BLOCK_SETTINGS
    node.query(
        f"""
        CREATE TABLE {name} ({columns})
        ENGINE = ReplicatedMergeTree('/clickhouse/tables/{name}', '{replica_name}')
        PARTITION BY {partition_by}
        ORDER BY {order_by}
        SETTINGS {settings}
        """
    )


def make_mt(
    node,
    name,
    columns,
    partition_by,
    order_by="tuple()",
    extra_settings="",
):
    """Create a MergeTree table with block-number settings."""
    settings = f"{_BLOCK_SETTINGS}, {extra_settings}" if extra_settings else _BLOCK_SETTINGS
    node.query(
        f"""
        CREATE TABLE {name} ({columns})
        ENGINE = MergeTree()
        PARTITION BY {partition_by}
        ORDER BY {order_by}
        SETTINGS {settings}
        """
    )


def make_source(
    node,
    name,
    columns,
    partition_by,
    engine="ReplicatedMergeTree",
    order_by="tuple()",
    replica_name="r1",
    extra_settings="",
):
    """Create an export source table of the given MergeTree flavour.

    *replica_name* is ignored for a plain MergeTree, which has no replicas.
    """
    if is_replicated_engine(engine):
        make_rmt(
            node,
            name,
            columns,
            partition_by,
            replica_name=replica_name,
            order_by=order_by,
            extra_settings=extra_settings,
        )
    else:
        make_mt(
            node,
            name,
            columns,
            partition_by,
            order_by=order_by,
            extra_settings=extra_settings,
        )


def setup_source_tables(
    nodes,
    name,
    columns,
    partition_by,
    engine,
    insert_values=None,
    order_by="tuple()",
    extra_settings="",
    replica_names=None,
):
    """Create the export source table on the nodes that can host it and insert the initial data.

    A `ReplicatedMergeTree` source is created on every node, and the insert made on the first
    node is synced to the others. A plain `MergeTree` only exists on the first node, which is
    the only node that can drive its export, so there is nothing to create or sync elsewhere.

    This is the single place that branches on the engine, so the tests themselves stay free of
    engine conditionals. Returns the nodes hosting the source table.
    """
    nodes = list(nodes)
    hosts = nodes if is_replicated_engine(engine) else nodes[:1]

    if replica_names is None:
        replica_names = [node.name for node in hosts]

    for node, replica_name in zip(hosts, replica_names):
        make_source(
            node,
            name,
            columns,
            partition_by,
            engine=engine,
            order_by=order_by,
            replica_name=replica_name,
            extra_settings=extra_settings,
        )

    if insert_values:
        hosts[0].query(f"INSERT INTO {name} VALUES {insert_values}")
        for node in hosts[1:]:
            node.query(f"SYSTEM SYNC REPLICA {name}")

    return hosts


def make_iceberg_s3(
    node,
    name,
    columns,
    partition_by="",
    url=None,
    s3_retry_attempts=3,
    if_not_exists=False,
):
    """Create an IcebergS3 table at a MinIO prefix.

    *url* defaults to ``http://minio1:9001/root/data/{name}/``.
    """
    if url is None:
        url = f"http://minio1:9001/root/data/{name}/"
    ine = "IF NOT EXISTS " if if_not_exists else ""
    pclause = f"PARTITION BY {partition_by}" if partition_by else ""
    node.query(
        f"""
        CREATE TABLE {ine}{name} ({columns})
        ENGINE = IcebergS3('{url}', '{MINIO_USER}', '{MINIO_PASS}')
        {pclause}
        SETTINGS s3_retry_attempts = {s3_retry_attempts}
        """
    )


def first_partition_id(node, table):
    """Return the partition_id of the first active part of *table*."""
    return node.query(
        f"SELECT partition_id FROM system.parts"
        f" WHERE database = currentDatabase() AND table = '{table}' AND active"
        f" ORDER BY name LIMIT 1"
    ).strip()


def unique_suffix():
    """Return a UUID with hyphens replaced by underscores, suitable for table names."""
    return str(uuid.uuid4()).replace("-", "_")


# -- how a destination whose column count exceeds the source's is matched
EXTRA_SOURCE_COLUMN_MODES = [
    pytest.param("POSITION", id="by-position"),
    pytest.param("NAME", id="by-name"),
]


class RejectedPartitionExportCase(NamedTuple):
    src_columns: str
    src_partition_by: str
    dst_columns: str
    dst_partition_by: str
    insert_values: str
    error_substrings: tuple = ()


# Partition keys that must be rejected regardless of the destination kind. A destination-specific
# suite may append its own cases (see the Iceberg transform case).
REJECTED_PARTITION_EXPORT_CASES = [
    pytest.param(
        RejectedPartitionExportCase(
            src_columns="a Int32, b Int32",
            src_partition_by="a",
            dst_columns="b Int32, a Int32",
            dst_partition_by="a",
            insert_values="(1, 1), (1, 2)",
            error_substrings=("partition key column",),
        ),
        id="same_partition_key_different_column_order_single_column",
    ),
    pytest.param(
        RejectedPartitionExportCase(
            src_columns="a Int32, b Int32, c Int32, val String",
            src_partition_by="(a, b, c)",
            dst_columns="c Int32, b Int32, a Int32, val String",
            dst_partition_by="(a, b, c)",
            insert_values="(1, 1, 1, 'x'), (1, 1, 1, 'y')",
            error_substrings=("partition key column",),
        ),
        id="same_partition_key_different_column_order_multi_column",
    ),
    pytest.param(
        RejectedPartitionExportCase(
            src_columns="a Int32, b Int32, c Int32, val String",
            src_partition_by="(a, b)",
            dst_columns="a Int32, b Int32, c Int32, val String",
            dst_partition_by="(a, b, c)",
            insert_values="(1, 2, 3, 'x')",
            error_substrings=(
                "column 'c', which is not part of the source MergeTree partition key",
            ),
        ),
        id="multi_column_partition_key_more_in_destination",
    ),
]
