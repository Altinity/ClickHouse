import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "content_addressed_gc_s3"

# Endpoint is http://minio1:9001/root/cas_gc_data/, so the pool's blobs live under this key prefix
# inside the `root` MinIO bucket.
BLOBS_PREFIX = "cas_gc_data/blobs/"

# Enough rows / inserts to materialise several distinct blobs in the pool.
NUM_ROWS = 100000
NUM_INSERTS = 8

# The background GC runs with grace=1s, interval=1s. After DROP TABLE ... SYNC the dropped table's
# footers/blobs become unreferenced and the sweep reclaims them after grace. We poll for that with a
# generous total bound; this is waiting on a known background process, not papering over a race.
RECLAIM_RETRIES = 60
RECLAIM_SLEEP = 1.0  # seconds; total bound ~= RECLAIM_RETRIES * RECLAIM_SLEEP = 60s


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_minio=True,
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


def test_gc_reclaims_dropped_blobs():
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_gc_test SYNC")

    # (1) Baseline: how many blobs exist in the pool before we create our table.
    baseline = count_blobs()

    node.query(
        """
        CREATE TABLE cas_gc_test (
            id Int64,
            data String
        ) ENGINE = MergeTree()
        ORDER BY id
        SETTINGS storage_policy = '{}'
        """.format(
            STORAGE_POLICY
        )
    )

    # (2) Insert enough distinct rows across several inserts to produce several blobs.
    for i in range(NUM_INSERTS):
        node.query(
            "INSERT INTO cas_gc_test "
            "SELECT number + {offset}, toString(number + {offset}) "
            "FROM numbers({rows})".format(offset=i * NUM_ROWS, rows=NUM_ROWS)
        )

    assert int(node.query("SELECT count() FROM cas_gc_test")) == NUM_INSERTS * NUM_ROWS

    after_insert = count_blobs()
    assert (
        after_insert > baseline
    ), "expected blob count to rise above baseline {} after inserts, got {}".format(
        baseline, after_insert
    )

    # (3) Drop the table: refs are unlinked synchronously; the blobs become unreferenced GC fodder.
    node.query("DROP TABLE cas_gc_test SYNC")

    # (4) Poll until the background GC reclaims the orphaned blobs and the count returns to baseline.
    #     Bounded wait on a background process (grace=1s, interval=1s) — not a race hack.
    final = after_insert
    for _ in range(RECLAIM_RETRIES):
        final = count_blobs()
        if final <= baseline:
            break
        time.sleep(RECLAIM_SLEEP)

    assert final <= baseline, (
        "background GC did not reclaim dropped blobs within {}s: "
        "baseline={}, after_insert={}, final={}".format(
            int(RECLAIM_RETRIES * RECLAIM_SLEEP), baseline, after_insert, final
        )
    )
