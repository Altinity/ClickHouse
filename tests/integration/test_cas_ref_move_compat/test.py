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


# The pool lives under bucket `test`, prefix `shared_pool/` (the disk's endpoint).
POOL = "shared_pool"


def _count(node, where):
    node.query("SYSTEM FLUSH LOGS cas_log")
    return int(node.query(f"SELECT count() FROM system.cas_log WHERE {where}").strip())


def _content_keys():
    # Blobs and part manifests: the objects a GC round deletes.
    keys = set()
    for prefix in (POOL + "/blobs/", POOL + "/cas/manifests/"):
        for o in cluster.rustfs_client.list_objects(cluster.rustfs_bucket, prefix, recursive=True):
            keys.add(o.object_name)
    return keys


@pytest.fixture(scope="module")
def generation_three_pool(start_cluster):
    """The old build creates the pool and `t_old`; the new build then mounts it and creates `t_new`, whose
    `_ckpt` is generation 3. Returns nothing: the cases read the two nodes."""
    old, new = cluster.instances["old"], cluster.instances["new"]
    # Pool created by the old build (reader floor 1).
    old.query("CREATE TABLE t_old (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS storage_policy = 'cas_shared'")
    old.query("INSERT INTO t_old SELECT number FROM numbers(100)")

    # The new build mounts it: the first writable mount raises the floor to 3 before any write, so the
    # new build's first rename is already an owner move. The server is stopped while the disk config lands:
    # a running server's config reloader would mount the pool (and raise the floor) before the restart, and
    # the log line asserted below would belong to a process whose log the restart discards.
    new.stop_clickhouse()
    for name in ("storage_conf.xml", "server_root_id_new.xml"):
        new.copy_file_to_container(os.path.join(CONFIG_DIR, name), f"/etc/clickhouse-server/config.d/{name}")
    new.start_clickhouse()
    new.query("CREATE TABLE t_new (a UInt64) ENGINE = MergeTree ORDER BY a SETTINGS storage_policy = 'cas_shared'")
    new.query("INSERT INTO t_new SELECT number FROM numbers(100)")


def test_old_pool_floor_and_old_reader(generation_three_pool):
    old, new = cluster.instances["old"], cluster.instances["new"]
    assert _count(new, "event_type = 'ref_move'") == 1
    assert new.contains_in_log("min_reader_generation raised 1 -> 3")

    # The old node keeps serving its own table.
    old.query("INSERT INTO t_old SELECT number FROM numbers(100)")
    assert old.query("SELECT count() FROM t_old").strip() == "200"


def test_old_gc_leader_holds_the_namespace_then_the_old_server_refuses_to_mount(generation_three_pool):
    # The old node mounted first and its GC (interval 1 s) has held the pool's GC lease since, so it is the
    # leader. It cannot decode `t_new`'s generation 3 `_ckpt`: its rounds hold that namespace and suppress
    # every destructive step. The hold line names the namespace, which carries the table's uuid, and only the
    # leader's ref intake logs it, so a fresh line is a fresh round of the old leader.
    old, new = cluster.instances["old"], cluster.instances["new"]
    new.query("SYSTEM CAS GC STOP 'disk_cas_shared'")
    try:
        new.query("INSERT INTO t_new SELECT number FROM numbers(100, 100)")
        new.query("OPTIMIZE TABLE t_new FINAL")
        uuid = new.query("SELECT uuid FROM system.tables WHERE database = currentDatabase() AND name = 't_new'").strip()
        content_before = _content_keys()
        assert content_before, "the pool has no content objects to watch"
        # Only lines logged after this call count: `look_behind_lines=0` follows the log from its end.
        old.wait_for_log_line(
            f"namespace [^ ]*{uuid}[^ ]* has no usable checkpoint.*folds nothing and reclaims nothing",
            timeout=120,
            repetitions=2,
            look_behind_lines=0,
        )
        missing = content_before - _content_keys()
        assert not missing, f"the old GC leader deleted content objects while it held a namespace: {sorted(missing)[:10]}"
        assert new.query("SELECT count(), sum(a) FROM t_new").strip() == "200\t19900"
    finally:
        new.query("SYSTEM CAS GC START 'disk_cas_shared'")

    # The old node refuses to mount the pool after a restart: the reader floor 3 is above its ceiling.
    old.stop_clickhouse()
    old.start_clickhouse(expected_to_fail=True)
    assert old.contains_in_log("pool requires reader generation 3 but this build supports at most 1")
