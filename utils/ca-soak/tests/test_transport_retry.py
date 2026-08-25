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
    is_keeper_transient,
    retry_on_transport,
    ABORTED_CODE,
    NODE_DOWN_CODES,
    KEEPER_EXCEPTION_CODE,
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


def keeper_session_expired_error():
    # The B190 shape: `Exception::fromMessage` (no "Coordination error:" prefix).
    body = "Code: 999. Coordination::Exception: Session expired. (KEEPER_EXCEPTION) (version 26.6.1.1)"
    return QueryError("Node(localhost:8123)", 500, body, "OPTIMIZE TABLE ca_stress")


def keeper_operation_timeout_error():
    # The real reproducer from the task3v2 2h chaos soak (task3v2-chaos-diag-report.md): a mutation
    # entry create under .../mutations hit a Keeper RPC that made no progress for 30s while ch1's
    # session was dying/re-establishing. `Exception::fromPath` always prefixes path-bearing Keeper
    # errors with "Coordination error: {message}, path {path}".
    body = (
        "Code: 999. Coordination::Exception: Coordination error: Operation timeout, "
        "path /clickhouse/tables/ca_stress/mutations. (KEEPER_EXCEPTION) (version 26.6.1.1)"
    )
    return QueryError("Node(localhost:8123)", 500, body, "ALTER TABLE ca_stress DELETE WHERE bucket = 7")


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
    # CONTRACT CHANGE (20m-i3, two-tier ABORTED): an ABORTED reaching this loop means the inner
    # `retry_on_aborted` budget already exhausted, and the outer envelope now absorbs it (see
    # `is_aborted`). The representative non-retryable error is a LOGIC QueryError.
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise unknown_table_error()   # a logic QueryError -> must not be retried here

    with pytest.raises(QueryError):
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
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


# --- Keeper-coordination transients on the MUTATION path (task3v2-chaos-diag-report.md) --------
#
# A chaos-window `ALTER TABLE ca_stress DELETE ...` hit `Code: 999. Coordination::Exception:
# Coordination error: Operation timeout, path /clickhouse/tables/ca_stress/mutations.
# (KEEPER_EXCEPTION)` during a ~70s self-healing Keeper-session/mount-lease outage and aborted the
# whole 2h run: `retry_on_transport` (which gates `apply_barrier`, the mutation path) treated no
# KEEPER_EXCEPTION variant as retryable, even though `apply_barrier`'s own docstring claims
# transport-resilience. Fixed by broadening the (renamed) `is_keeper_transient` classifier to match
# BOTH the `Session expired` and `Operation timeout` KEEPER_EXCEPTION shapes and wiring it into
# `retry_on_transport` alongside `is_node_down`/`is_readonly`/`is_mount_fenced`.

def test_keeper_session_expired_is_keeper_transient():
    e = keeper_session_expired_error()
    assert e.is_keeper_transient is True
    assert e.is_keeper_session_expired is True   # back-compat alias still matches
    assert is_keeper_transient(e) is True


def test_keeper_operation_timeout_is_keeper_transient():
    # This is the exact variant from the diag report that the OLD narrower `Session expired`-only
    # classifier would NOT have matched even if it had been wired into retry_on_transport.
    e = keeper_operation_timeout_error()
    assert e.is_keeper_transient is True
    assert is_keeper_transient(e) is True


def test_keeper_exception_code_constant():
    assert KEEPER_EXCEPTION_CODE == 999


def test_non_keeper_error_is_not_keeper_transient():
    # A genuine, unrelated HTTP 500 (not a KEEPER_EXCEPTION at all) must NOT be classified as a
    # Keeper transient -- fail-fast for real errors must be preserved.
    e = unknown_table_error()
    assert e.is_keeper_transient is False
    assert is_keeper_transient(e) is False


def test_keeper_code_without_recognized_text_is_not_keeper_transient():
    # Defensive: code 999 alone (no recognized message shape) must not blanket-match -- classification
    # is by BODY TEXT, not bare code, same discipline as the other classifiers in this module.
    body = "Code: 999. DB::Exception: some unrelated future KEEPER_EXCEPTION shape. (KEEPER_EXCEPTION)"
    e = QueryError("Node(x:1)", 500, body, "ALTER TABLE t DELETE WHERE 1")
    assert e.is_keeper_transient is False
    assert is_keeper_transient(e) is False


