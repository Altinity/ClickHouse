import shlex
import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

STORAGE_POLICY = "ref_snaplog"
RO_DISK = "disk_ca_ro"

# Endpoint is http://rustfs1:11121/test/cas_snaplog_data/, so the pool lives under bucket `test`,
# prefix `cas_snaplog_data/`. The snapshot+log ref protocol keeps a table's immutable transaction logs
# and snapshots under cas/ns/stream/, part manifests under cas/manifests/, and content blobs under blobs/.
POOL = "cas_snaplog_data"
BLOBS_PREFIX = POOL + "/blobs/"
REFS_PREFIX = POOL + "/cas/ns/stream/"
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

    # The snapshot+log ref format is actually in use: immutable ref objects exist under cas/ns/stream/.
    assert _count(REFS_PREFIX) > 0, "expected ref log/snapshot objects under cas/ns/stream/"
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
    # The content count at the moment the refs are unlinked. Under Stage A suppression this is the
    # floor the pool must never fall below, so it is captured before any waiting happens.
    after_drop_content = _content_objects()

    # (5) ##############################################################################
    #     ###  STAGE-A CONTRACT.  RESTORE THE RECLAMATION ASSERTIONS AT STAGE B TASK 7b.   ###
    #     ##############################################################################
    #
    #     This step USED to poll until the pool's CONTENT (blobs + part manifests) drained back to
    #     baseline, and assert `final <= content_baseline`. It cannot, and must not, assert that today.
    #
    #     A GC round may destroy only while holding a frontier proof for EVERY namespace that can hold
    #     a live edge, and Stage A cannot enumerate that set — so `UniversePolicy::kDefault` is
    #     `StageA_Suppressed` (`Gc/CasGc.h`) and production GC reclaims NOTHING for the whole of
    #     Stage A, by design. Task 9's nine-site destructive inventory gates every delete call reachable
    #     from GC, each with its own zero-delete-under-suppression test.
    #
    #     AT TASK 7b, three edits: restore the early-exit poll and `assert final <= content_baseline`
    #     here, delete the suppression-evidence block below, and restore step (6)'s
    #     `preview_deletes=0` expectation to whatever the reclaiming pool then produces.
    #
    #     Until then this asserts the Stage-A truth, and asserts it WITH EVIDENCE rather than by
    #     observing an absence — an absence of reclamation is also what a wedged GC, a lost lease or a
    #     crashed background thread would produce, and those are bugs. Same treatment, and same
    #     reasoning, as `test_content_addressed_gc_s3.py::test_stage_a_gc_is_suppressed_and_says_so`
    #     (Task 9, `afa08749a47`).
    for _ in range(RECLAIM_RETRIES):
        time.sleep(RECLAIM_SLEEP)
    final = _content_objects()

    # Stated as `>= after_drop_content` rather than `> content_baseline` because it is the stronger
    # claim: not "some of it survived" but "none of it went".
    assert final >= after_drop_content, (
        "Stage A must reclaim NOTHING, but the pool's content shrank: baseline={}, "
        "at_drop={}, final={} (blobs={}, manifests={}). If Task 7b has landed, this test is the "
        "thing to update — see the comment above.".format(
            content_baseline,
            after_drop_content,
            final,
            _count(BLOBS_PREFIX),
            _count(MANIFESTS_PREFIX),
        )
    )

    # …AND THE SUPPRESSION IS EVIDENCED, which is what separates "GC declined, by design" from "GC was
    # wedged, crashed, or never held the lease" — all of which also reclaim nothing.
    node.query("SYSTEM FLUSH LOGS")
    rounds = int(
        node.query(
            "SELECT count() FROM system.content_addressed_garbage_collection_log "
            "WHERE event_type = 'Finish' AND outcome = 'Success'"
        ).strip()
    )
    assert rounds > 0, "no successful GC round ran at all — this is not suppression, it is a wedge"

    deleted = int(
        node.query(
            "SELECT sum(objects_deleted + manifests_deleted + entries_redeleted) "
            "FROM system.content_addressed_garbage_collection_log WHERE event_type = 'Finish'"
        ).strip()
        or 0
    )
    assert deleted == 0, (
        "GC's own bookkeeping reports {} deletion(s) while Stage A suppression is in force".format(
            deleted
        )
    )

    # (6) Read-only consumers on the RETAINED pool. The two invariants below hold whether or not
    #     reclamation is suppressed, and they are the ones that matter: unreferenced content that GC has
    #     not removed is retention, never a dangle, and never an oracle divergence.
    final_fsck = _disks(node, "ca-fsck")
    assert "dangling=0" in final_fsck, final_fsck
    assert "snapshot_oracle_mismatches=0" in final_fsck, final_fsck

    #     `ca-gc-dryrun` must still RUN and answer. Its `preview_deletes` count is deliberately NOT
    #     pinned during Stage A: the preview describes the reclamation that suppression withholds, so
    #     the number it reports is a statement about what a Stage-B round would do, not about this one.
    #     AT TASK 7b: restore `assert "preview_deletes=0" in dryrun`.
    dryrun = _disks(node, "ca-gc-dryrun")
    assert "preview_deletes=" in dryrun, dryrun
