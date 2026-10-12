import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "cas_gc_conditional_batch"
DISK_NAME = "disk_cas_gc_conditional_batch"
BLOBS_PREFIX = "cas_gc_conditional_batch/blobs/"
CHUNK_KEYS = 2
NUM_INSERTS = 6
MAX_GC_ROUNDS = 12


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


def list_blob_bodies():
    return {
        o.object_name
        for o in cluster.rustfs_client.list_objects(
            cluster.rustfs_bucket, BLOBS_PREFIX, recursive=True
        )
        if not o.object_name.endswith(".meta")
    }


def latest_gc_round(node):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            f"SELECT max(round) FROM system.cas_gc_log WHERE disk_name = '{DISK_NAME}'"
        ).strip()
    )


def gc_pending_delete_rows(node, after_round):
    node.query("SYSTEM FLUSH LOGS")
    rows = (
        node.query(
            "SELECT p.phase_metrics['redeleted'], p.phase_metrics['delete_batches'], "
            "p.phase_metrics['jobs_failed'] "
            "FROM system.cas_gc_log AS p INNER JOIN "
            "(SELECT round_id FROM system.cas_gc_log "
            f"WHERE disk_name = '{DISK_NAME}' AND event_type = 'Finish' "
            f"AND round > {after_round}) AS f USING round_id "
            f"WHERE p.disk_name = '{DISK_NAME}' AND p.event_type = 'Phase' "
            "AND p.phase = 'pending_deletes' AND p.phase_metrics['redeleted'] > 0 "
            "ORDER BY p.event_time_microseconds"
        )
        .strip()
        .splitlines()
    )
    return [tuple(int(x) for x in row.split("\t")) for row in rows]


def gc_redeleted_total(node, after_round):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            "SELECT sum(entries_redeleted) FROM system.cas_gc_log "
            f"WHERE disk_name = '{DISK_NAME}' AND event_type = 'Finish' "
            f"AND round > {after_round}"
        ).strip()
    )


def test_rustfs_pending_blob_deletes_use_singleton_fallback():
    node = cluster.instances["node"]
    table = "conditional_batch_redelete"
    blobs_before = list_blob_bodies()
    node.query(
        f"CREATE TABLE {table} (k UInt64, v String) ENGINE = MergeTree ORDER BY k "
        f"SETTINGS storage_policy = '{STORAGE_POLICY}'"
    )
    node.query(f"SYSTEM STOP MERGES {table}")
    for i in range(NUM_INSERTS):
        node.query(
            f"INSERT INTO {table} SELECT number + {i * 1000}, "
            f"concat(toString(number), '-{i}') FROM numbers(1000) "
            "SETTINGS max_insert_block_size = 1000"
        )

    created_blobs = list_blob_bodies() - blobs_before
    assert len(created_blobs) > CHUNK_KEYS
    first_round = latest_gc_round(node)
    node.query(f"DROP TABLE {table} SYNC")

    rows = []
    remaining = created_blobs
    for _ in range(MAX_GC_ROUNDS):
        node.query("SYSTEM CAS GC RUN")
        rows = gc_pending_delete_rows(node, first_round)
        remaining = created_blobs & list_blob_bodies()
        if not remaining and rows:
            break

    assert not remaining, f"GC left blob bodies in RustFS: {sorted(remaining)}"
    assert rows, "pending_deletes never reported completed blob redeletes"
    for redeleted, batches, jobs_failed in rows:
        assert jobs_failed == 0
        assert batches == redeleted
    assert gc_redeleted_total(node, first_round) >= len(created_blobs)
