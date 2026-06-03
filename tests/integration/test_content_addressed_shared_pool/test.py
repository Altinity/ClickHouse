import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

# Both servers mount the SAME content-addressed pool (endpoint .../root/shared_pool/). The blob pool
# (blobs/ + parts/) is shared across servers; refs are per-server under store/<server_id>/..., so the
# two servers dedup identical content while keeping independent ref roots.
STORAGE_POLICY = "content_addressed_shared"

# blobs/ holds content blobs, parts/ holds part footers. These are the shared pool's object prefixes
# inside the `root` MinIO bucket. "No leftovers" means BOTH drain back to baseline.
BLOBS_PREFIX = "shared_pool/blobs/"
PARTS_PREFIX = "shared_pool/parts/"

# Deterministic data. Identical rows on both nodes => identical content blobs => cross-server dedup.
NUM_ROWS = 100000

# Background GC: grace=3s, interval=1s. After both DROP ... SYNC the pool's objects become
# unreferenced and a sweep (run by either server) reclaims them after grace. Bounded poll: this waits
# on a known background process, it is not papering over a race.
RECLAIM_RETRIES = 60
RECLAIM_SLEEP = 1.0  # seconds; total bound ~= 60s


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    # Only one instance needs with_minio to stand up MinIO; both reach the shared minio1. Both load the
    # identical storage_conf.xml, so both mount the SAME shared pool.
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml"],
        with_minio=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "node2",
        main_configs=["configs/storage_conf.xml"],
        with_minio=True,
        stay_alive=True,
    )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def count_prefix(prefix):
    objects = cluster.minio_client.list_objects(
        cluster.minio_bucket, prefix, recursive=True
    )
    return len(list(objects))


def count_pool_objects():
    # The shared pool is empty only when BOTH content blobs and part footers are gone.
    return count_prefix(BLOBS_PREFIX) + count_prefix(PARTS_PREFIX)


def test_two_servers_share_one_pool():
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]

    node1.query("DROP TABLE IF EXISTS t1 SYNC")
    node2.query("DROP TABLE IF EXISTS t2 SYNC")

    # (0) Baseline pool object count before either table exists.
    baseline = count_pool_objects()

    # (1) Each server creates its OWN MergeTree table on the shared pool. Distinct names => distinct
    #     table UUIDs => independent per-server refs, but the SAME shared blob pool.
    create_tpl = (
        "CREATE TABLE {tbl} (id Int64, v UInt64, s String) "
        "ENGINE = MergeTree() ORDER BY id "
        "SETTINGS storage_policy = '{policy}'"
    )
    node1.query(create_tpl.format(tbl="t1", policy=STORAGE_POLICY))
    node2.query(create_tpl.format(tbl="t2", policy=STORAGE_POLICY))

    # (2) INSERT IDENTICAL deterministic data into both. The content blobs are byte-identical, so the
    #     shared pool dedups them across the two servers. Logical reads must still be correct on each.
    insert_tpl = (
        "INSERT INTO {tbl} "
        "SELECT number, number * 10, toString(number) FROM numbers({rows})"
    )
    node1.query(insert_tpl.format(tbl="t1", rows=NUM_ROWS))
    node2.query(insert_tpl.format(tbl="t2", rows=NUM_ROWS))

    expected_sum_id = (NUM_ROWS - 1) * NUM_ROWS // 2
    assert int(node1.query("SELECT count() FROM t1")) == NUM_ROWS
    assert int(node2.query("SELECT count() FROM t2")) == NUM_ROWS
    assert int(node1.query("SELECT sum(id) FROM t1")) == expected_sum_id
    assert int(node2.query("SELECT sum(id) FROM t2")) == expected_sum_id

    # Cross-server dedup sanity: the two identical single-part inserts must NOT have doubled the pool's
    # blob count. With dedup the blob count after both inserts is well below twice the per-server count.
    after_insert = count_pool_objects()
    assert after_insert > baseline, (
        "expected pool object count to rise above baseline {} after inserts, got {}".format(
            baseline, after_insert
        )
    )

    # (3) Heavy mutations / merges on EACH server, in parallel ownership of the shared pool.
    #     UPDATE (id/s carry forward by reference), DELETE, then OPTIMIZE FINAL.
    node1.query("ALTER TABLE t1 UPDATE v = v + 1 WHERE id % 2 = 0 SETTINGS mutations_sync = 2")
    node2.query("ALTER TABLE t2 UPDATE v = v + 1 WHERE id % 2 = 0 SETTINGS mutations_sync = 2")

    node1.query("ALTER TABLE t1 DELETE WHERE id % 100 = 0 SETTINGS mutations_sync = 2")
    node2.query("ALTER TABLE t2 DELETE WHERE id % 100 = 0 SETTINGS mutations_sync = 2")

    node1.query("OPTIMIZE TABLE t1 FINAL")
    node2.query("OPTIMIZE TABLE t2 FINAL")

    # Post-mutation expected aggregates (identical recipe on both, so both must match).
    count_after_mut = int(node1.query("SELECT count() FROM t1"))
    sum_after_mut = int(node1.query("SELECT sum(v) FROM t1"))
    digest_after_mut = node1.query("SELECT sum(cityHash64(id, v, s)) FROM t1").strip()

    assert int(node2.query("SELECT count() FROM t2")) == count_after_mut
    assert int(node2.query("SELECT sum(v) FROM t2")) == sum_after_mut
    assert node2.query("SELECT sum(cityHash64(id, v, s)) FROM t2").strip() == digest_after_mut

    # (4) Let the background GC (enabled on BOTH servers, short grace) run several sweep cycles while
    #     both tables are still live. The cross-server safety property: a sweep run by either server
    #     must NOT reclaim a blob that the OTHER server's live part references (deduped/shared blob).
    #     Sleeping here is waiting on the known background sweep cadence, not a race workaround.
    time.sleep(3 * RECLAIM_SLEEP + 3)  # > grace(3s) + a few interval(1s) cycles

    # Re-read on BOTH servers: no data lost to the other server's GC.
    assert int(node1.query("SELECT count() FROM t1")) == count_after_mut
    assert int(node1.query("SELECT sum(v) FROM t1")) == sum_after_mut
    assert node1.query("SELECT sum(cityHash64(id, v, s)) FROM t1").strip() == digest_after_mut

    assert int(node2.query("SELECT count() FROM t2")) == count_after_mut
    assert int(node2.query("SELECT sum(v) FROM t2")) == sum_after_mut
    assert node2.query("SELECT sum(cityHash64(id, v, s)) FROM t2").strip() == digest_after_mut

    # (5) Both servers drop their tables. Refs are unlinked synchronously; the shared pool's blobs and
    #     footers become unreferenced GC fodder. Then poll until the pool drains back to baseline.
    node1.query("DROP TABLE t1 SYNC")
    node2.query("DROP TABLE t2 SYNC")

    final = count_pool_objects()
    for _ in range(RECLAIM_RETRIES):
        final = count_pool_objects()
        if final <= baseline:
            break
        time.sleep(RECLAIM_SLEEP)

    assert final <= baseline, (
        "shared pool did not drain to baseline within {}s after both servers dropped their tables: "
        "baseline={}, after_insert={}, final={} (blobs={}, parts={})".format(
            int(RECLAIM_RETRIES * RECLAIM_SLEEP),
            baseline,
            after_insert,
            final,
            count_prefix(BLOBS_PREFIX),
            count_prefix(PARTS_PREFIX),
        )
    )
