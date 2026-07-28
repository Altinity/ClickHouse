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
# footers/blobs become unreferenced. How long we give the background GC to do its rounds; this is
# waiting on a known background process, not papering over a race.
RECLAIM_RETRIES = 60
RECLAIM_SLEEP = 1.0  # seconds; total bound ~= RECLAIM_RETRIES * RECLAIM_SLEEP = 60s

# The destructive phases of a GC round. Every one of them stamps `suppressed` into its phase_metrics,
# which is how a suppressed round says so in a queryable way rather than only in the text log.
DESTRUCTIVE_PHASES = (
    "handoff_reclaim",
    "manifest_deletes",
    "namespace_cleanup",
    "ref_object_cleanup",
    "orphan_sweep",
)


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
    # Both content blobs and part footers count: reclamation means BOTH drain.
    return count_prefix(BLOBS_PREFIX) + count_prefix(PARTS_PREFIX)


def gc_log_scalar(node, query):
    node.query("SYSTEM FLUSH LOGS")
    return int(node.query(query).strip())


def test_stage_a_gc_is_suppressed_and_says_so():
    """
    ####################################################################################
    ###  STAGE-A CONTRACT.  RESTORE THE RECLAMATION ASSERTIONS AT STAGE B TASK 7b.   ###
    ####################################################################################

    This test USED to assert that the background GC reclaims a dropped table's blobs and part
    footers (`final <= baseline`). It cannot, and must not, assert that today.

    A GC round may destroy only while holding a frontier proof for EVERY namespace that can hold a
    live edge — reachability is a property of the whole pool, so deleting one blob asserts something
    about every namespace at once, including the ones the round never looked at. Stage A cannot
    enumerate that set: the universe is `(sealed cursors) UNION (this round's LIST hint)`, and both
    can omit a namespace at the same time. So `UniversePolicy::kDefault` is `StageA_Suppressed`
    (src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h) and **production GC
    reclaims nothing for the whole of Stage A, by design**.

    Stage B's catalog makes the universe knowable, and its Task 7b flips that one constant. AT THAT
    POINT, three edits: restore the early-exit poll and `assert final <= baseline` at step (4), delete
    the suppression-evidence block (5)-(6), and rename this test back to
    `test_gc_reclaims_dropped_blobs`.

    Until then this asserts the Stage-A truth, and asserts it with EVIDENCE rather than by observing
    an absence — an absence of reclamation is also what a wedged GC, a lost lease, or a crashed
    background thread would produce. The evidence separates those: the rounds ran, every namespace
    reached a proven frontier, and destruction was suppressed anyway. That combination is reachable
    only through the universe seam.
    """
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

    # (4) Give the background GC the same window it used to be given to reclaim (grace=1s,
    #     interval=1s), so plenty of rounds run. AT TASK 7b: restore the early-exit poll and the
    #     `assert final <= baseline` that used to close this test.
    for _ in range(RECLAIM_RETRIES):
        time.sleep(RECLAIM_SLEEP)
    final = count_pool_objects()

    # (5) STAGE-A TRUTH: nothing was reclaimed. Stated as `>= after_insert` rather than `> baseline`
    #     because it is the stronger claim — not "some of it survived" but "none of it went".
    assert final >= after_insert, (
        "Stage A must reclaim NOTHING, but the pool shrank: baseline={}, after_insert={}, final={} "
        "(blobs={}, parts={}). If Task 7b has landed, this test is the thing to update — see its "
        "docstring.".format(
            baseline,
            after_insert,
            final,
            count_prefix(BLOBS_PREFIX),
            count_prefix(PARTS_PREFIX),
        )
    )

    # (6) …AND THE SUPPRESSION IS EVIDENCED, which is what separates "GC declined, by design" from
    #     "GC was wedged, crashed, or never held the lease" — all of which also reclaim nothing.
    #
    #     (a) Rounds actually ran and completed as the leader.
    rounds = gc_log_scalar(
        node,
        "SELECT count() FROM system.content_addressed_garbage_collection_log "
        "WHERE event_type = 'Finish' AND outcome = 'Success'",
    )
    assert rounds > 0, "no successful GC round ran at all — this is not suppression, it is a wedge"

    #     (b) No round deleted anything, on its own bookkeeping rather than on the S3 object count.
    deleted = gc_log_scalar(
        node,
        "SELECT sum(objects_deleted + manifests_deleted + entries_redeleted) "
        "FROM system.content_addressed_garbage_collection_log WHERE event_type = 'Finish'",
    )
    assert deleted == 0, "a Stage-A round reported destructive work: {}".format(deleted)

    #     (c) THE LOAD-BEARING ONE. At least one round proved EVERY namespace in its universe
    #         (frontier_proven == frontier_namespaces, both nonzero) and suppressed anyway. A round
    #         held up by a clamp, a hold, or an exhausted probe budget would have
    #         frontier_proven < frontier_namespaces; only the universe seam suppresses a round whose
    #         per-namespace proofs are all green. This is the assertion that pins WHY.
    fully_proven_rounds = gc_log_scalar(
        node,
        "SELECT count() FROM system.content_addressed_garbage_collection_log "
        "WHERE phase = 'fold_ref_intake' "
        "  AND phase_metrics['frontier_namespaces'] > 0 "
        "  AND phase_metrics['frontier_proven'] = phase_metrics['frontier_namespaces']",
    )
    assert fully_proven_rounds > 0, (
        "no round reached a fully proven frontier, so the absence of reclamation is NOT the Stage-A "
        "universe seam — something per-round is holding GC up and needs investigating"
    )

    #     (d) And the destructive phases said `suppressed` in their own metrics.
    suppressed_phases = gc_log_scalar(
        node,
        "SELECT uniqExact(phase) FROM system.content_addressed_garbage_collection_log "
        "WHERE phase IN {} AND phase_metrics['suppressed'] = 1".format(DESTRUCTIVE_PHASES),
    )
    assert suppressed_phases > 0, (
        "no destructive phase reported itself suppressed; the gate is not being consulted where it "
        "is supposed to be"
    )
