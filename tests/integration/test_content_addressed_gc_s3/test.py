import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "content_addressed_gc_s3"

# Endpoint is http://rustfs1:11121/test/cas_gc_data/, so the pool's blobs and part footers live
# under these key prefixes inside the `test` RustFS bucket. The authoritative "no S3 leftovers"
# proof checks BOTH: a dropped table must leave neither content blobs nor part footers behind.
BLOBS_PREFIX = "cas_gc_data/blobs/"
PARTS_PREFIX = "cas_gc_data/parts/"

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
        with_rustfs=True,
        stay_alive=True,
    )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def count_prefix(prefix):
    objects = cluster.rustfs_client.list_objects(
        cluster.rustfs_bucket, prefix, recursive=True
    )
    return len(list(objects))


def count_pool_objects():
    # Both content blobs and part footers must be reclaimed for the pool to be truly empty.
    return count_prefix(BLOBS_PREFIX) + count_prefix(PARTS_PREFIX)


def test_gc_reclaims_dropped_blobs():
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_gc_test SYNC")

    # (1) Baseline: how many objects (blobs + part footers) exist in the pool before our table.
    baseline = count_pool_objects()

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

    after_insert = count_pool_objects()
    assert (
        after_insert > baseline
    ), "expected pool object count (blobs+parts) to rise above baseline {} after inserts, got {}".format(
        baseline, after_insert
    )

    # (3) Drop the table: refs are unlinked synchronously; the blobs and part footers become
    #     unreferenced GC fodder.
    node.query("DROP TABLE cas_gc_test SYNC")

    # (4) Poll until the background GC reclaims the orphaned objects and the count returns to
    #     baseline. Bounded wait on a background process (grace=1s, interval=1s) — not a race hack.
    #     This is the authoritative "no S3 leftovers" proof: BOTH blobs/ and parts/ must drain.
    final = after_insert
    for _ in range(RECLAIM_RETRIES):
        final = count_pool_objects()
        if final <= baseline:
            break
        time.sleep(RECLAIM_SLEEP)

    assert final <= baseline, (
        "background GC did not reclaim dropped objects (blobs+parts) within {}s: "
        "baseline={}, after_insert={}, final={} (blobs={}, parts={})".format(
            int(RECLAIM_RETRIES * RECLAIM_SLEEP),
            baseline,
            after_insert,
            final,
            count_prefix(BLOBS_PREFIX),
            count_prefix(PARTS_PREFIX),
        )
    )
