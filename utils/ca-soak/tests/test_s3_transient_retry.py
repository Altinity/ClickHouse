"""Unit tests for the S3-transient tolerance (the task3 v3 chaos gap).

Pure tests, no docker, no real cluster. Reproducer: a 19s `rustfs pause` chaos fault made an
INSERT's CAS part-manifest conditional PUT time out after its deliberately-single ~3s attempt
(RFC cas-s3-timeout-retry-control) and the un-retried `Code: 499 ... Timeout ... (S3_ERROR)`
aborted the whole 2h run (tmp/task3_soak_2h_v3.log:518).

Covered here, following test_transport_retry.py's pattern for each retryable class:
  * classification: every TRANSPORT flavor of an S3_ERROR body (Timeout / Connection refused /
    Connection reset / Broken pipe / DNS error) IS `is_s3_transient`;
  * correctness signals are NEVER matched nor retried, even under code 499: S3 semantics
    (NoSuchKey, AccessDenied), raw PreconditionFailed, LOGICAL_ERROR, CORRUPTED_DATA -- and a
    correctness marker BEATS a transport flavor when both appear;
  * a transport flavor WITHOUT the S3_ERROR envelope (e.g. TIMEOUT_EXCEEDED) is not matched;
  * `retry_on_transport` retries-then-succeeds on the exact v3 shape, and budget exhaustion
    still fails loudly (bounded, not an infinite loop).
"""

import pytest

from soak.cluster import (
    QueryError,
    is_s3_transient,
    retry_on_transport,
    S3_ERROR_CODE,
)


def s3_timeout_error():
    # The EXACT shape from the task3 v3 failure (tmp/task3_soak_2h_v3.log:518): a CAS part-manifest
    # conditional PUT (single-attempt client) timing out while the rustfs container was paused.
    body = (
        "Code: 499. DB::Exception: Message: Poco::Exception. Code: 1000, e.code() = 0, Timeout "
        "(version 26.6.1.1), bucket test, key soak_pool/cas/manifests/ca_soak_ch2/store/110/"
        "11084089-34ba-415d-ab27-e73d4253d78e@cas@/0000000000000003-00000000000011cf/000001.proto, "
        "object size 4143. (S3_ERROR) (version 26.6.1.1)"
    )
    return QueryError("Node(localhost:8124)", 500, body, "INSERT INTO ca_stress VALUES")


def s3_flavored_error(flavor):
    body = (
        f"Code: 499. DB::Exception: Message: Poco::Exception. Code: 1000, e.code() = 0, {flavor} "
        "(version 26.6.1.1), bucket test, key soak_pool/cas/blobs/aa/bb.bin. (S3_ERROR)"
    )
    return QueryError("Node(localhost:8123)", 500, body, "INSERT INTO ca_stress VALUES")


def s3_nosuchkey_error():
    # S3 SEMANTIC absence (the B145-documented shape) -- a correctness signal (INV-NO-DANGLE on the
    # CAS read path), never weather.
    body = (
        "Code: 499. DB::Exception: Message: NoSuchKey: The specified key does not exist "
        "(version 26.6.1.1), bucket test, key soak_pool/cas/blobs/aa/bb.bin. (S3_ERROR)"
    )
    return QueryError("Node(localhost:8123)", 500, body, "SELECT count() FROM ca_stress")


def s3_access_denied_error():
    body = (
        "Code: 499. DB::Exception: Message: AccessDenied: Access Denied "
        "(version 26.6.1.1), bucket test, key soak_pool/cas/gc/state. (S3_ERROR)"
    )
    return QueryError("Node(localhost:8123)", 500, body, "INSERT INTO ca_stress VALUES")


def logical_error():
    body = "Code: 49. DB::Exception: something impossible happened. (LOGICAL_ERROR) (version 26.6.1.1)"
    return QueryError("Node(localhost:8123)", 500, body, "INSERT INTO ca_stress VALUES")


def timeout_exceeded_error():
    # A plain query-level TIMEOUT_EXCEEDED contains the word "Timeout" but is NOT an S3 transient
    # (no S3_ERROR envelope) -- it must not be swallowed by this classifier.
    body = ("Code: 159. DB::Exception: Timeout exceeded: elapsed 62.1 seconds, maximum 60 seconds. "
            "(TIMEOUT_EXCEEDED) (version 26.6.1.1)")
    return QueryError("Node(localhost:8123)", 500, body, "SELECT count() FROM ca_stress")


# --- classification ---------------------------------------------------------------------------

def test_s3_error_code_constant():
    assert S3_ERROR_CODE == 499


