"""Unit tests for `sync_replica_with_readonly_retry` (B155 fix).

A ReplicatedMergeTree replica transiently becomes TABLE_IS_READ_ONLY (code 242) while
re-establishing its ZooKeeper session after a chaos fault. The `sync_replica_with_readonly_retry`
helper must retry `SYSTEM SYNC REPLICA` on that transient with bounded backoff, and only escalate
to a `CheckpointFailure` when the readonly window PERSISTS past the budget. Any non-readonly
`QueryError` must propagate immediately (no retry). Pure tests: a fake `Node` whose `command`
raises a configurable sequence of errors, with injected sleep/monotonic so no real time passes."""

import pytest

from soak.cluster import QueryError, TABLE_IS_READ_ONLY_CODE
from soak.checker import CheckpointFailure, sync_replica_with_readonly_retry


# --- helpers -----------------------------------------------------------------------------------

def readonly_error(node="Node(x:1)"):
    """A real TABLE_IS_READ_ONLY response body shape as returned by ClickHouse HTTP interface."""
    body = (
        "Code: 242. DB::Exception: Table ca_stress is in readonly mode due to "
        "initialization failure: Lost ZooKeeper session and can't recover it. "
        "(TABLE_IS_READ_ONLY) (version 26.6.1.1)"
    )
    return QueryError(node, 500, body, "SYSTEM SYNC REPLICA ca_stress")


def code_readonly_only_error(node="Node(x:1)"):
    """Body with only the numeric code (no text tag) — also must match."""
    body = f"Code: {TABLE_IS_READ_ONLY_CODE}. DB::Exception: Table is in readonly mode."
    return QueryError(node, 500, body, "SYSTEM SYNC REPLICA ca_stress")


def unknown_table_error(node="Node(x:1)"):
    body = "Code: 60. DB::Exception: Table ca_stress does not exist. (UNKNOWN_TABLE)"
    return QueryError(node, 500, body, "SYSTEM SYNC REPLICA ca_stress")


class FakeClock:
    def __init__(self):
        self.t = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.t

    def sleep(self, s):
        self.sleeps.append(s)
        self.t += s


class FakeNode:
    """Raises `errors` in sequence (each call pops one error or returns None on success)."""

    def __init__(self, errors):
        """errors: list of Exception | None. None means success. A list element may also be
        a callable() -> Exception | None for dynamic sequences."""
        self._errors = list(errors)
        self.calls = 0
        self.sqls = []

    def command(self, sql, timeout=None, settings=None):
        self.calls += 1
        self.sqls.append(sql)
        if not self._errors:
            return None
        err = self._errors.pop(0)
        if callable(err) and not isinstance(err, Exception):
            err = err()
        if err is not None:
            raise err


# --- classification ----------------------------------------------------------------------------

def test_is_readonly_detects_text_tag():
    assert readonly_error().is_readonly is True


def test_is_readonly_detects_numeric_code():
    assert code_readonly_only_error().is_readonly is True


def test_is_readonly_false_for_other_errors():
    assert unknown_table_error().is_readonly is False


def test_table_is_read_only_code_constant():
    assert TABLE_IS_READ_ONLY_CODE == 242


# --- retry: success paths ---------------------------------------------------------------------

def test_succeeds_immediately_no_retry():
    clock = FakeClock()
    node = FakeNode([])   # no errors -> succeeds immediately
    logs = []
    sync_replica_with_readonly_retry(
        node, "ca_stress",
        readonly_budget_s=120.0,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=logs.append,
    )
    assert node.calls == 1
    assert clock.sleeps == []     # no sleep needed
    assert not logs               # no retry log on first-attempt success


def test_retries_on_readonly_then_recovers():
    """Two readonly transients followed by success: must retry twice and then proceed."""
    clock = FakeClock()
    node = FakeNode([readonly_error(), readonly_error(), None])
    logs = []
    sync_replica_with_readonly_retry(
        node, "ca_stress",
        readonly_budget_s=120.0,
        backoff_start_s=1.0, backoff_cap_s=5.0,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=logs.append,
    )
    assert node.calls == 3        # 2 failures + 1 success
    assert len(clock.sleeps) == 2
    # Log on recovery (attempt > 0 success): must mention "recovered"
    assert any("recovered" in m for m in logs)
    # Log on each retry: must mention "transiently readonly"
    retry_logs = [m for m in logs if "transiently readonly" in m]
    assert len(retry_logs) == 2


def test_logs_retry_budget_info():
    """The retry log must include budget information so operators can see how much time remains."""
    clock = FakeClock()
    node = FakeNode([readonly_error(), None])
    logs = []
    sync_replica_with_readonly_retry(
        node, "ca_stress",
        readonly_budget_s=120.0,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=logs.append,
    )
    retry_log = next(m for m in logs if "transiently readonly" in m)
    assert "120" in retry_log     # budget mentioned
    assert "budget" in retry_log


# --- retry: failure/escalation paths ---------------------------------------------------------