def test_keeper_operation_timeout_retried_and_succeeds():
    """The exact reproducer: `apply_barrier`'s mutation retry must tolerate `Operation timeout` and
    succeed once the Keeper session self-heals (the real outage lasted ~66-90s; well within budget)."""
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        if calls["n"] <= 2:
            raise keeper_operation_timeout_error()
        return "ok"

    out = retry_on_transport(attempt, attempts=10, sleep_fn=lambda s: None)
    assert out == "ok"
    assert calls["n"] == 3


def test_keeper_session_expired_retried_and_succeeds():
    """Regression check: the `Session expired` shape (already tolerated on the best-effort OPTIMIZE
    path since B190) must now ALSO be tolerated on the mutation/transport-retry path."""
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        if calls["n"] <= 1:
            raise keeper_session_expired_error()
        return "ok"

    out = retry_on_transport(attempt, attempts=10, sleep_fn=lambda s: None)
    assert out == "ok"
    assert calls["n"] == 2


def test_keeper_transient_budget_exhaustion_still_fails():
    # A KEEPER_EXCEPTION that never clears (budget exhausted) must still surface loudly, same as any
    # other transport-retryable class -- this is not an unbounded loop.
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise keeper_operation_timeout_error()

    with pytest.raises(QueryError) as ei:
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert ei.value.is_keeper_transient
    assert calls["n"] == 5   # exactly the bounded budget, no more


def test_non_keeper_http_500_not_retried():
    # A genuine (non-transport, non-Keeper) HTTP 500 -- a real logic error -- must fail FAST with no
    # retry at all, preserving fail-fast behavior for genuine errors.
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise unknown_table_error()

    with pytest.raises(QueryError) as ei:
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert not ei.value.is_keeper_transient
    assert calls["n"] == 1   # raised on the first attempt, no retry


def stage_uncertain_aborted_error():
    # The 20m-i3 reproducer (tmp/soak_20m_i3.log:183): the stagefix (c3d9aa9d8d6) controller
    # exhausts its manifest-PUT budget during a compound chaos window and surfaces ABORTED —
    # persisting far past `retry_on_aborted`'s sub-second inner budget.
    body = ("Code: 236. DB::Exception: stageManifest: part-manifest PUT at 'soak_pool/cas/.../000001.proto' "
            "is UNCERTAIN (retry budget exhausted) — nothing conclusive was named; the caller re-stages "
            "with a fresh ManifestId. (ABORTED) (version 26.6.1.1)")
    return QueryError("Node(localhost:8123)", 500, body, "INSERT INTO ca_stress VALUES")


def test_persistent_aborted_is_absorbed_by_transport_envelope():
    # Two-tier design: after retry_on_aborted exhausts and re-raises, the OUTER transport loop must
    # absorb the still-transient ABORTED (compound fault window) and succeed once the store heals.
    from soak.cluster import is_aborted

    err = stage_uncertain_aborted_error()
    assert is_aborted(err)

    calls = {"n": 0}

    def fn():
        calls["n"] += 1
        if calls["n"] < 4:
            raise err
        return "ok"

    slept = []
    assert retry_on_transport(fn, attempts=10, sleep_fn=slept.append) == "ok"
    assert calls["n"] == 4
    assert len(slept) == 3


def test_persistent_aborted_still_fails_after_transport_budget():
    err = stage_uncertain_aborted_error()

    def fn():
        raise err

    with pytest.raises(QueryError):
        retry_on_transport(fn, attempts=5, sleep_fn=lambda _s: None)


def test_non_aborted_logic_error_still_fails_fast():
    err = unknown_table_error()
    calls = {"n": 0}

    def fn():
        calls["n"] += 1
        raise err

    with pytest.raises(QueryError):
        retry_on_transport(fn, attempts=10, sleep_fn=lambda _s: None)
    assert calls["n"] == 1
