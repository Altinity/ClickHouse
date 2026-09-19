"""Antalya protocol version negotiation over the native TCP handshake.

The log assertions count occurrences before and after the query because the cluster fixture is
module-scoped, so a plain substring check would pass on an earlier test's output.
"""

import pytest

from helpers.cluster import CLICKHOUSE_CI_MIN_TESTED_VERSION, ClickHouseCluster

cluster = ClickHouseCluster(__file__)

node1 = cluster.add_instance("node1")
node2 = cluster.add_instance("node2")
# A build from before the marker existed: it sends an unmarked `ServerHello` and does not know to
# strip one.
node_old = cluster.add_instance(
    "node_old",
    image="altinity/clickhouse-server",
    tag=CLICKHOUSE_CI_MIN_TESTED_VERSION,
    with_installed_binary=True,
)

NEGOTIATED = "Antalya protocol: "


@pytest.fixture(scope="module")
def started_cluster():
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def count_in_log(node, substring):
    return int(node.count_in_log(substring))


def test_remote_function_negotiates(started_cluster):
    """Only the initiator logs the version: the worker learns nothing about the peer."""
    initiator_before = count_in_log(node1, NEGOTIATED)
    worker_before = count_in_log(node2, NEGOTIATED)

    assert node1.query("SELECT count() FROM remote('node2', system.one)") == "1\n"

    assert count_in_log(node1, NEGOTIATED) > initiator_before
    assert count_in_log(node2, NEGOTIATED) == worker_before


def test_new_initiator_against_an_unmarked_worker(started_cluster):
    """A worker that predates the marker neither strips nor expects one."""
    assert (
        node1.query(
            "SELECT count() FROM remote('node_old', numbers(10))"
            " SETTINGS log_comment = 'antalya_marker_old_worker'"
        )
        == "10\n"
    )

    node_old.query("SYSTEM FLUSH LOGS")
    assert (
        node_old.query(
            "SELECT DISTINCT client_name FROM system.query_log"
            " WHERE log_comment = 'antalya_marker_old_worker' AND type = 'QueryFinish'"
            " AND is_initial_query"
        )
        == "ClickHouse server\n"
    )


def test_unmarked_initiator_against_a_marked_server(started_cluster):
    """An old client gets a marked `ServerHello` it does not know to strip, and only displays it."""
    assert node_old.query("SELECT count() FROM remote('node1', numbers(10))") == "10\n"
