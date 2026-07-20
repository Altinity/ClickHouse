import shlex
import time

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.test_tools import assert_eq_with_retry

cluster = ClickHouseCluster(__file__)

# Both servers mount the SAME content-addressed pool over RustFS (not MinIO -- MinIO cannot serve CA
# pools, see memory), distinct server_root_id (node1/node2) -- exactly the shared-pool model test's
# two-node topology (test_content_addressed_shared_pool), just on rustfs instead of minio (the model
# test predates rustfs support; test_content_addressed_ref_snaplog is the rustfs precedent copied here).
STORAGE_POLICY = "content_addressed_dpm"
RO_DISK = "disk_ca_ro"
CA_DISK = "disk_content_addressed_dpm"

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
    # Run a clickhouse-disks command against the read-only CA window over the same pool -- fsck refuses
    # a writable pool, so it must go through disk_ca_ro (the ref-snaplog integration test's idiom).
    return node.exec_in_container(
        [
            "bash",
            "-c",
            "/usr/bin/clickhouse disks -C /etc/clickhouse-server/config.xml "
            "--disk {} --save-logs --query {}".format(RO_DISK, shlex.quote(query)),
        ]
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

    # (4) Hard-kill node2: SIGKILL, no graceful farewell -- node2's mount lease is left to expire
    #     naturally, exactly the scenario decommission exists for.
    node2.stop_clickhouse(kill=True)

    # (5) Wait until node1 observes node2's mount as no longer live (expired once its lease's TTL
    #     elapses with no renewal, since there was no graceful farewell to mark it terminated instead).
    #     min() because node1 sees the pool through TWO disks (the writable disk + the disk_ca_ro
    #     fsck window), so the mounts table carries one row per disk view for the same srid --
    #     aggregate to a single row for the equality assert.
    assert_eq_with_retry(
        node1,
        "SELECT min(state != 'live') FROM system.content_addressed_mounts WHERE srid = '{}'".format(
            SRID2
        ),
        "1",
        retry_count=90,
        sleep_time=1.0,
    )

    # (6) Decommission the dead member from node1. SYSTEM queries do not accept a FORMAT clause
    #     (ParserSystemQuery is not part of ParserQueryWithOutput), so parse the default TSV row.
    #     Column order matches the interpreter's ColumnsDescription: srid, namespaces_removed,
    #     namespaces_already_removed, committed_refs_removed, precommits_removed,
    #     manifest_debris_removed, staging_objects_removed, mountpoint_objects_removed,
    #     slot_removed, warnings.
    report_tsv = node1.query(
        "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '{}' FROM DISK '{}'".format(SRID2, CA_DISK)
    ).rstrip("\n")
    fields = report_tsv.split("\t")
    assert len(fields) == 10, report_tsv
    assert fields[0] == SRID2, report_tsv
    assert int(fields[1]) >= 1, report_tsv  # namespaces_removed
    assert int(fields[8]) == 1, report_tsv  # slot_removed
    assert fields[9] == "", report_tsv  # warnings

    # (7) node1's own data survives the whole flow untouched.
    assert int(node1.query("SELECT count() FROM t1")) == n1_count
    assert int(node1.query("SELECT sum(id) FROM t1")) == n1_sum

    # (8) node2's srid is gone from the mounts table.
    assert (
        node1.query(
            "SELECT count() FROM system.content_addressed_mounts WHERE srid = '{}'".format(SRID2)
        ).strip()
        == "0"
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
    final = _count(BLOBS_PREFIX)
    for _ in range(RECLAIM_RETRIES):
        final = _count(BLOBS_PREFIX)
        if final <= blobs_baseline:
            break
        time.sleep(RECLAIM_SLEEP)

    assert final <= blobs_baseline, (
        "pool did not drain node2's content within {}s after decommission: "
        "baseline={}, final={}".format(
            int(RECLAIM_RETRIES * RECLAIM_SLEEP), blobs_baseline, final
        )
    )

    fsck = _disks(node1, "fsck")
    assert "dangling=0" in fsck, fsck
    assert "unaccounted=0" in fsck, fsck

    # (10) Re-run the same command: the slot is gone, so this is now an unknown pool member.
    err = node1.query_and_get_error(
        "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '{}' FROM DISK '{}'".format(SRID2, CA_DISK)
    )
    assert "BAD_ARGUMENTS" in err, err


def test_drop_pool_member_rejected_on_readonly_disk():
    # disk_ca_ro is the fail-close guard's target: an observe-only window over the SAME pool (used
    # elsewhere in this test only for fsck). Decommission is a mutating operation, so it must be
    # rejected on this disk exactly like `createTransaction`/GC round/GC rebuild are -- READONLY,
    # not a silent no-op or a crash further down the call chain.
    node1 = cluster.instances["node1"]
    err = node1.query_and_get_error(
        "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'whatever' FROM DISK '{}'".format(RO_DISK)
    )
    assert "read-only" in err, err
