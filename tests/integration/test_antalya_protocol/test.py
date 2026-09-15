"""Antalya protocol version negotiation over the native TCP handshake.

Covers what a stateless test cannot express: a cluster in `remote_servers` against a separate host,
one authenticated with a `<secret>`, and a peer built before the marker existed. See
`src/Core/AntalyaProtocol.h`.

Only the side that opened the connection logs the negotiated version, because only the server
advertises and only the client reads. The log assertions count occurrences before and after the
query because the cluster fixture is module-scoped, so a plain substring check would pass on an
earlier test's output.
"""

import pytest

from helpers.cluster import CLICKHOUSE_CI_MIN_TESTED_VERSION, ClickHouseCluster

cluster = ClickHouseCluster(__file__)

MAIN_CONFIGS = ["configs/remote_servers.xml", "configs/validate_client_info.xml"]

node1 = cluster.add_instance("node1", main_configs=MAIN_CONFIGS)
node2 = cluster.add_instance("node2", main_configs=MAIN_CONFIGS)
# A build from before the marker existed: it sends an unmarked `ServerHello` and does not know to
# strip one. Nothing in the handshake may depend on the peer being Antalya.
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
    """The initiator reads the version off the marked `ServerHello` it gets back. The worker learns
    nothing about the initiator, so only the initiator's log carries the line."""
    initiator_before = count_in_log(node1, NEGOTIATED)
    worker_before = count_in_log(node2, NEGOTIATED)

    assert node1.query("SELECT count() FROM remote('node2', system.one)") == "1\n"

    assert count_in_log(node1, NEGOTIATED) > initiator_before
    assert count_in_log(node2, NEGOTIATED) == worker_before


def test_distributed_cluster_negotiates(started_cluster):
    negotiated_before = count_in_log(node1, NEGOTIATED)

    assert (
        node1.query(
            "SELECT count() FROM clusterAllReplicas('plain_cluster', system.one)"
            " SETTINGS prefer_localhost_replica = 0"
        )
        == "1\n"
    )

    assert count_in_log(node1, NEGOTIATED) > negotiated_before


def test_interserver_secret_negotiates(started_cluster):
    """The worker returns early from `receiveHello` for an interserver-secret connection, but
    `sendHello` still runs, so the initiator still gets a marked `ServerHello`."""
    secret_before = count_in_log(node2, "INTERSERVER SECRET")
    negotiated_before = count_in_log(node1, NEGOTIATED)

    assert (
        node1.query(
            "SELECT count() FROM clusterAllReplicas('secret_cluster', system.one)"
            " SETTINGS prefer_localhost_replica = 0"
        )
        == "1\n"
    )

    assert count_in_log(node2, "INTERSERVER SECRET") > secret_before
    assert count_in_log(node1, NEGOTIATED) > negotiated_before


def test_remote_table_function_sends_a_matching_client_info(started_cluster):
    """`remote(host, <table function>)` is the one path that sends `query_kind = INITIAL_QUERY`, so
    the Query packet carries its own `client_name`, which must equal the one the `Hello` sent or the
    worker rejects the query with `CLIENT_INFO_DOES_NOT_MATCH`."""
    assert (
        node1.query(
            "SELECT count() FROM remote('node2', numbers(10))"
            " SETTINGS log_comment = 'antalya_marker_remote_function'"
        )
        == "10\n"
    )

    node2.query("SYSTEM FLUSH LOGS query_log")
    assert (
        node2.query(
            "SELECT DISTINCT client_name FROM system.query_log"
            " WHERE log_comment = 'antalya_marker_remote_function' AND type = 'QueryFinish'"
            " AND is_initial_query"
        )
        == "ClickHouse server\n"
    )


def test_client_name_in_query_log_is_untouched(started_cluster):
    """Nothing marks `client_name` and the server never rewrites it, so what an end user's client
    sent is what `system.query_log` stores."""
    node1.query("SELECT 1 SETTINGS log_comment = 'antalya_marker_client'")
    node1.query("SYSTEM FLUSH LOGS query_log")

    assert (
        node1.query(
            "SELECT DISTINCT client_name FROM system.query_log"
            " WHERE log_comment = 'antalya_marker_client' AND type = 'QueryFinish'"
        )
        == "ClickHouse client\n"
    )


def test_new_initiator_against_an_unmarked_worker(started_cluster):
    """The rolling-upgrade direction. The worker predates the marker, so it neither strips nor
    expects one; an initiator that marked its `Hello` would leave an Antalya string in the worker's
    `client_name` and, with `validate_tcp_client_information`, fail the query outright."""
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
    """The other direction: an old client gets a marked `ServerHello` it does not know to strip. It
    only ever displays that string, so the query must be unaffected."""
    assert node_old.query("SELECT count() FROM remote('node1', numbers(10))") == "10\n"
