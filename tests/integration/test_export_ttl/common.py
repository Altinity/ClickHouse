import contextlib
import json
import os
import time

from helpers.export_partition_helpers import is_replicated_engine, make_iceberg_s3, unique_suffix
from helpers.iceberg_export_stats import fetch_manifest_entries

# Helpers shared by the `TTL ... EXPORT TO TABLE` modules.

# The columns of a source and of its Iceberg destination, which has no unsigned types.
COLUMNS = "id Int64, year Int32, t DateTime"

# Rows whose TTL `t + INTERVAL 1 DAY` is due long ago, and rows that are not due before a test ends.
DUE = "now() - INTERVAL 10 DAY"
NOT_DUE = "now()"

# A group is shipped on the first check after its parts are due.
FAST_TTL_SETTINGS = {
    "ttl_export_check_period_seconds": 1,
    "ttl_export_batch_window_seconds": 0,
    "ttl_export_batch_max_delay_seconds": 0,
}

PAUSE_EXPORT_FAILPOINT = "export_part_pause_before_schema_validation"


def settings_clause(settings):
    return ", ".join(f"{name} = {value!r}" if isinstance(value, str) else f"{name} = {value}" for name, value in settings.items())


def zookeeper_path(table):
    return f"/clickhouse/tables/{table}"


def engine_clause(engine, table, replica_name="replica1"):
    if is_replicated_engine(engine):
        return f"ReplicatedMergeTree('{zookeeper_path(table)}', '{replica_name}')"
    return "MergeTree()"


def source_ddl(table, columns, partition_by, ttl, engine="MergeTree", replica_name="replica1", order_by="tuple()", settings=None):
    all_settings = dict(FAST_TTL_SETTINGS)
    all_settings.update(settings or {})
    partition = f"PARTITION BY {partition_by}" if partition_by else ""
    return (
        f"CREATE TABLE {table} ({columns}) ENGINE = {engine_clause(engine, table, replica_name)}"
        f" {partition} ORDER BY {order_by} TTL {ttl} SETTINGS {settings_clause(all_settings)}"
    )


def create_source(node, table, columns, partition_by, ttl, **kwargs):
    node.query(source_ddl(table, columns, partition_by, ttl, **kwargs))


def create_source_error(node, table, columns, partition_by, ttl, **kwargs):
    return node.query_and_get_error(source_ddl(table, columns, partition_by, ttl, **kwargs))


def create_s3_hive(node, table, columns, partition_by):
    node.query(
        f"CREATE TABLE {table} ({columns})"
        f" ENGINE = S3(s3_conn, filename='{table}', format=Parquet, partition_strategy='hive') PARTITION BY {partition_by}"
    )


def create_s3_wildcard(node, table, columns, partition_by):
    node.query(
        f"CREATE TABLE {table} ({columns})"
        f" ENGINE = S3(s3_conn, filename='{table}/{{_partition_id}}/{{_file}}.parquet', format=Parquet, partition_strategy='wildcard')"
        f" PARTITION BY {partition_by}"
    )


def wait_until(predicate, timeout=90, message="The condition did not hold", interval=0.5):
    """Poll *predicate* until it returns a truthy value, and return it."""
    start = time.time()
    last = None
    while time.time() - start < timeout:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"{message} within {timeout} s, last value: {last!r}")


def query_json(node, query):
    result = node.query(f"{query} FORMAT JSONCompact", settings={"output_format_json_quote_64bit_integers": 0})
    return json.loads(result)["data"]


# `system.ttl_exports`

TTL_STATE_COLUMNS = [
    "partition_id",
    "exported_parts",
    "claimed_parts",
    "eligible_parts",
    "parts_held_by_delete_gate",
    "current_transaction_id",
    "last_error",
    "scheduler_replica",
]


def ttl_rows(node, table, columns=None):
    """The rows of `system.ttl_exports` of *table*, by partition id."""
    columns = columns or TTL_STATE_COLUMNS
    rows = query_json(node, f"SELECT {', '.join(columns)} FROM system.ttl_exports WHERE table = '{table}' ORDER BY partition_id")
    return {row[0]: dict(zip(columns, row)) for row in rows}


def wait_for_same_ttl_rows(replicas, table, settled, timeout=90, columns=None):
    """Wait until every replica shows the same rows of `system.ttl_exports` for *table*, and they
    satisfy *settled*."""
    start = time.time()
    while True:
        rows = [ttl_rows(replica, table, columns) for replica in replicas]
        if all(replica_rows == rows[0] for replica_rows in rows) and settled(rows[0]):
            return rows[0]
        assert time.time() - start < timeout, f"The rows of system.ttl_exports did not settle: {rows}"
        time.sleep(0.5)


def first_eligible_time(node, table, partition_id):
    """When the scheduler first saw an eligible part of the partition that is not exported, 0 if never."""
    return int(node.query(
        f"SELECT toUnixTimestamp(first_eligible_time) FROM system.ttl_exports WHERE table = '{table}' AND partition_id = '{partition_id}'"
    ).strip() or 0)


