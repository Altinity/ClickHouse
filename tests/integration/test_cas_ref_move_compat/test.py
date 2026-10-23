import os

import pytest
from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)
OLD_TAG = "26.6.4.20001.altinityantalya"
CONFIG_DIR = os.path.join(os.path.dirname(os.path.realpath(__file__)), "configs")


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "old",
        main_configs=["configs/storage_conf.xml", "configs/server_root_id_old.xml"],
        image="altinity/clickhouse-server",
        tag=OLD_TAG,
        with_installed_binary=True,
        with_rustfs=True,
        stay_alive=True,
    )
    cluster.add_instance(
        "new",
        # No CAS disk at start: the pool must be created by the old build first (see the test).
        main_configs=[],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def _count(node, where):
    node.query("SYSTEM FLUSH LOGS cas_log")
    return int(node.query(f"SELECT count() FROM system.cas_log WHERE {where}").strip())


def test_old_pool_floor_and_old_reader(start_cluster):
    old, new = cluster.instances["old"], cluster.instances["new"]
    # Pool created by the old build (reader floor 1).
    old.query("CREATE TABLE t_old (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS storage_policy = 'cas_shared'")
    old.query("INSERT INTO t_old SELECT number FROM numbers(100)")

    # The new build mounts it: the first writable mount raises the floor to 2 before any rename, so the
    # new build's first rename is already an owner move.
    for name in ("storage_conf.xml", "server_root_id_new.xml"):
        new.copy_file_to_container(os.path.join(CONFIG_DIR, name), f"/etc/clickhouse-server/config.d/{name}")
    new.restart_clickhouse()
    new.query("CREATE TABLE t_new (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS storage_policy = 'cas_shared'")
    new.query("INSERT INTO t_new SELECT number FROM numbers(100)")
    assert _count(new, "event_type = 'ref_move'") == 1
    assert new.contains_in_log("min_reader_generation raised 1 -> 2")

    # The old node keeps serving its own table ...
    old.query("INSERT INTO t_old SELECT number FROM numbers(100)")
    assert old.query("SELECT count() FROM t_old").strip() == "200"
    # ... and refuses to mount the pool after a restart (reader floor 2 > its ceiling).
    old.stop_clickhouse()
    old.start_clickhouse(expected_to_fail=True)
    assert old.contains_in_log("pool requires reader generation 2 but this build supports at most 1")
