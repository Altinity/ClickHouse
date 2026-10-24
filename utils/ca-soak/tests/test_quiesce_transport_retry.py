"""Regression test for a live-soak finding (2026-07-19): a single transient transport error
(connection reset) during `quiesce()`'s backlog-drain POLL LOOP -- not the workload itself --
propagated straight out uncaught, aborting the whole multi-hour soak run with a bare
"TRANSPORT FAILURE" even though every workload op (INSERT/BARRIER) already gets a bounded
transport-retry. This can happen right after a chaos fault window closes: `wait_for_healthy`'s
`/ping` check can pass while the node is still settling its query-handling connections for a
moment, especially after a `both restart`/`both pause` fault where BOTH replicas are
re-establishing state at once.

`quiesce()`'s internal `backlog()`/`merge_activity()`/`errored_queue()` probes must now tolerate a
transient transport error via `_scalar_resilient` (bounded `retry_on_transport`), matching the
resilience the driver's own INSERT/BARRIER path already has."""

import time

from soak.checker import quiesce


class FakeNode:
    """Returns "0"-shaped answers for every backlog/merge/errored-queue probe query, and a fixed
    `toUnixTimestamp(now())`. The FIRST `.scalar()` call across the whole fake cluster raises a
    transient transport error exactly once; every call after that succeeds."""

    _shared_state = {"raised": False}

    def __init__(self, name):
        self.name = name
        self.scalar_calls = 0
        self.command_calls = []

    def __repr__(self):
        return f"FakeNode({self.name})"

    def command(self, sql, timeout=None, settings=None):
        self.command_calls.append(sql)

    def scalar(self, sql):
        self.scalar_calls += 1
        if not FakeNode._shared_state["raised"]:
            FakeNode._shared_state["raised"] = True
            raise ConnectionResetError("[Errno 104] Connection reset by peer")
        if "toUnixTimestamp" in sql:
            return "1700000000"
        if "max(elapsed)" in sql:
            return ""
        return "0"


class FakeCluster:
    def __init__(self, *nodes):
        self._nodes = list(nodes)

    def nodes(self):
        return self._nodes


def test_quiesce_tolerates_one_transient_transport_error_during_backlog_poll(monkeypatch):
    FakeNode._shared_state["raised"] = False   # reset the once-only injection point
    monkeypatch.setattr(time, "sleep", lambda s: None)   # no real delay for the retry backoff
    cluster = FakeCluster(FakeNode("ch1"), FakeNode("ch2"))
    logs = []

    now = quiesce(cluster, "ca_stress", timeout_s=10, admin_timeout_s=5, log_fn=logs.append)

    assert now == 1700000000
    # At least one probe call hit the injected transient and was retried, not fatal.
    assert any("transiently failed" in m for m in logs)


def test_quiesce_still_fails_fast_on_a_genuine_errored_replication_queue_entry(monkeypatch):
    """The transport-error tolerance must not mask a REAL errored replication-queue entry -- that
    must still fail the checkpoint immediately (existing `is_genuine_hang` "errored" behavior)."""
    monkeypatch.setattr(time, "sleep", lambda s: None)

    class ErroredNode(FakeNode):
        def scalar(self, sql):
            self.scalar_calls += 1
            if "last_exception" in sql:
                return "1"   # one genuinely errored queue entry
            if "toUnixTimestamp" in sql:
                return "1700000000"
            if "max(elapsed)" in sql:
                return ""
            return "5"   # non-zero backlog so drain() actually reaches the errored_queue() check

    FakeNode._shared_state["raised"] = True   # do not exercise the transient-injection path here
    cluster = FakeCluster(ErroredNode("ch1"), ErroredNode("ch2"))
    import pytest
    from soak.checker import CheckpointFailure
    with pytest.raises(CheckpointFailure, match="genuine error"):
        quiesce(cluster, "ca_stress", timeout_s=10, admin_timeout_s=5, log_fn=lambda m: None)
