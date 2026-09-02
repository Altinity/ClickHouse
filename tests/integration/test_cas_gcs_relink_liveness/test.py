"""Fetch-by-relink liveness on a slow control plane.

Two replicas of one ReplicatedMergeTree table share one CAS pool over the fake GCS service of
`test_cas_gcs`, with every `_ckpt` write delayed so each ref-lane flush is slow the way it is on real
GCS (about one mutation per second per object). Both replicas insert continuously, so each is a sender
and a receiver at once and each keeps its own lane busy. A confirm rule that refuses whenever the
sender's lane is busy starves both replication queues here (finding F11 of the 2026-09-02 live GCS
campaign); the ref-scoped rule lets them drain. The unit tests pin the rule; this is the liveness
reproduction they cannot give.
"""
import os
import threading
import time

import pytest

from helpers.cluster import ClickHouseCluster
from helpers.mock_servers import start_mock_servers

GCS_HOST = "fakegcs"
GCS_PORT = 8080
CONFIG_IN_CONTAINER = "/etc/clickhouse-server/config.d/cas_gcs_shared.xml"
MOCK_DIR = os.path.join(os.path.dirname(__file__), "..", "test_cas_gcs", "gcs_mocks")
NODES = ("node1", "node2")

# Every `_ckpt` PUT sleeps this long: a flush's committed-frontier publication, and so the tenure, lasts
# at least this long. Chosen well above the fake's own latency and well below the replication queue's
# retry backoff, so the lanes stay busy without the test taking minutes.
CKPT_DELAY_MS = 1000
INSERTS_PER_NODE = 80
ROWS_PER_INSERT = 1000
DRAIN_TIMEOUT_S = 180

cluster = ClickHouseCluster(__file__)


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    # As in test_cas_gcs: a CAS disk mounts at server start and fails closed when its store is
    # unreachable, and the fake can only be launched once its container is up. So both nodes start
    # without the disk, the disk configuration is installed with the node's own server root, and each
    # node is restarted once the fake answers.
    for name in NODES:
        cluster.add_instance(name, macros={"replica": name}, with_zookeeper=True, stay_alive=True)
    cluster.add_instance(
        GCS_HOST, hostname=GCS_HOST, image="altinityinfra/python-bottle", tag="latest", stay_alive=True
    )
    try:
        cluster.start()
        start_mock_servers(cluster, MOCK_DIR, [("server.py", GCS_HOST, str(GCS_PORT))])
        for name in NODES:
            node = cluster.instances[name]
            node.copy_file_to_container(
                os.path.join(os.path.dirname(__file__), "configs", "storage_conf.xml"),
                CONFIG_IN_CONTAINER,
            )
            node.replace_in_config(CONFIG_IN_CONTAINER, "__SERVER_ROOT_ID__", name)
            node.restart_clickhouse()
        yield cluster
    finally:
        cluster.shutdown()


def _control_post(path):
    container = cluster.get_container_id(GCS_HOST)
    return cluster.exec_in_container(
        container, ["curl", "-sS", "-X", "POST", "http://localhost:{}{}".format(GCS_PORT, path)]
    )


def _queue_size(node, table):
    return int(
        node.query("SELECT count() FROM system.replication_queue WHERE table = '{}'".format(table))
    )


def _refusal_counters(node):
    return node.query(
        "SELECT event, value FROM system.events WHERE event LIKE 'CASRelinkConfirmRefused%' "
        "ORDER BY event FORMAT TSV"
    )


def test_both_queues_drain_under_slow_checkpoints():
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "relink_liveness"
    for node in (node1, node2):
        node.query("DROP TABLE IF EXISTS {} SYNC".format(table))
        node.query(
            "CREATE TABLE {t} (id Int64, v UInt64) "
            "ENGINE = ReplicatedMergeTree('/clickhouse/tables/{t}', '{{replica}}') "
            "ORDER BY id SETTINGS storage_policy = 'cas_gcs_shared'".format(t=table)
        )

    _control_post("/_control/delay?substr=_ckpt&ms={}".format(CKPT_DELAY_MS))
    try:
        errors = []

        def insert_loop(node, base):
            try:
                for i in range(INSERTS_PER_NODE):
                    node.query(
                        "INSERT INTO {} SELECT number, number * 10 FROM numbers({}, {})".format(
                            table, base + i * ROWS_PER_INSERT, ROWS_PER_INSERT
                        )
                    )
            except Exception as e:  # surfaced below, on the test thread
                errors.append((node.name, repr(e)))

        threads = [
            threading.Thread(target=insert_loop, args=(node1, 0)),
            threading.Thread(target=insert_loop, args=(node2, 10_000_000)),
        ]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        assert errors == [], errors

        # Liveness: each replica fetches the other's parts by relink while its own lane stays busy
        # with its own inserts and its own fetch bookkeeping. Both queues must still drain.
        deadline = time.time() + DRAIN_TIMEOUT_S
        while time.time() < deadline and (_queue_size(node1, table) or _queue_size(node2, table)):
            time.sleep(1)
        sizes = (_queue_size(node1, table), _queue_size(node2, table))
        for node in (node1, node2):
            print(node.name, "refusal counters:", _refusal_counters(node))
        assert sizes == (0, 0), (
            "replication queues did not drain in {} s with slow checkpoints: node1={} node2={}".format(
                DRAIN_TIMEOUT_S, *sizes
            )
        )
    finally:
        _control_post("/_control/delay?substr=&ms=0")

    expected = 2 * INSERTS_PER_NODE * ROWS_PER_INSERT
    assert int(node1.query("SELECT count() FROM {}".format(table))) == expected
    assert int(node2.query("SELECT count() FROM {}".format(table))) == expected
    for node in (node1, node2):
        node.query("DROP TABLE IF EXISTS {} SYNC".format(table))
