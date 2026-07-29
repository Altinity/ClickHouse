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
    # RustFS (not MinIO) backs the pool: the CA mount capability probe requires enforced
    # conditional-DELETE semantics, which MinIO OSS lacks — the fail-closed probe aborted server
    # startup there (PR#2073 CI triage). Both instances reach the shared rustfs1 and load the
    # identical storage_conf.xml, so both mount the SAME shared pool.
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node1.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "node2",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node2.xml"],
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

    at_drop = count_pool_objects()

    # ##################################################################################
    # ###  STAGE-A CONTRACT.  RESTORE THE RECLAMATION ASSERTIONS AT STAGE B TASK 7b.  ###
    # ##################################################################################
    # This USED to poll until the pool drained and assert `final <= baseline`. It cannot, and must not,
    # assert that today: a GC round may destroy only while holding a frontier proof for EVERY namespace
    # that can hold a live edge, Stage A cannot enumerate that set, so `UniversePolicy::kDefault` is
    # `StageA_Suppressed` (`Gc/CasGc.h`) and production GC reclaims NOTHING for the whole of Stage A,
    # by design. AT TASK 7b: restore the early-exit poll and the `final <= baseline` assertion, and delete
    # the suppression-evidence block below. Until then this asserts the Stage-A truth WITH EVIDENCE
    # rather than by observing an absence — a wedged GC, a lost lease or a crashed background thread
    # also reclaim nothing, and those are bugs. Same treatment as Task 9's
    # `test_content_addressed_gc_s3.py::test_stage_a_gc_is_suppressed_and_says_so` (`afa08749a47`).
    for _ in range(RECLAIM_RETRIES):
        time.sleep(RECLAIM_SLEEP)
    final = count_pool_objects()

    assert final >= at_drop, (
        "Stage A must reclaim NOTHING, but the shared pool shrank after both servers dropped: "
        "baseline={}, after_insert={}, at_drop={}, final={} (blobs={}, parts={})".format(
            baseline,
            after_insert,
            at_drop,
            final,
            count_prefix(BLOBS_PREFIX),
            count_prefix(PARTS_PREFIX),
        )
    )


# Crash-resilience uses a SMALLER, DISTINCT dataset per node. Distinct content => node1's blobs are
# NOT deduped with node2's, so "node2's GC must not reclaim node1's blobs while node1 is down" is a
# real, observable invariant on the pool object count (node1's blobs cannot hide behind node2's).
CRASH_ROWS = 50000