def test_v3_reproducer_timeout_is_s3_transient():
    e = s3_timeout_error()
    assert e.is_s3_transient is True
    assert is_s3_transient(e) is True


@pytest.mark.parametrize("flavor", ["Timeout", "Connection refused", "Connection reset by peer",
                                    "Broken pipe", "DNS error"])
def test_every_transport_flavor_is_s3_transient(flavor):
    e = s3_flavored_error(flavor)
    assert e.is_s3_transient is True
    assert is_s3_transient(e) is True


def test_s3_transient_does_not_leak_into_other_classifiers():
    # The S3 transient is its OWN class: not node-down, not aborted, not keeper, not readonly --
    # so it cannot accidentally enter retry paths reserved for those (e.g. `retry_on_aborted`).
    e = s3_timeout_error()
    assert e.is_node_down is False
    assert e.is_aborted is False
    assert e.is_keeper_transient is False
    assert e.is_readonly is False


def test_nosuchkey_is_not_s3_transient():
    # S3 4xx SEMANTICS (object absent) is a correctness signal: on the CAS read path a live ref
    # naming a missing object is durability loss (INV-NO-DANGLE); a retry would mask it.
    e = s3_nosuchkey_error()
    assert e.is_s3_transient is False
    assert is_s3_transient(e) is False


def test_access_denied_is_not_s3_transient():
    # Auth/config failure is deterministic; retrying can only mask it.
    e = s3_access_denied_error()
    assert e.is_s3_transient is False
    assert is_s3_transient(e) is False


def test_correctness_marker_beats_transport_flavor():
    # Even when a transport flavor word ALSO appears, a correctness marker keeps the error
    # fail-fast: classification is fail-closed toward surfacing real damage.
    body = (
        "Code: 499. DB::Exception: Message: NoSuchKey: The specified key does not exist; "
        "Timeout while re-reading (version 26.6.1.1), bucket test, key k. (S3_ERROR)"
    )
    e = QueryError("Node(x:1)", 500, body, "SELECT 1")
    assert e.is_s3_transient is False


def test_code_499_without_transport_flavor_is_not_s3_transient():
    # Defensive: the S3_ERROR envelope alone (some future semantic shape, e.g. SlowDown) must not
    # blanket-match -- classification requires an explicit transport flavor, same discipline as the
    # other classifiers in this module.
    body = "Code: 499. DB::Exception: Message: SlowDown: Please reduce your request rate. (S3_ERROR)"
    e = QueryError("Node(x:1)", 500, body, "INSERT INTO ca_stress VALUES")
    assert e.is_s3_transient is False


def test_timeout_without_s3_envelope_is_not_s3_transient():
    # "Timeout" alone (a query-level TIMEOUT_EXCEEDED) is not an S3 transient.
    e = timeout_exceeded_error()
    assert e.is_s3_transient is False
    assert is_s3_transient(e) is False


def test_logical_error_is_not_s3_transient():
    e = logical_error()
    assert e.is_s3_transient is False
    assert is_s3_transient(e) is False


def test_non_query_error_is_not_s3_transient():
    assert is_s3_transient(OSError("no route to host")) is False
    assert is_s3_transient(ValueError("logic bug")) is False


# --- retry_on_transport wiring ------------------------------------------------------------------

def test_s3_transient_retried_and_succeeds():
    """The exact v3 reproducer: the INSERT retry must tolerate the rustfs-pause S3 Timeout and
    succeed once the store is back (the fault lasted 19s; well within the ~287s budget)."""
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        if calls["n"] <= 2:
            raise s3_timeout_error()
        return "ok"

    out = retry_on_transport(attempt, attempts=10, sleep_fn=lambda s: None)
    assert out == "ok"
    assert calls["n"] == 3


def test_s3_transient_budget_exhaustion_still_fails():
    # An S3_ERROR that never clears (the store never came back) must still surface loudly after the
    # bounded budget -- this is not an unbounded loop.
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise s3_timeout_error()

    with pytest.raises(QueryError) as ei:
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert ei.value.is_s3_transient
    assert calls["n"] == 5   # exactly the bounded budget, no more


def test_nosuchkey_never_retried():
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise s3_nosuchkey_error()

    with pytest.raises(QueryError):
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert calls["n"] == 1   # raised on the first attempt, no retry


def test_access_denied_never_retried():
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise s3_access_denied_error()

    with pytest.raises(QueryError):
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert calls["n"] == 1


def test_logical_error_never_retried():
    calls = {"n": 0}

    def attempt():
        calls["n"] += 1
        raise logical_error()

    with pytest.raises(QueryError):
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)
    assert calls["n"] == 1
