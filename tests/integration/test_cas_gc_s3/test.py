import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "cas_gc_s3"

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


def test_gc_reclaims_dropped_blobs():
    """
    The background GC reclaims a dropped table's blobs and part footers.

    A GC round may destroy only while holding a frontier proof for EVERY namespace that can hold a live
    edge — reachability is a property of the whole pool, so deleting one blob asserts something about
    every namespace at once, including the ones the round never looked at. The catalog supplies that set,
    and each namespace's proof is one exact-key read at its cursor's successor.

    So the reclamation below is asserted TOGETHER WITH the gate's own reason for permitting it: rounds
    ran, every namespace in the universe reached a proven frontier, and no destructive phase reported
    itself suppressed. Without that, a pool that shrank for some unrelated reason would read as a pass.
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

    # (4) Poll for the reclamation, exiting as soon as it has happened (grace=1s, interval=1s, so
    #     plenty of rounds run within the window).
    final = count_pool_objects()
    for _ in range(RECLAIM_RETRIES):
        if final <= baseline:
            break
        time.sleep(RECLAIM_SLEEP)
        final = count_pool_objects()

    # (5) The dropped table's objects are GONE, back to the pre-table baseline.
    assert final <= baseline, (
        "the dropped table's objects were not reclaimed: baseline={}, after_insert={}, final={} "
        "(blobs={}, parts={})".format(
            baseline,
            after_insert,
            final,
            count_prefix(BLOBS_PREFIX),
            count_prefix(PARTS_PREFIX),
        )
    )

    # (6) …AND FOR THE RIGHT REASON, which is what separates "the gate opened on a proven frontier"
    #     from "the pool shrank for some other reason".
    #
    #     (a) Rounds actually ran and completed as the leader.
    rounds = gc_log_scalar(
        node,
        "SELECT count() FROM system.cas_gc_log "
        "WHERE event_type = 'Finish' AND outcome = 'Success'",
    )
    assert rounds > 0, "no successful GC round ran at all — this is not suppression, it is a wedge"

    #     (b) The rounds report the deletion on their OWN bookkeeping, not only on the S3 object count.
    deleted = gc_log_scalar(
        node,
        "SELECT sum(objects_deleted + manifests_deleted) "
        "FROM system.cas_gc_log WHERE event_type = 'Finish'",
    )
    assert deleted > 0, "the pool shrank but no round reported deleting anything"

    #     (c) At least one round proved EVERY namespace in its universe (frontier_proven ==
    #         frontier_namespaces, both nonzero). A round held up by a clamp, a hold or an exhausted
    #         probe budget would have frontier_proven < frontier_namespaces and could not have opened
    #         the gate.
    fully_proven_rounds = gc_log_scalar(
        node,
        "SELECT count() FROM system.cas_gc_log "
        "WHERE phase = 'fold_ref_intake' "
        "  AND phase_metrics['frontier_namespaces'] > 0 "
        "  AND phase_metrics['frontier_proven'] = phase_metrics['frontier_namespaces']",
    )
    assert fully_proven_rounds > 0, (
        "no round reached a fully proven frontier, so whatever removed those objects was not a round "
        "acting on a complete frontier"
    )

    #     (d) And no destructive phase reported itself suppressed — the gate really did open.
    suppressed_phases = gc_log_scalar(
        node,
        "SELECT uniqExact(phase) FROM system.cas_gc_log "
        "WHERE phase IN {} AND phase_metrics['suppressed'] = 1".format(DESTRUCTIVE_PHASES),
    )
    assert suppressed_phases == 0, (
        "a destructive phase reported itself suppressed on a pool whose frontier was complete"
    )


# Each restart is a remount: the writer epoch advances and recovery closes every table's previous
# epoch with a seal. Restarts with and without an insert in between give the table epochs that hold
# rows and epochs that hold nothing but their seal, the shape a long-lived pool accumulates.
EPOCH_CHANGES_WITH_INSERT = 3
EPOCH_CHANGES_IDLE = 2


def read_ahead_events(node):
    rows = node.query(
        "SELECT event, value FROM system.events "
        "WHERE event IN ('CASGCReadAheadHit', 'CASGCReadAheadWasted') "
        "SETTINGS system_events_show_zero_values = 1"
    )
    events = dict(line.split("\t") for line in rows.strip().splitlines())
    return int(events.get("CASGCReadAheadHit", 0)), int(
        events.get("CASGCReadAheadWasted", 0)
    )


def test_gc_read_ahead_wastes_nothing_across_epoch_changes():
    """
    With read-ahead on (the default `cas_gc_io_concurrency`), the GC's ref-log walks hint only ids
    the round's LIST returned. A walk learns where an epoch ends only by decoding its seal, so hinting
    by arithmetic alone requested a window of ids past every seal that never exist -- once per epoch per
    walk, and epochs only accumulate. Across several epoch changes nothing may be wasted, the walks must
    still be read ahead, and GC must still reclaim the dropped table.
    """
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_gc_epochs SYNC")
    baseline = count_pool_objects()
    node.query(
        "CREATE TABLE cas_gc_epochs (id Int64, data String) ENGINE = MergeTree() ORDER BY id "
        "SETTINGS storage_policy = '{}'".format(STORAGE_POLICY)
    )

    def insert(offset):
        node.query(
            "INSERT INTO cas_gc_epochs SELECT number + {offset}, toString(number) "
            "FROM numbers({rows})".format(offset=offset, rows=NUM_ROWS)
        )

    inserts = 0
    for _ in range(NUM_INSERTS):
        insert(inserts * NUM_ROWS)
        inserts += 1
    for _ in range(EPOCH_CHANGES_WITH_INSERT):
        node.restart_clickhouse()
        insert(inserts * NUM_ROWS)
        inserts += 1
    for _ in range(EPOCH_CHANGES_IDLE):
        node.restart_clickhouse()

    assert int(node.query("SELECT count() FROM cas_gc_epochs")) == inserts * NUM_ROWS

    # The counters are per process, so they cover only rounds since the last restart -- each of which
    # walks the table's whole history, across every epoch above.
    hits, wasted = read_ahead_events(node)
    for _ in range(RECLAIM_RETRIES):
        if hits > 0:
            break
        time.sleep(RECLAIM_SLEEP)
        hits, wasted = read_ahead_events(node)
    assert hits > 0, "no GC walk was read ahead, so this test would prove nothing"
    assert wasted == 0, "read-ahead requested {} keys it never took".format(wasted)

    node.query("DROP TABLE cas_gc_epochs SYNC")
    final = count_pool_objects()
    for _ in range(RECLAIM_RETRIES):
        if final <= baseline:
            break
        time.sleep(RECLAIM_SLEEP)
        final = count_pool_objects()
    assert final <= baseline, (
        "the dropped table's objects were not reclaimed: baseline={}, final={}".format(
            baseline, final
        )
    )

    hits, wasted = read_ahead_events(node)
    assert wasted == 0, "read-ahead requested {} keys it never took".format(wasted)
