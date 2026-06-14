"""Unit tests for the Phase-2 transport-failure handling (chaos robustness).

Pure tests, no docker, no real cluster:
  * `is_transport_error` classifies node-down failures (connection refused/reset/timeout) as transport,
    and keeps a server-side `QueryError` (incl. the B137 ABORTED) OUT of the transport class so it is
    handled by the distinct ABORTED-retry path.
  * `retry_on_transport` retries transport failures with bounded attempts and reroutes (the caller
    alternates replicas via the attempt index), and propagates a non-transport error immediately.
"""

import socket
import urllib.error

import pytest

from soak.cluster import (
    QueryError,
    is_transport_error,
    is_node_down,
    retry_on_transport,
    ABORTED_CODE,
    NODE_DOWN_CODES,
)


def aborted_query_error():
    body = "Code: 236. DB::Exception: ... ABORTED, retry the operation. (ABORTED)"
    return QueryError("Node(x:1)", 500, body, "INSERT INTO t VALUES")


def unknown_table_error():
    body = "Code: 60. DB::Exception: Table t does not exist. (UNKNOWN_TABLE)"
    return QueryError("Node(x:1)", 500, body, "INSERT INTO t VALUES")


def query_cancelled_error():
    # The shape observed in the Phase-2 run when a `docker restart` gracefully shut node2 down mid
    # in-flight INSERT (real reproducer; see backlog B142).
    body = ("Code: 394. DB::Exception: Query was cancelled. (QUERY_WAS_CANCELLED) "
            "(version 26.6.1.1)")
    return QueryError("Node(localhost:8124)", 500, body, "INSERT INTO ca_stress VALUES")


def network_error():
    body = "Code: 210. DB::Exception: I/O error: Broken pipe. (NETWORK_ERROR)"
    return QueryError("Node(x:1)", 500, body, "INSERT INTO t VALUES")


# --- classification ---------------------------------------------------------------------------

def test_connection_refused_is_transport():
    # urllib wraps a refused TCP connect into URLError(reason=ConnectionRefusedError).
    err = urllib.error.URLError(ConnectionRefusedError(111, "Connection refused"))
    assert is_transport_error(err) is True


def test_connection_reset_is_transport():
    err = urllib.error.URLError(ConnectionResetError(104, "Connection reset by peer"))
    assert is_transport_error(err) is True


def test_socket_timeout_is_transport():
    assert is_transport_error(socket.timeout("timed out")) is True
    assert is_transport_error(urllib.error.URLError(socket.timeout("timed out"))) is True


def test_bare_oserror_is_transport():
    assert is_transport_error(OSError("no route to host")) is True


def test_urlerror_without_oserror_reason_is_transport():
    # A URLError with a non-exception reason still means no HTTP response was produced.
    assert is_transport_error(urllib.error.URLError("unknown")) is True


def test_http_error_is_not_transport():
    # HTTPError means the server RESPONDED (it is a URLError subclass) -> not a transport failure.
    http = urllib.error.HTTPError("http://x/", 500, "err", {}, None)
    assert is_transport_error(http) is False


def test_query_error_is_not_transport():
    # A server-side ClickHouse exception (incl. the retryable ABORTED) is NOT a transport failure;
    # it is handled by the distinct ABORTED-retry path.
    assert is_transport_error(aborted_query_error()) is False
    assert is_transport_error(unknown_table_error()) is False
    assert aborted_query_error().is_aborted is True
    assert ABORTED_CODE == 236


def test_value_error_is_not_transport():
    assert is_transport_error(ValueError("logic bug")) is False


# --- node-down classification (the QueryError twin of a dropped connection, backlog B142) ------

def test_query_cancelled_is_node_down_but_not_transport():
    e = query_cancelled_error()
    # A server-side cancellation is NOT a connection-level transport error...
    assert is_transport_error(e) is False
    # ...but IS a node-down failure (graceful shutdown cancelled the in-flight query) -> retried.
    assert e.is_node_down is True
    assert is_node_down(e) is True


