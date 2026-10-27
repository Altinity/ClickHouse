import shlex
import threading
import time

import pytest
from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

DISK = "disk_cas_shared"


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node1",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_node1.xml", "configs/quiet_system_logs.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def _wait(pred, timeout=120, desc=""):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if pred():
                return
        except Exception:
            pass
        time.sleep(1)
    raise AssertionError("timeout: " + desc)


def _cli_rows(node, query):
    # SYSTEM queries take no FORMAT clause; the format is a client option. Columns are looked up by header name.
    out = node.exec_in_container(
        ["bash", "-c", "clickhouse client --format TSVWithNames --query " + shlex.quote(query)]
    ).strip().split("\n")
    header = out[0].split("\t")
    return [dict(zip(header, line.split("\t"))) for line in out[1:]]


def _assert_fsck_clean(node):
    # Same shape as tests/queries/0_stateless/04290_cas_no_leftovers.sh: drain the GC pipeline first (the
    # dropped temporary ref leaves unreachable blobs that GC has not retired yet), then audit.
    def drained():
        rows = _cli_rows(node, f"SYSTEM CAS GC RUN '{DISK}'")
        return rows and rows[0]["pending_condemned"] == "0"

    _wait(drained, timeout=60, desc="GC retire pipeline drained")
    rows = _cli_rows(node, f"SYSTEM CAS FSCK '{DISK}'")
    assert rows, rows
    for column in ("dangling", "unreachable", "unaccounted", "stale_edge", "corrupted_runs", "chain_broken", "lifeless_keys"):
        assert rows[0][column] == "0", rows


def test_crash_between_early_commit_and_rename_leaves_no_part(start_cluster):
    node = cluster.instances["node1"]
    node.query("DROP TABLE IF EXISTS t_restart SYNC")
    node.query(
        "CREATE TABLE t_restart (a UInt64) ENGINE = MergeTree ORDER BY a "
        "SETTINGS storage_policy = 'cas_shared', min_bytes_for_wide_part = 0, "
        "temporary_directories_lifetime = 1, merge_tree_clear_old_temporary_directories_interval_seconds = 1"
    )
    node.query("SYSTEM ENABLE FAILPOINT part_storage_pause_after_commit_transaction")

    def insert():
        try:
            node.query("INSERT INTO t_restart SELECT number FROM numbers(1000)")
        except Exception:
            pass  # the server is killed under this query

    t = threading.Thread(target=insert, daemon=True)
    t.start()
    try:
        # The failpoint is global and pauses only the first part commit. Waiting on it instead of polling
        # cas_log: a log flush would itself be a part commit.
        node.query("SYSTEM WAIT FAILPOINT part_storage_pause_after_commit_transaction PAUSE", timeout=120)
        assert (
            node.query("SELECT count() FROM system.processes WHERE query LIKE 'INSERT INTO t_restart%'").strip() == "1"
        ), "the parked commit must belong to the INSERT"
        node.stop_clickhouse(kill=True)
    finally:
        # Releases the parked INSERT if the kill was not reached; a no-op once the server is gone.
        try:
            node.query("SYSTEM DISABLE FAILPOINT part_storage_pause_after_commit_transaction")
        except Exception:
            pass
        t.join(timeout=60)
    node.start_clickhouse()

    assert node.query("SELECT count() FROM t_restart").strip() == "0"
    _wait(
        lambda: node.query(
            "SYSTEM FLUSH LOGS cas_log; SELECT count() FROM system.cas_log "
            "WHERE event_type = 'ref_drop' AND ref_name LIKE 'tmp_insert_%'"
        ).strip()
        == "1",
        desc="temporary ref removed by clearOldTemporaryDirectories",
    )
    assert node.query("SELECT count() FROM system.parts WHERE table = 't_restart' AND name LIKE 'tmp_%'").strip() == "0"
    _assert_fsck_clean(node)
    node.query("DROP TABLE t_restart SYNC")


def test_storage_commit_does_not_hold_parts_lock(start_cluster):
    node = cluster.instances["node1"]
    node.query("DROP TABLE IF EXISTS t_lock SYNC")
    node.query(
        "CREATE TABLE t_lock (a UInt64) ENGINE = MergeTree ORDER BY a "
        "SETTINGS storage_policy = 'cas_shared', min_bytes_for_wide_part = 0"
    )
    node.query("SYSTEM ENABLE FAILPOINT part_storage_pause_before_commit_transaction")

    def insert_a():
        node.query("INSERT INTO t_lock SELECT number FROM numbers(1000)")

    t = threading.Thread(target=insert_a, daemon=True)
    t.start()
    try:
        # Pauses only the first part commit (A); B is not held by the failpoint.
        node.query("SYSTEM WAIT FAILPOINT part_storage_pause_before_commit_transaction PAUSE", timeout=120)
        assert (
            node.query("SELECT count() FROM system.processes WHERE query LIKE 'INSERT INTO t_lock SELECT number FROM numbers(1000)%'").strip()
            == "1"
        ), "the parked commit must belong to INSERT A"
        # B needs DataPartsLock to commit. It runs in its own thread: `max_execution_time` cannot
        # interrupt a mutex wait, so a regression must fail the join instead of hanging the runner.
        b_errors = []

        def insert_b():
            try:
                node.query("INSERT INTO t_lock SELECT number FROM numbers(10)")
            except Exception as e:
                b_errors.append(e)

        tb = threading.Thread(target=insert_b, daemon=True)
        tb.start()
        tb.join(timeout=60)
        if tb.is_alive():
            # The finally block releases A first, so B can finish and the thread does not leak.
            pytest.fail("insert B blocked while A was parked")
        assert not b_errors, b_errors
        assert node.query("SELECT count() FROM t_lock").strip() == "10"
        assert (
            node.query(
                "SELECT count() FROM system.processes WHERE query LIKE 'INSERT INTO t_lock SELECT number FROM numbers(1000)%'"
            ).strip()
            == "1"
        ), "A must still be parked"
    finally:
        node.query("SYSTEM DISABLE FAILPOINT part_storage_pause_before_commit_transaction")
        t.join()
    assert node.query("SELECT count() FROM t_lock").strip() == "1010"
    node.query("DROP TABLE t_lock SYNC")
