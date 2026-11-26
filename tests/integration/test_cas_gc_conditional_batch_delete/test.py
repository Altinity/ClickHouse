import json
import logging

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


def list_keys(pool_prefix, prefix):
    return {
        o.object_name
        for o in cluster.rustfs_client.list_objects(
            cluster.rustfs_bucket, f"{pool_prefix}/{prefix}/", recursive=True
        )
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
    rows = node.query(
        "SELECT f.round AS round_no, p.phase_metrics AS metrics, "
        "p.ProfileEvents['DiskS3DeleteObjects'] AS s3_requests, "
        "p.ProfileEvents['CASBulkDeleteRequests'] AS intent_requests "
        "FROM system.cas_gc_log AS p INNER JOIN "
        "(SELECT round_id, round FROM system.cas_gc_log "
        f"WHERE disk_name = '{disk_name}' AND event_type = 'Finish' AND outcome = 'Success' "
        f"AND round > {after_round}) AS f USING round_id "
        f"WHERE p.disk_name = '{disk_name}' AND p.event_type = 'Phase' "
        "AND p.phase = 'pending_deletes' ORDER BY f.round FORMAT JSONEachRow",
        settings={"output_format_json_quote_64bit_integers": 0},
    )
    return [json.loads(row) for row in rows.splitlines()]


def reclaim_blob_bodies(pool_prefix, chunk_keys):
    node = cluster.instances["node"]
    disk_name = f"disk_{pool_prefix}"
    table = f"{pool_prefix}_redelete"
    node.query(f"SYSTEM CAS GC STOP '{disk_name}'")
    assert not list_keys(pool_prefix, "blobs")
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

    created_blobs = list_keys(pool_prefix, "blobs")
    assert len(created_blobs) > 2
    first_round = latest_gc_round(node, disk_name)
    node.query(f"DROP TABLE {table} SYNC")

    for _ in range(MAX_GC_ROUNDS):
        node.query(f"SYSTEM CAS GC RUN '{disk_name}'")
        remaining = created_blobs & list_keys(pool_prefix, "blobs")
        if not remaining and not list_keys(pool_prefix, "blob-intents"):
            break
    else:
        pytest.fail(f"{disk_name}: GC left blobs {remaining} or intents {list_keys(pool_prefix, 'blob-intents')}")

    total_deleted = total_body_requests = total_intent_requests = 0
    for row in gc_pending_delete_rows(node, disk_name, first_round):
        logging.info("Conditional delete requests on %s: %s", disk_name, row)
        metrics = row["metrics"]
        assert metrics["jobs_scheduled"] == metrics["delete_chunk_jobs_scheduled"] == 0, row
        assert metrics["jobs_failed"] == metrics["delete_chunk_jobs_failed"] == 0, row
        assert metrics["delete_chunk_job_schedule_failures"] == 0, row
        # No other writer replaces or removes these blobs after the drop.
        assert metrics["bodies_deleted"] == metrics["deleted"], row
        expected_chunks = (metrics["bodies_deleted"] + chunk_keys - 1) // chunk_keys
        assert metrics["delete_chunks"] == expected_chunks, row
        # Only intent cleanup uses write-once deletes in this phase. The phase
        # also sees all conditional S3 requests because blob deletes run inline.
        expected_intent_requests = (metrics["intents_deleted"] + chunk_keys - 1) // chunk_keys
        assert row["intent_requests"] == expected_intent_requests, row
        body_requests = row["s3_requests"] - row["intent_requests"]
        assert body_requests == expected_chunks, row
        total_deleted += metrics["deleted"]
        total_body_requests += body_requests
        total_intent_requests += row["intent_requests"]
    assert total_deleted == len(created_blobs)
    assert total_intent_requests > 0
    return total_deleted, total_body_requests


def test_pending_blob_deletes_use_conditional_batches():
    singleton_deleted, singleton_requests = reclaim_blob_bodies(
        "cas_gc_conditional_singleton", 1
    )
    batch_deleted, batch_requests = reclaim_blob_bodies("cas_gc_conditional_batch", 2)

    # With the real mount probe gating multi-key chunks, a passed comparison proves batching
    # was enabled and reached RustFS. Both disks reclaim the same size workload.
    assert singleton_deleted == batch_deleted
    assert batch_requests < singleton_requests
