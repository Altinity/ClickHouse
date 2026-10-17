import re
import shlex
import time

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.test_tools import assert_eq_with_retry

cluster = ClickHouseCluster(__file__)

# Both servers mount the SAME content-addressed pool over RustFS (not MinIO -- MinIO cannot serve CA
# pools, see memory), distinct server_root_id (node1/node2) -- exactly the shared-pool model test's
# two-node topology (test_cas_shared_pool), just on rustfs instead of minio (the model
# test predates rustfs support; test_cas_ref_snaplog is the rustfs precedent copied here).
STORAGE_POLICY = "cas_dpm"
RO_DISK = "disk_ca_ro"
CA_DISK = "disk_cas_dpm"

SRID1 = "node1"
SRID2 = "node2"

POOL = "cas_dpm_data"
BLOBS_PREFIX = POOL + "/blobs/"

NUM_ROWS = 20000

# Background GC: grace=2s, interval=1s (storage_conf.xml). After the drop-pool-member command removes
# node2's namespaces the content that was only reachable through them becomes unreferenced GC fodder;
# poll until it drains. Bounded wait on a known background process, not a race workaround.
RECLAIM_RETRIES = 120
RECLAIM_SLEEP = 1.0  # total bound ~= 120s


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
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


def _count(prefix):
    return len(
        list(cluster.rustfs_client.list_objects(cluster.rustfs_bucket, prefix, recursive=True))
    )


def _disks(node, query):
    # Run a clickhouse-disks command against the read-only CA window over the same pool — cas-fsck refuses
    # a writable pool, so it must go through disk_ca_ro (the ref-snaplog integration test's idiom).
    return node.exec_in_container(
        [
            "bash",
            "-c",
            "/usr/bin/clickhouse disks -C /etc/clickhouse-server/config.xml "
            "--disk {} --save-logs --query {}".format(RO_DISK, shlex.quote(query)),
        ]
    )


# A stop that lands after the admin's renewal write was sent ends its renewer terminally: the farewell is
# skipped (fail-safe), the run reports rerun with exactly these warnings, and the admin's live body holds the
# slot until GC fences it: about one lease TTL plus a GC round. Tolerated until the stop waits for an in-flight renewal
# before the farewell, a tracked follow-up.
FAREWELL_SKIPPED_BY_A_RACING_STOP = (
    "the slot does not hold this run's farewell; "
    "the retirement is not verified; rerun SYSTEM CAS DROP POOL MEMBER"
)

# The result row of SYSTEM CAS DROP POOL MEMBER, in column order.
REPORT_COLUMNS = [
    "server_root_id",
    "namespaces_removed",
    "namespaces_already_removed",
    "committed_refs_removed",
    "precommits_removed",
    "manifest_debris_removed",
    "staging_objects_removed",
    "mountpoint_objects_removed",
    "member_retired",
    "warnings",
    "namespaces_pending_gc",
]


def _drop_member(node):
    # Retried through the "wait for its lease to lapse" refusal, bounded: the slot may hold the live body
    # FAREWELL_SKIPPED_BY_A_RACING_STOP describes.
    for _ in range(90):
        try:
            report_tsv = node.query(
                "SYSTEM CAS DROP POOL MEMBER '{}' FROM DISK '{}'".format(SRID2, CA_DISK)
            ).rstrip("\n")
        except Exception as e:
            if "alive or contended" not in str(e):
                raise
            time.sleep(1.0)
            continue
        fields = report_tsv.split("\t")
        assert len(fields) == len(REPORT_COLUMNS), report_tsv
        return report_tsv, dict(zip(REPORT_COLUMNS, fields))
    pytest.fail("decommission kept refusing: lease never lapsed within the bound")


def _assert_retired_or_racing_stop(report, report_tsv):
    if int(report["member_retired"]) == 1:
        assert report["warnings"] == "", report_tsv
    else:
        # TSV escapes the apostrophe.
        assert report["warnings"].replace("\\'", "'") == FAREWELL_SKIPPED_BY_A_RACING_STOP, report_tsv


