import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

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


def list_blob_bodies(pool_prefix):
    return {
        o.object_name
        for o in cluster.rustfs_client.list_objects(
            cluster.rustfs_bucket, f"{pool_prefix}/blobs/", recursive=True
        )
        if not o.object_name.endswith(".meta")
    }


def latest_gc_round(node, disk_name):
    node.query("SYSTEM FLUSH LOGS")
    return int(
        node.query(
            f"SELECT max(round) FROM system.cas_gc_log WHERE disk_name = '{disk_name}'"
        ).strip()
    )


def gc_pending_delete_rows(node, disk_name, after_round):
    node.query("SYSTEM FLUSH LOGS")
    rows = (
        node.query(
            "SELECT f.round, p.phase_metrics['redeleted'], p.phase_metrics['deleted'], "
            "p.phase_metrics['delete_chunks'], p.ProfileEvents['DiskS3DeleteObjects'], "
            "p.phase_metrics['read_blob_jobs_failed'], "
            "p.phase_metrics['delete_chunk_jobs_failed'], "
            "p.phase_metrics['read_blob_job_schedule_failures'], "
            "p.phase_metrics['delete_chunk_job_schedule_failures'] "
            "FROM system.cas_gc_log AS p INNER JOIN "
            "(SELECT round_id, round FROM system.cas_gc_log "
            f"WHERE disk_name = '{disk_name}' AND event_type = 'Finish' AND outcome = 'Success' "
            f"AND round > {after_round}) AS f USING round_id "
            f"WHERE p.disk_name = '{disk_name}' AND p.event_type = 'Phase' "
            "AND p.phase = 'pending_deletes' AND p.phase_metrics['redeleted'] > 0 "
            "ORDER BY p.event_time_microseconds"
        )
        .strip()
        .splitlines()
    )
    return [tuple(int(x) for x in row.split("\t")) for row in rows]


def reclaim_blob_bodies(pool_prefix, chunk_keys):
    node = cluster.instances["node"]
    disk_name = f"disk_{pool_prefix}"
    table = f"{pool_prefix}_redelete"
    node.query(f"SYSTEM CAS GC STOP '{disk_name}'")
    assert not list_blob_bodies(pool_prefix)
    node.query(
        f"CREATE TABLE {table} (k UInt64, v String) ENGINE = MergeTree ORDER BY k "
        f"SETTINGS storage_policy = '{pool_prefix}'"
    )
    node.query(f"SYSTEM STOP MERGES {table}")
    for i in range(NUM_INSERTS):
        node.query(
            f"INSERT INTO {table} SELECT number + {i * 1000}, "
            f"concat(toString(number), '-{i}') FROM numbers(1000) "
            "SETTINGS max_insert_block_size = 1000"
        )

    created_blobs = list_blob_bodies(pool_prefix)
    assert len(created_blobs) > 2
    first_round = latest_gc_round(node, disk_name)
    node.query(f"DROP TABLE {table} SYNC")

    rows = []
    remaining = created_blobs
    for _ in range(MAX_GC_ROUNDS):
        node.query(f"SYSTEM CAS GC RUN '{disk_name}'")
        rows = gc_pending_delete_rows(node, disk_name, first_round)
        remaining = created_blobs & list_blob_bodies(pool_prefix)
        if not remaining and rows:
            break

    assert not remaining, f"GC left blob bodies in RustFS: {sorted(remaining)}"
    assert rows, f"{disk_name}: pending_deletes never reported completed blob redeletes"
    total_deleted = total_requests = 0
    for (
        round_no,
        redeleted,
        deleted,
        delete_chunks,
        s3_delete_requests,
        read_blob_jobs_failed,
        delete_chunk_jobs_failed,
        read_blob_job_schedule_failures,
        delete_chunk_job_schedule_failures,
    ) in rows:
        assert read_blob_jobs_failed == 0
        assert delete_chunk_jobs_failed == 0
        assert read_blob_job_schedule_failures == 0
        assert delete_chunk_job_schedule_failures == 0
        # The isolated workload neither replaces nor removes blob bodies outside GC.
        assert redeleted == deleted, f"{disk_name}, round {round_no}: {rows}"
        expected_chunks = (deleted + chunk_keys - 1) // chunk_keys
        assert delete_chunks == expected_chunks, f"{disk_name}, round {round_no}: {rows}"
        assert s3_delete_requests == delete_chunks, f"{disk_name}, round {round_no}: {rows}"
        total_deleted += deleted
        total_requests += s3_delete_requests
    assert total_deleted == len(created_blobs)
    return total_deleted, total_requests


def test_pending_blob_deletes_use_conditional_batches():
    singleton_deleted, singleton_requests = reclaim_blob_bodies(
        "cas_gc_conditional_singleton", 1
    )
    batch_deleted, batch_requests = reclaim_blob_bodies("cas_gc_conditional_batch", 2)

    # With the real mount probe gating multi-key chunks, a passed comparison proves batching
    # was enabled and reached RustFS. Both disks reclaim the same size workload.
    assert singleton_deleted == batch_deleted
    assert batch_requests < singleton_requests
