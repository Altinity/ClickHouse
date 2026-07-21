import shlex
import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "ref_snaplog"
RO_DISK = "disk_ca_ro"

# Endpoint is http://rustfs1:11121/test/cas_snaplog_data/, so the pool lives under bucket `test`,
# prefix `cas_snaplog_data/`. The snapshot+log ref protocol keeps a table's immutable transaction logs
# and snapshots under cas/refs/, part manifests under cas/manifests/, and content blobs under blobs/.
POOL = "cas_snaplog_data"
BLOBS_PREFIX = POOL + "/blobs/"
REFS_PREFIX = POOL + "/cas/refs/"
MANIFESTS_PREFIX = POOL + "/cas/manifests/"

NUM_ROWS = 20000
NUM_INSERTS = 8

# Background GC runs every 1s with a 2s grace. After DROP TABLE ... SYNC the dropped namespace's content
# (blobs) and part manifests become GC fodder; we poll until they drain. Bounded wait on a known
# background process, not a race hack.
RECLAIM_RETRIES = 120
RECLAIM_SLEEP = 1.0  # total bound ~= 120s


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


def _count(prefix):
    return len(
        list(
            cluster.rustfs_client.list_objects(
                cluster.rustfs_bucket, prefix, recursive=True
            )
        )
    )


def _content_objects():
    # Content blobs + part manifests: the objects the GC fold + condemn/delete pipeline and the
    # namespace-cleanup item reclaim after a namespace is removed.
    return _count(BLOBS_PREFIX) + _count(MANIFESTS_PREFIX)


def _disks(node, query):
    # Run a clickhouse-disks command against the read-only CA window over the same pool.
    return node.exec_in_container(
        [
            "bash",
            "-c",
            "/usr/bin/clickhouse disks -C /etc/clickhouse-server/config.xml "
            "--disk {} --save-logs --query {}".format(RO_DISK, shlex.quote(query)),
        ]
    )


def test_ref_snaplog_lifecycle_reclaims_and_fsck_clean():
    node = cluster.instances["node"]

    for t in ("ref_t1", "ref_t1_renamed", "ref_t2"):
        node.query("DROP TABLE IF EXISTS {} SYNC".format(t))

    content_baseline = _content_objects()

    # (1) Two tables on the CA/rustfs policy.
    for t in ("ref_t1", "ref_t2"):
        node.query(
            "CREATE TABLE {} (id Int64, data String) ENGINE = MergeTree() ORDER BY id "
            "SETTINGS storage_policy = '{}'".format(t, STORAGE_POLICY)
        )

    # (2) Several inserts each -> distinct content blobs + one immutable ref-log transaction per insert.
    for t in ("ref_t1", "ref_t2"):
        for i in range(NUM_INSERTS):
            node.query(
                "INSERT INTO {} SELECT number + {off}, toString(number + {off}) "
                "FROM numbers({rows})".format(t, off=i * NUM_ROWS, rows=NUM_ROWS)
            )

    assert int(node.query("SELECT count() FROM ref_t1")) == NUM_INSERTS * NUM_ROWS
    assert int(node.query("SELECT count() FROM ref_t2")) == NUM_INSERTS * NUM_ROWS

    # The snapshot+log ref format is actually in use: immutable ref objects exist under cas/refs/.
    assert _count(REFS_PREFIX) > 0, "expected ref log/snapshot objects under cas/refs/"
    assert (
        _content_objects() > content_baseline
    ), "expected content objects to rise above baseline after inserts"

    # Read-only consumers agree while data is present: fsck finds no missing objects (dangling) and the
    # snapshot integrity oracle finds no divergence between any published snapshot and its log replay.
    live_fsck = _disks(node, "ca-fsck")
    assert "dangling=0" in live_fsck, live_fsck
    assert "snapshot_oracle_mismatches=0" in live_fsck, live_fsck

    # (3) Rename one table: data must survive (its ref namespace and its logs/snapshots are unaffected).
    node.query("RENAME TABLE ref_t1 TO ref_t1_renamed")
    assert (
        int(node.query("SELECT count() FROM ref_t1_renamed")) == NUM_INSERTS * NUM_ROWS
    )

    # (4) Drop both: the writer appends remove_namespace; background GC folds the -1 edges, condemns and
    #     deletes the now-unreferenced blobs, and runs the namespace-cleanup item that reclaims the
    #     removed namespace's physical @cas@ prefixes (part manifests + verbatim files).
    node.query("DROP TABLE ref_t1_renamed SYNC")
    node.query("DROP TABLE ref_t2 SYNC")

    # (5) Poll until the pool's CONTENT (blobs + part manifests) drains back to baseline: the
    #     authoritative "no content leftovers" proof of the fold + condemn/delete + namespace-cleanup
    #     lifecycle on real rustfs.
    #
    #     NOTE (known limitation, escalated in the Task 13 report): the immutable ref LOG objects under
    #     cas/refs/ of a removed namespace are NOT asserted to drain here. `Gc::cleanupRefObjects` runs
    #     only on FOLD rounds and BEFORE the round republishes the `Removed` snapshot, so on a fully
    #     quiesced pool (no further FOLD rounds after the drops fold) those covered logs persist as debris.
    #     Content reclamation (the data-safety-critical path) is unaffected and is what this test pins.
    final = _content_objects()
    for _ in range(RECLAIM_RETRIES):
        final = _content_objects()
        if final <= content_baseline:
            break
        time.sleep(RECLAIM_SLEEP)

    assert final <= content_baseline, (
        "background GC did not reclaim dropped content within {}s: baseline={}, final={} "
        "(blobs={}, manifests={})".format(
            int(RECLAIM_RETRIES * RECLAIM_SLEEP),
            content_baseline,
            final,
            _count(BLOBS_PREFIX),
            _count(MANIFESTS_PREFIX),
        )
    )

    # (6) Read-only consumers on the reclaimed pool: fsck still reports no dangling and no snapshot-oracle
    #     divergence (the lingering ref logs are debris, never a dangle or an oracle mismatch), and
    #     ca-gc-dryrun runs and reports no pending content deletes.
    final_fsck = _disks(node, "ca-fsck")
    assert "dangling=0" in final_fsck, final_fsck
    assert "snapshot_oracle_mismatches=0" in final_fsck, final_fsck

    dryrun = _disks(node, "ca-gc-dryrun")
    assert "preview_deletes=0" in dryrun, dryrun