def _srid2_retirement(node1):
    return tuple(
        node1.query(
            "SELECT state, retirement_verified FROM system.cas_mounts "
            "WHERE server_root_id = '{}' AND disk = '{}'".format(SRID2, CA_DISK)
        )
        .strip()
        .split("\t")
    )


def test_drop_dead_pool_member_heals_the_pool():
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]

    node1.query("DROP TABLE IF EXISTS t1 SYNC")
    node2.query("DROP TABLE IF EXISTS t2 SYNC")

    blobs_baseline = _count(BLOBS_PREFIX)

    create_tpl = (
        "CREATE TABLE {tbl} (id Int64, s String) ENGINE = MergeTree() ORDER BY id "
        "SETTINGS storage_policy = '{policy}'"
    )

    # (2) node2 gets its own table with several parts -- this data must NOT survive (node2 is about to
    #     be killed and decommissioned).
    node2.query(create_tpl.format(tbl="t2", policy=STORAGE_POLICY))
    for i in range(4):
        node2.query(
            "INSERT INTO t2 SELECT number + {off}, toString(number + {off}) "
            "FROM numbers({rows})".format(off=i * NUM_ROWS, rows=NUM_ROWS)
        )
    assert int(node2.query("SELECT count() FROM t2")) == 4 * NUM_ROWS

    # (3) node1 gets its own table -- this data MUST survive the whole flow untouched.
    node1.query(create_tpl.format(tbl="t1", policy=STORAGE_POLICY))
    node1.query(
        "INSERT INTO t1 SELECT number, toString(number) FROM numbers({})".format(NUM_ROWS)
    )
    n1_count = int(node1.query("SELECT count() FROM t1"))
    n1_sum = int(node1.query("SELECT sum(id) FROM t1"))
    assert n1_count == NUM_ROWS

    assert _count(BLOBS_PREFIX) > blobs_baseline, "expected content blobs after both nodes' inserts"

    # (3b) T9: system.cas_mounts scopes the GC-health columns (is_leader et al.) to the
    #      row for THIS server's own server_root_id; peer rows read NULL. Background GC (1s interval)
    #      should have led at least one round on each node by now, but that is a background race, not
    #      something this test synchronizes on directly -- poll rather than assume. For every disk
    #      that reports any non-NULL is_leader row there must be exactly one such row, and it must
    #      belong to the querying node's own server_root_id -- never a peer's.
    for node, own_srid in ((node1, SRID1), (node2, SRID2)):
        rows = []
        for _ in range(30):
            rows = (
                node.query(
                    "SELECT disk, server_root_id FROM system.cas_mounts "
                    "WHERE is_leader IS NOT NULL ORDER BY disk"
                )
                .strip()
                .splitlines()
            )
            if rows:
                break
            time.sleep(1.0)
        assert rows, "expected at least one GC-health row with is_leader populated on {}".format(
            node.name
        )
        seen_disks = set()
        for row in rows:
            disk, srid = row.split("\t")
            assert srid == own_srid, "peer server_root_id '{}' leaked GC health on disk '{}': {}".format(
                srid, disk, rows
            )
            assert disk not in seen_disks, "duplicate non-NULL is_leader row for disk '{}': {}".format(
                disk, rows
            )
            seen_disks.add(disk)

    # (4) Hard-kill node2: SIGKILL, no graceful farewell -- node2's mount lease is left to expire
    #     naturally, exactly the scenario decommission exists for.
    node2.stop_clickhouse(kill=True)

    # (5) Wait until node1 observes node2's mount as no longer live (expired once its lease's TTL
    #     elapses with no renewal, since there was no graceful farewell to mark it terminated instead).
    #     min() because node1 sees the pool through TWO disks (the writable disk + the disk_ca_ro
    #     fsck window), so the mounts table carries one row per disk view for the same server_root_id --
    #     aggregate to a single row for the equality assert.
    assert_eq_with_retry(
        node1,
        "SELECT min(state != 'live') FROM system.cas_mounts WHERE server_root_id = '{}'".format(
            SRID2
        ),
        "1",
        retry_count=90,
        sleep_time=1.0,
    )

    # (6) Decommission the dead member from node1 -- PHASE 1 of a two-phase heal. SYSTEM queries do
    #     not accept a FORMAT clause (ParserSystemQuery is not part of ParserQueryWithOutput), so parse
    #     the default TSV row (REPORT_COLUMNS).
    #
    #     t2 is still `Live` at this point, so THIS call's normal drop path is what appends its
    #     removal terminal and moves its catalog row to `Removing` -- catalog deletion stays GC's job
    #     (`224aacd8eb9`), never the decommission command's, so a row this same call just legitimately
    #     transitioned is reported under `namespaces_pending_gc`.
    #     This is a verified retirement with GC completion pending, not a failure.
    #     The mounts-table poll above and the decommission command judge liveness by DIFFERENT
    #     predicates on purpose: the table renders TTL arithmetic over the last observed mount row,
    #     while the command re-reads the mountpoint object and refuses while the lease could still
    #     be live under its conservative safety margin. The destructive side being stricter is the
    #     fail-close direction, so the table saying "not live" does not guarantee the command is
    #     ready yet -- under sanitizer slowdowns the gap is wide enough to hit. Retry the command
    #     itself through the documented "wait for its lease to lapse" refusal, bounded.
    report_tsv, report = _drop_member(node1)
    assert report["server_root_id"] == SRID2, report_tsv
    assert int(report["namespaces_removed"]) >= 1, report_tsv  # the drop half did its work
    assert int(report["namespaces_pending_gc"]) >= 1, report_tsv  # GC owns the row now
    _assert_retired_or_racing_stop(report, report_tsv)

    # (6b) PHASE 2: drive GC and re-run the decommission until no row is pending. Folding a fresh
    #      terminal and pruning its catalog row are two separate GC rounds by design (a fold-then-prune
    #      handoff -- see `CasDecommissionCatalogDuties.FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound`),
    #      so poll rather than assume one round suffices. The explicit `GC RUN` is the same idiom
    #      `test_cas_replicated_relink` uses; it runs a synchronous round regardless of the background
    #      cadence. Catalog-row pruning is Task 5's catalog-only pre-fold drain and does not consult
    #      Stage A's destructive-reclaim suppression, so this heals under the current Stage-A posture.
    for _ in range(30):
        node1.query("SYSTEM CAS GC RUN '{}'".format(CA_DISK))
        report_tsv, report = _drop_member(node1)
        _assert_retired_or_racing_stop(report, report_tsv)  # on every rerun
        if int(report["namespaces_pending_gc"]) == 0 and int(report["member_retired"]) == 1:
            break
    else:
        pytest.fail("pool never healed after driving GC: {}".format(report_tsv))

    # (7) node1's own data survives the whole flow untouched.
    assert int(node1.query("SELECT count() FROM t1")) == n1_count
    assert int(node1.query("SELECT sum(id) FROM t1")) == n1_sum

    # (8) node2's mount slot stays and reads as `retired`: the decommission keeps
    # the control objects and tombstones the owner.
    assert (
        node1.query(
            "SELECT state FROM system.cas_mounts WHERE server_root_id = '{}' AND disk = '{}'".format(
                SRID2, CA_DISK
            )
        ).strip()
        == "retired"
    )

    # (9) Drive GC (node1's background GC is already running against the shared pool) to reclaim
    #     node2's now-unreferenced content, then poll for the blob count to drain back to baseline --
    #     the authoritative "no content leftovers" proof, mirroring the ref-snaplog integration test's
    #     idiom. node1's own t1 is still alive at this point and its blobs legitimately stay in the
    #     pool, so a drain-to-baseline check is only meaningful after t1 is dropped too -- its survival
    #     was already proven byte-for-byte in step 7, so drop it now and demand the pool drain to
    #     EMPTY: node2's content via the decommission, t1's via the ordinary drop, no leftovers from
    #     either. Then a read-only fsck over the drained pool must report clean (no dangling, no
    #     unaccounted objects).
    node1.query("DROP TABLE t1 SYNC")
    at_drop = _count(BLOBS_PREFIX)

    # THE RECLAMATION. Both contributions must go: node2's content via the decommission, t1's via the
    # ordinary drop. Polled with an early exit, then cross-checked against GC's own bookkeeping so that
    # a pool which shrank for some other reason cannot pass for a round that reclaimed it.
    final = _count(BLOBS_PREFIX)
    for _ in range(RECLAIM_RETRIES):
        if final <= blobs_baseline:
            break
        time.sleep(RECLAIM_SLEEP)
        final = _count(BLOBS_PREFIX)

    assert final <= blobs_baseline, (
        "the drained pool did not return to its baseline: baseline={}, at_drop={}, final={}".format(
            blobs_baseline, at_drop, final
        )
    )

    node1.query("SYSTEM FLUSH LOGS")
    rounds = int(
        node1.query(
            "SELECT count() FROM system.cas_gc_log "
            "WHERE event_type = 'Finish' AND outcome = 'Success'"
        ).strip()
        or 0
    )
    assert rounds > 0, "no successful GC round ran at all"
    deleted = int(
        node1.query(
            "SELECT sum(objects_deleted + manifests_deleted) "
            "FROM system.cas_gc_log WHERE event_type = 'Finish'"
        ).strip()
        or 0
    )
    assert deleted > 0, "the pool drained but GC's own bookkeeping reports no deletion"

    # node2's decommissioned-and-healed namespace (t2) left canonical dead-life residue behind: its
    # catalog row is gone (pruned by GC after the drop), but its `_ckpt`/`_files`/`_log`
    # objects are the perpetual namespace janitor's job, not decommission's or GC's own destructive
    # round. t1's row is pruned the same way once dropped. That residue must DRAIN to zero -- the
    # janitor deletes one bounded page per round, so this is polled rather than read once -- and it
    # must never be hard corruption on the way there, which is why `lifeless_keys` is checked on every
    # poll and not only at the end.
    for _ in range(RECLAIM_RETRIES):
        fsck = _disks(node1, "cas-fsck")
        assert "lifeless_keys=0" in fsck, fsck
        janitor_pending_match = re.search(r"\bjanitor_pending=(\d+)", fsck)
        assert janitor_pending_match, "cas-fsck summary is missing the janitor_pending field: {}".format(fsck)
        if int(janitor_pending_match.group(1)) == 0:
            break
        time.sleep(RECLAIM_SLEEP)

    assert "dangling=0" in fsck, fsck
    assert "unaccounted=0" in fsck, fsck
    assert int(janitor_pending_match.group(1)) == 0, (
        "the dead-life residue from the healed decommission and the t1 drop never drained: {}".format(fsck)
    )

    # (10) Re-run the same command: the owner carries a verified retirement, so the rerun is accepted
    #      and reports the member retired again.
    report_tsv, report = _drop_member(node1)
    _assert_retired_or_racing_stop(report, report_tsv)


def test_drop_pool_member_rejected_on_readonly_disk():
    # disk_ca_ro is the fail-close guard's target: an observe-only window over the SAME pool (used
    # elsewhere in this test only for fsck). Decommission is a mutating operation, so it must be
    # rejected on this disk exactly like `createTransaction`/GC round/GC rebuild are -- READONLY,
    # not a silent no-op or a crash further down the call chain.
    node1 = cluster.instances["node1"]
    err = node1.query_and_get_error(
        "SYSTEM CAS DROP POOL MEMBER 'whatever' FROM DISK '{}'".format(RO_DISK)
    )
    assert "read-only" in err, err