def test_persistent_readonly_raises_checkpoint_failure():
    """If the replica stays read-only past the budget, it must escalate to CheckpointFailure
    (not WORKLOAD FAILURE — the soak reports it as a real stuck-replica finding, not a plain
    query error, so the failure kind is distinct from a workload op failure)."""
    # The clock starts at 0; each sleep advances it. We pick a budget of 10s and a backoff of
    # 3s so after a few retries the deadline is crossed.
    clock = FakeClock()
    # Endless readonly errors (budget exhausted → CheckpointFailure on the next attempt check)
    node = FakeNode([readonly_error() for _ in range(50)])
    with pytest.raises(CheckpointFailure) as ei:
        sync_replica_with_readonly_retry(
            node, "ca_stress",
            readonly_budget_s=10.0,
            backoff_start_s=3.0, backoff_cap_s=5.0,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
        )
    msg = str(ei.value)
    assert "TABLE_IS_READ_ONLY" in msg or "stuck" in msg
    assert "budget exhausted" in msg


def test_non_readonly_error_propagates_immediately():
    """A non-readonly QueryError must be re-raised immediately — no retry."""
    clock = FakeClock()
    node = FakeNode([unknown_table_error()])
    with pytest.raises(QueryError) as ei:
        sync_replica_with_readonly_retry(
            node, "ca_stress",
            readonly_budget_s=120.0,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
        )
    assert not ei.value.is_readonly
    assert node.calls == 1        # no retry — fails immediately
    assert clock.sleeps == []     # no sleep


def test_non_readonly_error_after_one_readonly_propagates():
    """After a readonly retry, a genuine non-readonly error on the next attempt still propagates."""
    clock = FakeClock()
    node = FakeNode([readonly_error(), unknown_table_error()])
    with pytest.raises(QueryError) as ei:
        sync_replica_with_readonly_retry(
            node, "ca_stress",
            readonly_budget_s=120.0,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
        )
    assert not ei.value.is_readonly
    assert node.calls == 2        # one retry happened, then the genuine error surfaced


# --- backoff shape ----------------------------------------------------------------------------

def test_backoff_is_capped_exponential():
    """Backoff grows exponentially but is capped at `backoff_cap_s`."""
    clock = FakeClock()
    # Enough readonly errors so we see several backoff increments; budget is generous.
    node = FakeNode([readonly_error() for _ in range(6)] + [None])
    sync_replica_with_readonly_retry(
        node, "ca_stress",
        readonly_budget_s=300.0,
        backoff_start_s=1.0, backoff_cap_s=5.0,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
    )
    # Backoffs: 1, 2, 4, 5 (cap), 5 (cap), 5 (cap)
    assert clock.sleeps[0] == 1.0
    assert clock.sleeps[1] == 2.0
    assert clock.sleeps[2] == 4.0
    assert all(s <= 5.0 for s in clock.sleeps)


def test_transient_transport_error_retries_then_recovers():
    """A raw transport-level error (e.g. connection reset right after a chaos fault window closes,
    even though `wait_for_healthy`'s /ping already passed) must be retried with the same bounded
    budget as a TABLE_IS_READ_ONLY transient -- NOT propagate immediately."""
    clock = FakeClock()
    node = FakeNode([ConnectionResetError("[Errno 104] Connection reset by peer"), None])
    logs = []
    sync_replica_with_readonly_retry(
        node, "ca_stress",
        readonly_budget_s=120.0,
        sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=logs.append,
    )
    assert node.calls == 2        # 1 failure + 1 success
    assert len(clock.sleeps) == 1
    assert any("recovered" in m for m in logs)
    retry_logs = [m for m in logs if "transient transport error" in m]
    assert len(retry_logs) == 1


def test_persistent_transport_error_raises_checkpoint_failure():
    """If the transport error PERSISTS past the budget, it must escalate to CheckpointFailure, same
    as a stuck TABLE_IS_READ_ONLY -- the replica never actually recovered."""
    clock = FakeClock()
    node = FakeNode([ConnectionResetError("reset") for _ in range(50)])
    with pytest.raises(CheckpointFailure) as ei:
        sync_replica_with_readonly_retry(
            node, "ca_stress",
            readonly_budget_s=10.0,
            backoff_start_s=3.0, backoff_cap_s=5.0,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
        )
    msg = str(ei.value)
    assert "stuck" in msg
    assert "budget exhausted" in msg


def test_non_transport_non_readonly_error_still_propagates_immediately():
    """A logic error that is neither a readonly QueryError nor a transport error must still fail
    immediately -- the new transport tolerance must not become a blanket catch-all."""
    clock = FakeClock()
    node = FakeNode([ValueError("not a transport or readonly error")])
    with pytest.raises(ValueError):
        sync_replica_with_readonly_retry(
            node, "ca_stress",
            readonly_budget_s=120.0,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
        )
    assert node.calls == 1
    assert clock.sleeps == []


def test_backoff_does_not_exceed_remaining_budget():
    """The sleep is clipped to the remaining budget so we don't overshoot the deadline."""
    clock = FakeClock()
    # Budget = 3s, backoff starts at 2s — after first retry backoff=2 but only ~1s left.
    node = FakeNode([readonly_error() for _ in range(10)])
    with pytest.raises(CheckpointFailure):
        sync_replica_with_readonly_retry(
            node, "ca_stress",
            readonly_budget_s=3.0,
            backoff_start_s=2.0, backoff_cap_s=10.0,
            sleep_fn=clock.sleep, monotonic_fn=clock.monotonic, log_fn=lambda m: None,
        )
    # Total elapsed time must not massively overshoot the budget (may be slightly above
    # because the last sleep before the deadline check is clipped to remaining).
    assert clock.t <= 3.0 + 2.0   # generous bound: budget + one backoff_start_s gap