def partition_settled(rows, partition_id, exported=None):
    """Nothing of the partition is claimed or waiting to be exported."""
    row = rows.get(partition_id)
    if row is None or row["claimed_parts"] != 0 or row["eligible_parts"] != 0 or row["current_transaction_id"] != "":
        return False
    return exported is None or row["exported_parts"] == exported


def wait_for_partitions_exported(node, table, partition_ids, timeout=90):
    """Wait until nothing of *partition_ids* is claimed or eligible and no TTL task is in flight."""
    def settled():
        rows = ttl_rows(node, table)
        if any(not partition_settled(rows, partition_id) for partition_id in partition_ids):
            return None
        return pending_ttl_tasks(node, table) == 0 and rows

    return wait_until(settled, timeout, f"The TTL export of {table} partitions {partition_ids} did not settle")


def wait_for_last_error(node, table, partition_id, substring, timeout=90):
    def has_error():
        row = ttl_rows(node, table).get(partition_id)
        return row if row and substring in row["last_error"] else None

    return wait_until(has_error, timeout, f"No error containing {substring!r} for partition {partition_id} of {table}")


# `system.distributed_exports`

def ttl_tasks(node, table):
    rows = query_json(
        node,
        f"SELECT transaction_id, partition_id, status, parts, retry_of FROM system.distributed_exports"
        f" WHERE source_table = '{table}' AND source = 'ttl' ORDER BY create_time, transaction_id",
    )
    return [dict(zip(["transaction_id", "partition_id", "status", "parts", "retry_of"], row)) for row in rows]


def pending_ttl_tasks(node, table):
    return sum(1 for task in ttl_tasks(node, table) if task["status"] == "PENDING")


def completed_ttl_tasks(node, table):
    return [task for task in ttl_tasks(node, table) if task["status"] == "COMPLETED"]


# Destinations

def committed_s3_files(node, table):
    """Names of the data files that the commit files of an object storage destination reference."""
    lines = node.query(f"SELECT line FROM s3(s3_conn, filename='{table}/commit_*', format=LineAsString)").split()
    return {os.path.basename(line) for line in lines}


def commit_files(node, table):
    return int(node.query(f"SELECT uniqExact(_file) FROM s3(s3_conn, filename='{table}/commit_*', format=LineAsString)").strip())


def s3_files(node, table, committed_only=True):
    """{partition directory: sorted ids} of the data files of an object storage destination. Readers
    must only consider files that a commit file references, so by default the others are ignored."""
    committed = committed_s3_files(node, table) if committed_only else None
    result = {}
    rows = query_json(node, f"SELECT _path, id FROM s3(s3_conn, filename='{table}/**.parquet', format=Parquet, structure='id UInt64')")
    for path, row_id in rows:
        if committed is not None and os.path.basename(path) not in committed:
            continue
        directory = path.split(f"{table}/", 1)[1].rsplit("/", 1)[0]
        result.setdefault(directory, []).append(row_id)
    return {directory: sorted(ids) for directory, ids in result.items()}


def s3_ids(node, table, committed_only=True):
    return sorted(row_id for ids in s3_files(node, table, committed_only).values() for row_id in ids)


# Snapshots of each Iceberg destination when it was created, see `assert_one_snapshot_per_task`.
_snapshots_at_creation = {}


def create_iceberg(nodes, table, columns=COLUMNS, partition_by="year", attach=False):
    """An Iceberg destination at the MinIO prefix named after the table. It is created on the first of
    *nodes*, and the others attach to the same data. With *attach* the first one attaches too, for
    example to the data of a dropped table of the same name, which a `DROP` leaves in place."""
    nodes = list(nodes) if isinstance(nodes, (list, tuple)) else [nodes]
    make_iceberg_s3(nodes[0], table, columns, partition_by=partition_by, if_not_exists=attach)
    for node in nodes[1:]:
        make_iceberg_s3(node, table, columns, partition_by=partition_by, if_not_exists=True)
    _snapshots_at_creation[table] = iceberg_snapshots(nodes[0], table)


def iceberg_ids(node, table):
    return [int(x) for x in node.query(f"SELECT id FROM {table} ORDER BY id").split()]


def iceberg_snapshots(node, table):
    return int(node.query(
        f"SELECT count() FROM system.iceberg_history WHERE database = currentDatabase() AND table = '{table}'"
    ).strip())


def assert_one_snapshot_per_task(node, mt_table, iceberg_table):
    """Every completed TTL task committed one snapshot since the destination was created, and no
    other task committed."""
    snapshots = iceberg_snapshots(node, iceberg_table) - _snapshots_at_creation.get(iceberg_table, 0)
    completed = completed_ttl_tasks(node, mt_table)
    assert snapshots == len(completed), f"{snapshots} snapshots for {len(completed)} completed tasks: {ttl_tasks(node, mt_table)}"


def _partition_scalar(value):
    """A partition field value, without the Avro-union `{type: value}` wrapper."""
    if isinstance(value, dict):
        assert len(value) == 1, f"Unexpected partition union shape: {value!r}"
        value = next(iter(value.values()))
    return value


