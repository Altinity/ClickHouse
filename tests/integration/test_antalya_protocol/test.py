import pytest

from helpers.cluster import CLICKHOUSE_CI_MIN_TESTED_VERSION, ClickHouseCluster

cluster = ClickHouseCluster(__file__)

node1 = cluster.add_instance("node1")
node2 = cluster.add_instance("node2")
# An unmarked build predating Antalya protocol negotiation.
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
    initiator_before = count_in_log(node1, NEGOTIATED)
    worker_before = count_in_log(node2, NEGOTIATED)

    assert node1.query("SELECT count() FROM remote('node2', system.one)") == "1\n"

    assert count_in_log(node1, NEGOTIATED) > initiator_before
    assert count_in_log(node2, NEGOTIATED) == worker_before


def test_new_initiator_against_an_unmarked_worker(started_cluster):
    before = count_in_log(node1, NEGOTIATED)
    assert node1.query("SELECT count() FROM remote('node_old', numbers(10))") == "10\n"
    assert count_in_log(node1, NEGOTIATED) == before


def test_unmarked_initiator_against_a_marked_server(started_cluster):
    assert node_old.query("SELECT count() FROM remote('node1', numbers(10))") == "10\n"
