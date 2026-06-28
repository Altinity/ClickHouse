import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

# Both replicas mount the SAME content-addressed pool (endpoint .../root/shared_pool/). A
# ReplicatedMergeTree part written on one replica is therefore ALREADY present (as content blobs +
# manifest) in the pool when the other replica needs it — so the "fetch" is a fetch-by-relink: the
# fetching replica publishes its own ref to the existing blobs instead of downloading any bytes (the CA
# analogue of zero-copy replication, spec §4).
STORAGE_POLICY = "content_addressed_shared"

# The shared pool's blob prefix inside the `root` MinIO bucket. The relink proof is that the fetch does
# NOT create new objects under here: relink publishes a ref (per-server, under store/), never a blob.
BLOBS_PREFIX = "shared_pool/blobs/"

NUM_ROWS = 10000


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node1.xml"],
        macros={"replica": "node1"},
        with_minio=True,
        with_zookeeper=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "node2",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node2.xml"],
        macros={"replica": "node2"},
        with_minio=True,
        with_zookeeper=True,
        stay_alive=True,
    )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def count_blobs():
    objects = cluster.minio_client.list_objects(
        cluster.minio_bucket, BLOBS_PREFIX, recursive=True
    )
    return len(list(objects))


def test_replicated_fetch_by_relink():
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]

    node1.query("DROP TABLE IF EXISTS r SYNC")
    node2.query("DROP TABLE IF EXISTS r SYNC")

    # Two replicas of ONE ReplicatedMergeTree table on the shared CA pool. Lifting B33 is what makes this
    # CREATE succeed at all; the shared-pool mount is what makes the second replica start.
    create_tpl = (
        "CREATE TABLE r (id Int64, v UInt64, s String) "
        "ENGINE = ReplicatedMergeTree('/clickhouse/tables/r', '{{replica}}') "
        "ORDER BY id SETTINGS storage_policy = '{policy}'"
    )
    node1.query(create_tpl.format(policy=STORAGE_POLICY))
    node2.query(create_tpl.format(policy=STORAGE_POLICY))

    # (1) INSERT on replica node1. node2 must replicate the part.
    node1.query(
        "INSERT INTO r SELECT number, number * 10, toString(number) FROM numbers({rows})".format(
            rows=NUM_ROWS
        )
    )

    # Blob count after the insert, BEFORE node2 fetches. This is the relink baseline.
    blobs_after_insert = count_blobs()
    assert blobs_after_insert > 0, "insert must have written content blobs to the shared pool"

    # (2) node2 fetches the part. SYNC REPLICA blocks until the queue (the fetch) drains.
    node2.query("SYSTEM SYNC REPLICA r", timeout=60)

    # (3) node2 reads the SAME rows back.
    expected_sum_id = (NUM_ROWS - 1) * NUM_ROWS // 2
    assert int(node2.query("SELECT count() FROM r")) == NUM_ROWS
    assert int(node2.query("SELECT sum(id) FROM r")) == expected_sum_id
    assert int(node2.query("SELECT sum(v) FROM r")) == expected_sum_id * 10

    # (4) THE RELINK PROOF: the fetch created NO new blob objects. node2 published a ref to the blobs
    #     node1 already wrote — it did not download/re-write them. (Relink, not byte download.)
    blobs_after_fetch = count_blobs()
    assert blobs_after_fetch == blobs_after_insert, (
        "fetch-by-relink must not create new blob objects: had {} after insert, {} after node2 fetched "
        "(a byte download would have re-written blobs)".format(
            blobs_after_insert, blobs_after_fetch
        )
    )

    # (5) A merge on node1 fetched-by-relink by node2: insert a second part on node1, OPTIMIZE to merge,
    #     and confirm node2 picks up the merged part with still no new blobs beyond the merge's own.
    node1.query(
        "INSERT INTO r SELECT number, number * 10, toString(number) FROM numbers({a}, {rows})".format(
            a=NUM_ROWS, rows=NUM_ROWS
        )
    )
    node2.query("SYSTEM SYNC REPLICA r", timeout=60)
    blobs_before_merge = count_blobs()

    node1.query("OPTIMIZE TABLE r FINAL")
    node1.query("SYSTEM SYNC REPLICA r", timeout=60)
    blobs_after_merge_on_node1 = count_blobs()

    # node2 fetches the merged part. The merge itself may write new blobs on node1 (the merged content),
    # but node2's FETCH of that merged part must add NOTHING further (relink).
    node2.query("SYSTEM SYNC REPLICA r", timeout=60)
    blobs_after_merge_fetch = count_blobs()
    assert blobs_after_merge_fetch == blobs_after_merge_on_node1, (
        "fetch-by-relink of the merged part must not create new blobs: {} after node1 merged, {} after "
        "node2 fetched".format(blobs_after_merge_on_node1, blobs_after_merge_fetch)
    )

    assert int(node2.query("SELECT count() FROM r")) == 2 * NUM_ROWS
    assert int(node1.query("SELECT count() FROM r")) == 2 * NUM_ROWS

    node1.query("DROP TABLE IF EXISTS r SYNC")
    node2.query("DROP TABLE IF EXISTS r SYNC")