def iceberg_data_files(node, table):
    """{file name: partition record} of the live data files of an Iceberg table, from its manifests."""
    query_id = f"iceberg_files_{unique_suffix()}"
    node.query(f"SELECT * FROM {table}", query_id=query_id, settings={"iceberg_metadata_log_level": "manifest_file_entry"})
    files = {}
    for entry in fetch_manifest_entries(node, query_id):
        data_file = entry.get("data_file") or {}
        if data_file.get("content", 0) not in (0, None) or entry.get("status") == 2:
            continue
        partition = {name: _partition_scalar(value) for name, value in (data_file.get("partition") or {}).items()}
        files[os.path.basename(data_file["file_path"])] = partition
    return files


def iceberg_orphan_files(node, table):
    """Data files under the prefix of an Iceberg table that no manifest references."""
    written = {
        os.path.basename(row[0])
        for row in query_json(node, f"SELECT DISTINCT _path FROM s3(s3_conn, filename='{table}/**.parquet', format=Parquet, structure='id Int64')")
    }
    return written - set(iceberg_data_files(node, table))


def assert_iceberg_files_partitioned(node, table, field, expression):
    """Every data file holds rows of one value of *expression*, which is the value of the partition
    field *field* recorded for the file in the manifest. Returns the values."""
    files = iceberg_data_files(node, table)
    rows = query_json(node, f"SELECT _path, groupUniqArray({expression}) FROM {table} GROUP BY _path")
    assert rows, f"{table} has no rows"
    values = set()
    for path, file_values in rows:
        assert len(file_values) == 1, f"The file {path} holds rows of several partitions: {file_values}"
        name = os.path.basename(path)
        assert name in files, f"The file {name} is not in the manifests: {files}"
        recorded = files[name].get(field)
        assert recorded is not None and int(recorded) == int(file_values[0]), (
            f"The file {name} holds {expression} = {file_values[0]}, the manifest records {files[name]}"
        )
        values.add(int(file_values[0]))
    return values


def assert_exactly_once(ids, expected):
    assert sorted(ids) == sorted(expected), f"Expected exactly {sorted(expected)}, got {sorted(ids)}"


# Parts and merges

def active_parts(node, table, partition_id=None):
    condition = f" AND partition_id = '{partition_id}'" if partition_id is not None else ""
    return node.query(
        f"SELECT name FROM system.parts WHERE database = currentDatabase() AND table = '{table}' AND active{condition} ORDER BY name"
    ).split()


def merged_parts(node, table):
    """Names of the parts of *table* that were the source of a merge."""
    node.query("SYSTEM FLUSH LOGS part_log")
    return set(node.query(
        f"SELECT arrayJoin(merged_from) FROM system.part_log"
        f" WHERE database = currentDatabase() AND table = '{table}' AND event_type = 'MergeParts'"
    ).split())


def assert_never_merged(node, table, part_names, seconds=5):
    """Give merges *seconds* to happen, asking for them every second, and check that none of
    *part_names* was merged."""
    start = time.time()
    while time.time() - start < seconds:
        node.query(f"OPTIMIZE TABLE {table}")
        time.sleep(1)
    active = active_parts(node, table)
    missing = [name for name in part_names if name not in active]
    assert not missing, f"Parts {missing} are no longer active on {node.name}; active parts: {active}"
    merged = set(part_names) & merged_parts(node, table)
    assert not merged, f"Parts {sorted(merged)} were merged on {node.name}"


def optimize_final_error(node, table, partition_id):
    return node.query_and_get_error(
        f"OPTIMIZE TABLE {table} PARTITION ID '{partition_id}' FINAL", settings={"optimize_throw_if_noop": 1}
    )


@contextlib.contextmanager
def group_in_flight(node):
    """Pauses the export of the next part on *node*, which keeps its group claimed. Yields a function
    that waits until the export is paused."""
    node.query(f"SYSTEM ENABLE FAILPOINT {PAUSE_EXPORT_FAILPOINT}")
    try:
        yield lambda: node.query(f"SYSTEM WAIT FAILPOINT {PAUSE_EXPORT_FAILPOINT} PAUSE")
    finally:
        node.query(f"SYSTEM DISABLE FAILPOINT {PAUSE_EXPORT_FAILPOINT}")


@contextlib.contextmanager
def failpoint(nodes, name):
    for node in nodes:
        node.query(f"SYSTEM ENABLE FAILPOINT {name}")
    try:
        yield
    finally:
        for node in nodes:
            node.query(f"SYSTEM DISABLE FAILPOINT {name}")


# Replicas

def scheduler_holder(node, table):
    return node.query(
        f"SELECT value FROM system.zookeeper WHERE path = '{zookeeper_path(table)}/export_ttl' AND name = 'scheduler_lock'"
    ).strip()


def snapshot_refreshes(node):
    return int(node.query("SELECT value FROM system.events WHERE event = 'ExportTTLIndexSnapshotRefreshes'").strip() or 0)
