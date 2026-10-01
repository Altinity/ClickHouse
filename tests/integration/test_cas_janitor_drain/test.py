"""
A dropped table's ref stream drains in one deleting round of the namespace janitor on RustFS: batches of
the storage's size on a batch store, one-key jobs on a store without batch delete.
"""
import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

INSERTS = 2500
MAX_ROUNDS = 3


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def stream_keys(disk):
    prefix = f"cas_janitor_{disk.removeprefix('cas_')}/cas/ns/stream/"
    return [
        o.object_name
        for o in cluster.rustfs_client.list_objects(cluster.rustfs_bucket, prefix, recursive=True)
        if "/_log/" in o.object_name or "/_snap/" in o.object_name
    ]


def seed_and_drop(node, disk):
    node.query(f"DROP TABLE IF EXISTS t_{disk} SYNC")
    node.query(
        f"CREATE TABLE t_{disk} (k UInt64) ENGINE = MergeTree ORDER BY k "
        f"SETTINGS storage_policy = '{disk}', parts_to_delay_insert = 100000, parts_to_throw_insert = 100000"
    )
    node.query(f"SYSTEM STOP MERGES t_{disk}")
    # One row per block gives one part per row without INSERTS client round trips.
    node.query(
        f"INSERT INTO t_{disk} SELECT number FROM numbers({INSERTS}) SETTINGS max_block_size = 1, "
        "max_insert_block_size = 1, min_insert_block_size_rows = 1, min_insert_block_size_bytes = 0, "
        "max_insert_threads = 1"
    )
    assert len(stream_keys(disk)) >= INSERTS
    node.query(f"DROP TABLE t_{disk} SYNC")
    return len(stream_keys(disk))


def janitor_rows(node, disk, since):
    node.query("SYSTEM FLUSH LOGS")
    columns = [
        "janitor_deleted", "janitor_pages", "budget_exhausted", "batch_keys",
        "batches", "delete_jobs",
    ]
    select = ", ".join(f"phase_metrics['{c}']" for c in columns)
    out = node.query(
        f"SELECT {select} FROM system.cas_gc_log WHERE event_type = 'Phase' AND phase = 'namespace_cleanup' "
        f"AND disk_name = '{disk}' AND event_time_microseconds >= toDateTime64('{since}', 6) "
        "ORDER BY event_time_microseconds FORMAT TSV"
    )
    return [dict(zip(columns, map(int, line.split("\t")))) for line in out.strip().splitlines()]


def drain(node, disk):
    since = node.query("SELECT now64(6)").strip()
    dead_keys = seed_and_drop(node, disk)
    for _ in range(MAX_ROUNDS):
        node.query(f"SYSTEM CAS GC RUN {disk}")
        if not stream_keys(disk):
            break
    assert not stream_keys(disk), f"{disk}: dead ref stream keys left after {MAX_ROUNDS} rounds"
    rows = janitor_rows(node, disk, since)
    deleting = [i for i, r in enumerate(rows) if r["janitor_deleted"] > 0]
    assert len(deleting) == 1, f"{disk}: expected one deleting round, got {rows}"
    # Round 1 folds the drop and deletes nothing; the next round drains the whole stream.
    assert deleting[0] == 1, f"{disk}: the deleting round is not the one right after the fold round: {rows}"
    row = rows[deleting[0]]
    assert row["janitor_deleted"] >= dead_keys, f"{disk}: the round did not drain all {dead_keys} dead keys: {rows}"
    return row


def test_batch_store_drains_bulk_in_one_round():
    row = drain(cluster.instances["node"], "cas_batch")
    assert row["janitor_pages"] >= 3
    assert row["budget_exhausted"] == 0


def test_storage_chunk_setting_sizes_batches():
    row = drain(cluster.instances["node"], "cas_small_chunk")
    assert row["batch_keys"] == 100
    assert row["batches"] >= 25


def test_store_without_batch_delete_drains_with_one_key_jobs():
    row = drain(cluster.instances["node"], "cas_no_batch")
    assert row["batch_keys"] == 1
    assert row["delete_jobs"] >= INSERTS
    assert row["batches"] == row["delete_jobs"], f"a one-key job was skipped or held: {row}"