def test_network_error_is_node_down():
    e = network_error()
    assert e.is_node_down is True
    assert is_node_down(e) is True


def test_aborted_is_not_node_down():
    # The B137 ABORTED has its OWN retry path; it must not be swallowed by the node-down retry.
    e = aborted_query_error()
    assert e.is_node_down is False
    assert is_node_down(e) is False


def test_logic_error_is_not_node_down():
    e = unknown_table_error()
    assert e.is_node_down is False
    assert is_node_down(e) is False


def test_connection_drop_is_node_down():
    # The connection-level twin is also node-down.
    assert is_node_down(urllib.error.URLError(ConnectionResetError(104, "reset"))) is True


def test_node_down_codes_include_observed_codes():
    assert 394 in NODE_DOWN_CODES and 210 in NODE_DOWN_CODES and 209 in NODE_DOWN_CODES


# --- retry_on_transport -----------------------------------------------------------------------

class FlakyTransport:
    """Fails the first `down_times` calls with a transport error, then succeeds. Records the
    per-attempt index it was called with so we can assert rerouting (alternating replicas)."""

    def __init__(self, down_times):
        self.down_times = down_times
        self.attempts = []

    def __call__(self, attempt_idx):
        self.attempts.append(attempt_idx)
        if len(self.attempts) <= self.down_times:
            raise urllib.error.URLError(ConnectionRefusedError(111, "Connection refused"))
        return "ok"


def test_retry_recovers_after_node_comes_back():
    counter = {"i": 0}
    flaky = FlakyTransport(down_times=3)

    def attempt():
        i = counter["i"]
        counter["i"] += 1
        return flaky(i)

    out = retry_on_transport(attempt, attempts=10, sleep_fn=lambda s: None)
    assert out == "ok"
    # 3 failures + 1 success; attempt indices increase so the caller alternates replicas.
    assert flaky.attempts == [0, 1, 2, 3]


def test_retry_reroutes_across_replicas():
    # The attempt index alternates 0,1,0,1,... so a two-replica list reroutes each retry.
    seen = []

    def attempt(idx_box=[0]):
        i = idx_box[0]
        idx_box[0] += 1
        seen.append(i % 2)   # which replica this attempt picks
        raise urllib.error.URLError(ConnectionResetError(104, "reset"))

    with pytest.raises(urllib.error.URLError):
        retry_on_transport(attempt, attempts=4, sleep_fn=lambda s: None)
    assert seen == [0, 1, 0, 1]   # rerouted between the two replicas each retry


def test_retry_exhaustion_raises_transport():
    def attempt():
        raise urllib.error.URLError(ConnectionRefusedError(111, "Connection refused"))

    with pytest.raises(urllib.error.URLError):
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)


def test_non_transport_error_propagates_immediately():
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise aborted_query_error()   # a QueryError is NOT transport -> must not be retried here

    with pytest.raises(QueryError) as ei:
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert ei.value.is_aborted
    assert calls["n"] == 1            # raised on the first attempt, no transport retry


def test_query_cancelled_query_error_is_retried_and_recovers():
    # A QUERY_WAS_CANCELLED during a node restart must be retried/rerouted (the B142 fix), then the
    # op succeeds on the recovered/other replica.
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        if calls["n"] <= 2:
            raise query_cancelled_error()
        return "ok"

    out = retry_on_transport(attempt, attempts=10, sleep_fn=lambda s: None)
    assert out == "ok"
    assert calls["n"] == 3


def test_backoff_is_bounded_and_capped():
    sleeps = []

    def attempt():
        raise OSError("down")

    with pytest.raises(OSError):
        retry_on_transport(attempt, attempts=8, backoff_s=0.5, max_backoff_s=8.0,
                           sleep_fn=sleeps.append)
    # 7 sleeps before the 8th (final) attempt; capped-exponential, none exceeds the cap.
    assert len(sleeps) == 7
    assert all(s <= 8.0 for s in sleeps)
    assert sleeps[0] == 0.5 and sleeps[-1] == 8.0
