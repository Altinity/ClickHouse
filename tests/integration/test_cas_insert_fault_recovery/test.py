import time

import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

# Two replicas of one ReplicatedMergeTree on a SHARED content-addressed pool.
STORAGE_POLICY = "content_addressed_shared"


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node1.xml"],
        macros={"replica": "node1"},
        with_rustfs=True,
        with_zookeeper=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "node2",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node2.xml"],
        macros={"replica": "node2"},
        with_rustfs=True,
        with_zookeeper=True,
        stay_alive=True,
    )

    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def _wait_until(predicate, timeout=180, interval=2, desc=""):
    # Condition-based wait (systematic-debugging): the ordinary lost-part recovery is asynchronous
    # (part-check retry/backoff), so gating on a fixed-timeout `SYSTEM SYNC REPLICA` is inherently flaky —
    # that call blocks on the very recovery we are waiting for. Poll the actual OUTCOME instead, with a
    # generous cap. Transient errors while node1 is mid-restart are swallowed and retried.
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
        except Exception as e:  # node briefly unavailable during restart, etc.
            last = e
        if last is True:
            return
        time.sleep(interval)
    raise AssertionError("timed out after {}s waiting for: {} (last={!r})".format(timeout, desc, last))


def test_post_multi_termination_uses_ordinary_lost_part_recovery(start_cluster):
    # [TXN-ONE-PIPELINE Audit 4] After Phases 1-2, CA publication happens ONLY inside the disk commit
    # (`metadata_transaction->commit`). The failpoint `disk_object_storage_fail_commit_metadata_transaction`
    # throws in `DiskObjectStorageTransaction::commit` BEFORE that call, i.e. AFTER the Keeper multi already
    # added the part to node1's ZK part-set but BEFORE any blob / manifest / ref became durable. For a
    # sole-origin INSERT nothing is durable anywhere, so node2 has nothing to fetch (a byte download or a
    # fetch-by-relink both need content in the pool, and none was published).
    #
    # This is a REGRESSION test, not a new recovery mechanism: the spec (§Why There Is No Disk Precommit)
    # explicitly accepts that this window may lose the last copy of a part. What we assert is that CA rides
    # the ORDINARY missing-part path exactly like plain MergeTree — node1 restarts cleanly (no CA-specific
    # hang / wedge / exception), the replication queue drains, and the lost part is replaced by an empty
    # covering part (ProfileEvent `ReplicatedDataLoss` bumped). There is no SILENT data loss: the client
    # already received the INSERT error. node2's interserver-fetch attempt failing with `FILE_DOESNT_EXIST`
    # ("no ref for .../checksums.txt") is EXPECTED (node1 correctly cannot serve a part it never published);
    # it must be tolerated, not asserted against. The regression guard is: NO `LOGICAL_ERROR`, no
    # CA-specific exception, no wedge.
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]

    node1.query("DROP TABLE IF EXISTS t SYNC")
    node2.query("DROP TABLE IF EXISTS t SYNC")

    create = (
        "CREATE TABLE t (a UInt64) ENGINE = ReplicatedMergeTree('/clickhouse/tables/t', '{{replica}}') "
        "ORDER BY a SETTINGS storage_policy = '{policy}'"
    ).format(policy=STORAGE_POLICY)
    node1.query(create)
    node2.query(create)

    def loss_count():
        return int(
            node1.query(
                "SELECT sum(value) FROM system.events WHERE event = 'ReplicatedDataLoss'"
            )
            or 0
        )

    loss_before = loss_count()

    # Force the disk commit to throw AFTER the Keeper multi succeeds on node1 (ONCE failpoint).
    node1.query("SYSTEM ENABLE FAILPOINT disk_object_storage_fail_commit_metadata_transaction")

    # The INSERT's Keeper multi commits the part to ZK, then the disk commit throws -> the INSERT errors and
    # the part is in ZK but its content never became durable anywhere.
    node1.query_and_get_error("INSERT INTO t VALUES (1)")

    # Restart node1: on load `checkPartsImpl` sees the part in ZK / absent on disk -> enqueue -> the
    # part-check finds no source on node2 -> ordinary lost-part handling (empty covering part). Async: this
    # can take a while (retry/backoff), so we poll the outcome rather than gate on a fixed SYNC-REPLICA
    # timeout (which was the original flake).
    node1.restart_clickhouse()

    def node1_recovered():
        # Ordinary lost-part recovery completed on node1: the part was declared lost on all replicas
        # (ReplicatedDataLoss bumped), it was replaced by an empty covering part (0 rows), and node1's
        # replication queue drained (no stuck GET_PART for the lost part -> no CA-specific wedge).
        loss = loss_count()
        cnt = node1.query("SELECT count() FROM t").strip()
        queue = node1.query(
            "SELECT count() FROM system.replication_queue WHERE table = 't'"
        ).strip()
        return loss > loss_before and cnt == "0" and queue == "0"

    _wait_until(
        node1_recovered,
        timeout=180,
        desc="node1 ordinary lost-part recovery (ReplicatedDataLoss bumped, empty cover, queue drained)",
    )

    def node2_converged():
        # node2 converges to the same empty-cover state with a drained queue (the failing GET_PART is
        # superseded by the empty covering part replicated from node1).
        cnt = node2.query("SELECT count() FROM t").strip()
        queue = node2.query(
            "SELECT count() FROM system.replication_queue WHERE table = 't'"
        ).strip()
        return cnt == "0" and queue == "0"

    _wait_until(node2_converged, timeout=180, desc="node2 converges to empty-cover state, queue drained")

    # The regression guard: no CA-specific exception / LOGICAL_ERROR left either server wedged. The
    # expected `FILE_DOESNT_EXIST` interserver miss is tolerated (it is not a LOGICAL_ERROR).
    for node in (node1, node2):
        assert not node.contains_in_log(
            "LOGICAL_ERROR"
        ), "unexpected LOGICAL_ERROR in {}'s log — a CA-specific failure, not ordinary lost-part recovery".format(
            node.name
        )

    # Server is healthy (no wedge) and the ONCE failpoint is consumed: a fresh INSERT now succeeds end to
    # end and replicates to both replicas.
    node1.query("INSERT INTO t VALUES (2)")

    def replicated_one_row():
        return (
            node1.query("SELECT count() FROM t").strip() == "1"
            and node2.query("SELECT count() FROM t").strip() == "1"
        )

    _wait_until(replicated_one_row, timeout=120, desc="post-recovery INSERT replicates to both replicas")

    node1.query("DROP TABLE IF EXISTS t SYNC")
    node2.query("DROP TABLE IF EXISTS t SYNC")