def test_pool_survives_node_crash():
    # Proves the bucket is self-describing and the pool survives a hard node crash:
    #   (a) the surviving node keeps running with background GC on and loses no data;
    #   (b) the hard-killed node recovers its data on restart (refs are durable in the bucket);
    #   (c) any orphaned write-session the crash left behind is eventually reclaimed (its lease
    #       expires; the pool drains to baseline after DROP).
    # Lock-fencing safety (paused GC leader fenced by a peer's higher fence token) is covered by the
    # gtest SweepStopsWhenLeadershipLost; this test focuses on crash-resilience.
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]

    node1.query("DROP TABLE IF EXISTS crash1 SYNC")
    node2.query("DROP TABLE IF EXISTS crash2 SYNC")

    # (0) Baseline pool object count before either table exists.
    baseline = count_pool_objects()

    create_tpl = (
        "CREATE TABLE {tbl} (id Int64, v UInt64, s String) "
        "ENGINE = MergeTree() ORDER BY id "
        "SETTINGS storage_policy = '{policy}'"
    )
    node1.query(create_tpl.format(tbl="crash1", policy=STORAGE_POLICY))
    node2.query(create_tpl.format(tbl="crash2", policy=STORAGE_POLICY))

    # (1) DISTINCT deterministic data per node (different `v` recipe => different content blobs, so
    #     node1's blobs are NOT shared with node2's and cannot be hidden behind dedup).
    node1.query(
        "INSERT INTO crash1 SELECT number, number * 10, toString(number) "
        "FROM numbers({rows})".format(rows=CRASH_ROWS)
    )
    node2.query(
        "INSERT INTO crash2 SELECT number, number * 7, concat('n2_', toString(number)) "
        "FROM numbers({rows})".format(rows=CRASH_ROWS)
    )

    # Capture node1's authoritative aggregates BEFORE the crash; recovery must reproduce them exactly.
    n1_count = int(node1.query("SELECT count() FROM crash1"))
    n1_sum_id = int(node1.query("SELECT sum(id) FROM crash1"))
    n1_digest = node1.query("SELECT sum(cityHash64(id, v, s)) FROM crash1").strip()
    assert n1_count == CRASH_ROWS

    n2_count = int(node2.query("SELECT count() FROM crash2"))
    n2_digest = node2.query("SELECT sum(cityHash64(id, v, s)) FROM crash2").strip()
    assert n2_count == CRASH_ROWS

    # The pool now holds BOTH nodes' (distinct) blobs. Remember this high-water mark: after node1 is
    # killed, node2's GC must NOT shrink the pool below the level needed to hold node1's blobs.
    after_both_inserts = count_pool_objects()
    assert after_both_inserts > baseline

    # (2) HARD-KILL node1 (SIGKILL via pkill -9 => simulated crash). node2 stays up. A crash mid-flight
    #     can leave node1 holding an unreleased write-session lease (an orphaned pin) on the pool.
    node1.stop_clickhouse(kill=True)

    # (3) With node1 down, keep node2 working AND let node2's background GC run several sweep cycles.
    #     Two invariants:
    #       - node2 reads its OWN data correctly (no loss while it owns the pool alone);
    #       - node2's GC does NOT reclaim node1's blobs: node1's refs are durable roots in the bucket
    #         even though node1's process is gone. Sleeping here waits on the known sweep cadence.
    node2.query(
        "INSERT INTO crash2 SELECT number, number * 7, concat('n2b_', toString(number)) "
        "FROM numbers({rows})".format(rows=CRASH_ROWS)
    )
    node2.query("OPTIMIZE TABLE crash2 FINAL")
    n2_count_after = int(node2.query("SELECT count() FROM crash2"))
    assert n2_count_after == 2 * CRASH_ROWS

    time.sleep(3 * RECLAIM_SLEEP + 3)  # > grace(3s) + a few interval(1s) cycles of node2's GC

    # node2 lost nothing.
    assert int(node2.query("SELECT count() FROM crash2")) == n2_count_after
    # node1's blobs were NOT swept by node2's GC: the pool still holds at least node1's portion. node1
    # contributed (after_both_inserts - baseline) objects on top of the empty baseline, so even if
    # node2 had reclaimed every one of its own blobs the pool could not have dropped below that.
    node1_contribution = after_both_inserts - baseline
    pool_with_node1_down = count_pool_objects()
    assert pool_with_node1_down >= baseline + node1_contribution, (
        "node2's GC appears to have reclaimed node1's durable refs while node1 was down: "
        "baseline={}, after_both_inserts={}, node1_contribution={}, pool_now={}".format(
            baseline, after_both_inserts, node1_contribution, pool_with_node1_down
        )
    )

    # (4) RESTART node1. The bucket is self-describing: node1 rebuilds its active set from the durable
    #     refs and must re-read its table with the EXACT pre-crash count/sum/digest. After a hard kill
    #     the harness reconnects on start_clickhouse via wait_start; use the instance object fresh.
    #     150s, not the 60s default: a post-SIGKILL restart legitimately pays the unclean-reclaim
    #     cost before serving — the stale-token observation window over its own unexpired lease
    #     (~TTL + 5% + renew_period/2 ≈ 36.5s with defaults) plus the materialization grace
    #     (30s default) plus the lease re-write; ~71s observed end-to-end. A bounded wait on a
    #     known, by-design recovery protocol — not a race hack.
    node1.start_clickhouse(start_wait_sec=150)

    assert int(node1.query("SELECT count() FROM crash1")) == n1_count
    assert int(node1.query("SELECT sum(id) FROM crash1")) == n1_sum_id
    assert node1.query("SELECT sum(cityHash64(id, v, s)) FROM crash1").strip() == n1_digest

    # node2 still consistent after node1 rejoined.
    assert int(node2.query("SELECT count() FROM crash2")) == n2_count_after

    # (5) DROP both tables. Refs are unlinked synchronously; the orphaned write-session that node1's
    #     crash left behind no longer pins anything once its lease expires, so GC (run by either
    #     server) reclaims the lot. Bounded-poll the pool until it drains back to baseline.
    node1.query("DROP TABLE crash1 SYNC")
    node2.query("DROP TABLE crash2 SYNC")

    at_drop = count_pool_objects()

    # ##################################################################################
    # ###  STAGE-A CONTRACT.  RESTORE THE RECLAMATION ASSERTIONS AT STAGE B TASK 7b.  ###
    # ##################################################################################
    # Same reasoning as the first test in this file: `UniversePolicy::kDefault` is `StageA_Suppressed`,
    # so GC reclaims nothing for the whole of Stage A. What this test still pins, and what it was
    # really about, is that the hard kill left nothing EXTRA behind: the pool does not keep growing
    # once both tables are dropped and the orphaned write-session's lease expires. AT TASK 7b: restore
    # the early-exit poll and `assert final <= baseline`.
    for _ in range(RECLAIM_RETRIES):
        time.sleep(RECLAIM_SLEEP)
    final = count_pool_objects()

    assert final >= at_drop, (
        "Stage A must reclaim NOTHING, but the shared pool shrank after the crash + both DROPs: "
        "baseline={}, after_both_inserts={}, at_drop={}, "
        "final={} (blobs={}, parts={})".format(
            baseline,
            after_both_inserts,
            at_drop,
            final,
            count_prefix(BLOBS_PREFIX),
            count_prefix(PARTS_PREFIX),
        )
    )
