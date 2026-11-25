import re
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
BLOB_INTENTS_PREFIX = "cas_gc_data/blob-intents/"

# What `system.blob_storage_log` records per reclaimed blob: the intent `PUT` that condemns it, the body `DELETE`
# that reclaims it and the intent `DELETE` that follows. The `HEAD`s of the two decisions are not logged there.
LOGGED_REQUESTS_PER_RECLAIMED_BLOB = {
    ("Upload", "intent"): 1,
    ("Delete", "body"): 1,
    ("Delete", "intent"): 1,
}

# A blob body key or a deletion intent key, reduced to the blob's `<algo>/<shard>/<hex>` path.
BLOB_BODY_PATH = re.compile(r"(?:^|/)blobs/(?P<blob>[^/]+/[0-9a-f]{2}/[0-9a-f]+)$")
BLOB_INTENT_PATH = re.compile(r"(?:^|/)blob-intents/(?P<blob>[^/]+/[0-9a-f]{2}/[0-9a-f]+)/[^/]+/[0-9a-f]+$")

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
        main_configs=["configs/storage_conf.xml", "configs/blob_storage_log.xml"],
        with_rustfs=True,
        stay_alive=True,
    )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def list_keys(prefix):
    objects = cluster.rustfs_client.list_objects(
        cluster.rustfs_bucket, prefix, recursive=True
    )
    return {o.object_name for o in objects}


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


def test_gc_requests_per_reclaimed_blob():
    """
    The requests the pool's GC logs per reclaimed blob are exactly the three the intent protocol sends.

    `system.blob_storage_log` records uploads and deletes, not `HEAD`s: the intent `PUT`, the body `DELETE` and the
    intent `DELETE`. A fourth request means a repeated create or delete; a missing one means the body or its
    intent was removed some other way. Every blob of this test's own table is counted, and each was reclaimed.
    """
    node = cluster.instances["node"]

    node.query("DROP TABLE IF EXISTS cas_gc_requests SYNC")
    node.query(
        "CREATE TABLE cas_gc_requests (id Int64, data String) ENGINE = MergeTree() ORDER BY id "
        "SETTINGS storage_policy = '{}'".format(STORAGE_POLICY)
    )

    blobs_before = list_keys(BLOBS_PREFIX)
    for i in range(3):
        node.query(
            "INSERT INTO cas_gc_requests "
            "SELECT number + {offset}, toString(number + {offset}) FROM numbers({rows})".format(
                offset=(1 + i) * 10**9, rows=NUM_ROWS
            )
        )
    created_keys = list_keys(BLOBS_PREFIX) - blobs_before
    assert created_keys, "the inserts created no blob"
    created = {
        BLOB_BODY_PATH.search(key).group("blob") for key in created_keys
    }

    # The window starts before the drop: nothing the insert logged is counted.
    window_start = int(node.query("SELECT toUnixTimestamp64Micro(now64(6))").strip())
    node.query("DROP TABLE cas_gc_requests SYNC")

    def intents_of_created():
        return {
            key
            for key in list_keys(BLOB_INTENTS_PREFIX)
            if (m := BLOB_INTENT_PATH.search(key)) and m.group("blob") in created
        }

    # Reclaimed: every body gone and every intent of those bodies removed, so the whole ledger has been spent.
    for _ in range(RECLAIM_RETRIES):
        if not (list_keys(BLOBS_PREFIX) & created_keys) and not intents_of_created():
            break
        time.sleep(RECLAIM_SLEEP)
    assert not (list_keys(BLOBS_PREFIX) & created_keys), "bodies were not reclaimed"
    assert not intents_of_created(), "intents of the reclaimed bodies were not removed"

    node.query("SYSTEM FLUSH LOGS")
    rows = node.query(
        "SELECT event_type, remote_path FROM system.blob_storage_log "
        "WHERE toUnixTimestamp64Micro(event_time_microseconds) >= {} "
        "AND event_type IN ('Upload', 'Delete') AND error_code = 0 "
        "FORMAT TSV".format(window_start)
    )
    requests = {blob: {} for blob in created}
    for row in rows.splitlines():
        event_type, remote_path = row.split("\t")
        body = BLOB_BODY_PATH.search(remote_path)
        intent = BLOB_INTENT_PATH.search(remote_path)
        match = body or intent
        if not match or match.group("blob") not in requests:
            continue
        kind = (event_type, "body" if body else "intent")
        counts = requests[match.group("blob")]
        counts[kind] = counts.get(kind, 0) + 1

    assert requests, "the inserts created no blob"
    wrong = {
        blob: counts
        for blob, counts in requests.items()
        if counts != LOGGED_REQUESTS_PER_RECLAIMED_BLOB
    }
    assert not wrong, "logged requests per reclaimed blob differ from {}: {}".format(
        LOGGED_REQUESTS_PER_RECLAIMED_BLOB, wrong
    )
