"""Unit tests for the client-side ABORTED-retry wrapper (completes the B137 fix).

Pure tests: a fake Node whose `.command` raises a retryable ABORTED QueryError some number of times
then succeeds. No docker, no real cluster.
"""

import pytest

from soak.cluster import QueryError, retry_on_aborted, ABORTED_CODE


class FakeNode:
    def __init__(self, fail_with, fail_times):
        self.fail_with = fail_with          # a QueryError (or callable producing one) to raise
        self.fail_times = fail_times        # how many leading calls raise before success
        self.calls = 0

    def command(self, sql):
        self.calls += 1
        if self.calls <= self.fail_times:
            raise self.fail_with
        return None


def aborted_error():
    body = "Code: 236. DB::Exception: ... ABORTED, retry the operation. (ABORTED)"
    return QueryError("Node(x:1)", 500, body, "INSERT INTO t VALUES")


def other_error():
    body = "Code: 60. DB::Exception: Table t does not exist. (UNKNOWN_TABLE)"
    return QueryError("Node(x:1)", 500, body, "INSERT INTO t VALUES")


def test_is_aborted_detects_code_236():
    assert aborted_error().is_aborted is True
    assert other_error().is_aborted is False


def test_retry_succeeds_after_two_aborted():
    node = FakeNode(aborted_error(), fail_times=2)
    retries = []
    retry_on_aborted(
        lambda: node.command("INSERT INTO t VALUES"),
        backoff_s=0,
        on_retry=lambda attempt, err: retries.append(attempt),
    )
    assert node.calls == 3            # 2 failures + 1 success
    assert retries == [1, 2]          # on_retry fired before each of the 2 retries


def test_retry_returns_immediately_on_first_success():
    node = FakeNode(aborted_error(), fail_times=0)
    retry_on_aborted(lambda: node.command("INSERT INTO t VALUES"), backoff_s=0)
    assert node.calls == 1


def test_retry_exhaustion_raises_aborted():
    node = FakeNode(aborted_error(), fail_times=99)
    with pytest.raises(QueryError) as ei:
        retry_on_aborted(lambda: node.command("INSERT INTO t VALUES"), attempts=6, backoff_s=0)
    assert ei.value.is_aborted
    assert node.calls == 6            # bounded: exactly `attempts` tries


def test_non_aborted_error_is_not_retried():
    node = FakeNode(other_error(), fail_times=99)
    with pytest.raises(QueryError) as ei:
        retry_on_aborted(lambda: node.command("INSERT INTO t VALUES"), attempts=6, backoff_s=0)
    assert ei.value.code == 500 and not ei.value.is_aborted
    assert node.calls == 1            # raised on the first attempt, no retry


def test_aborted_code_constant():
    assert ABORTED_CODE == 236
