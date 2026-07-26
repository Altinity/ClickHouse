"""A timed-out fsck must return a LOWER BOUND, never nothing, and never a fabricated clean result.

Measured motivation, not hypothetical: in the 4-hour Part B soak, 4 of 39 checkpoints lost their entire
post-GC fsck gate to a 180 s timeout — `dangling`, `stale_edge` and the dryrun-subset assert all skipped —
plus 4 more entry-gate skips. The gate drops out exactly when the pool is large, which is when it matters.

The mechanism was an INVERSION of two budgets. `run_fsck` bounded the SUBPROCESS at 180 s while `ca-fsck`'s
own scan deadline defaulted to 600 s, so the process was always killed before its internal deadline could
fire — which is the only path that prints the accumulated `partial=1` counts. `--partial` existed in the
product the whole time and could never be reached from here.

These tests pin the corrected relationship: the scan's own deadline must sit strictly INSIDE the subprocess
budget, and a partial result must never be read as proof of anything.
"""
import subprocess

import pytest

from soak.fsck import run_fsck, stale_edge_verdict, PARTIAL_MARGIN_S


class _FakeCompleted:
    def __init__(self, stdout="", returncode=0, stderr=""):
        self.stdout = stdout
        self.returncode = returncode
        self.stderr = stderr


def _capture_cmd(monkeypatch, stdout=""):
    """Run `run_fsck` against a stubbed subprocess and return the argv it built."""
    seen = {}

    def fake_run(cmd, **kwargs):
        seen["cmd"] = cmd
        seen["timeout"] = kwargs.get("timeout")
        return _FakeCompleted(stdout=stdout)

    monkeypatch.setattr(subprocess, "run", fake_run)
    return seen


# ---------------------------------------------------------------------------
# The budget inversion
# ---------------------------------------------------------------------------

def test_partial_puts_the_scan_deadline_inside_the_subprocess_budget(monkeypatch):
    """`--timeout` must be strictly less than the subprocess timeout, or `--partial` is unreachable."""
    seen = _capture_cmd(monkeypatch)
    run_fsck("c", detail=False, timeout_s=180, partial=True)

    query = seen["cmd"][seen["cmd"].index("--query") + 1]
    assert "--partial" in query, query
    scan_budget = int(query.split("--timeout")[1].split()[0])
    assert scan_budget < seen["timeout"], (
        f"scan deadline {scan_budget}s must fire BEFORE the subprocess is killed at {seen['timeout']}s, "
        "otherwise the accumulated counts are lost — this is the exact inversion that cost 4 checkpoints"
    )
    assert scan_budget == 180 - PARTIAL_MARGIN_S


def test_without_partial_no_timeout_flag_is_injected(monkeypatch):
    """Non-partial callers keep the old behaviour: the product's own default deadline applies."""
    seen = _capture_cmd(monkeypatch)
    run_fsck("c", detail=False, timeout_s=180, partial=False)
    query = seen["cmd"][seen["cmd"].index("--query") + 1]
    assert "--partial" not in query
    assert "--timeout" not in query


def test_partial_margin_cannot_produce_a_nonpositive_budget(monkeypatch):
    """A caller passing a budget smaller than the margin must still get a positive scan deadline."""
    seen = _capture_cmd(monkeypatch)
    run_fsck("c", detail=False, timeout_s=5, partial=True)
    query = seen["cmd"][seen["cmd"].index("--query") + 1]
    assert int(query.split("--timeout")[1].split()[0]) >= 1


# ---------------------------------------------------------------------------
# A partial result proves nothing
# ---------------------------------------------------------------------------

def test_partial_result_is_parsed_with_its_flag_and_reason(monkeypatch):
    line = ("reachable=10 dangling=0 unreachable=0 pending_gc=0 awaiting_gc=0 unaccounted=0 "
            "stale_edge=0 corrupted_runs=0 snapshot_oracle_mismatches=0 snapshot_oracle_checked=0 "
            "physical_bytes=1 referenced_logical_bytes=1 distinct_blobs=1 total_blob_refs=1 "
            "dedup_ratio=1.0 partial=1 reason='fsck: exceeded the deadline during listing'")
    _capture_cmd(monkeypatch, stdout=line + "\n")
    got = run_fsck("c", detail=False, timeout_s=180, partial=True)
    assert got["partial"] == 1
    assert got["reason"].startswith("fsck: exceeded the deadline")
    assert got["reachable"] == 10


def test_stale_edge_on_a_partial_scan_is_unchecked_not_clean():
    """`unreachable == 0` proves `stale_edge == 0` only over the part of the pool that was WALKED.

    Without this, a timed-out scan that happened to visit a clean prefix reports the whole pool clean —
    the precise "a check that passed while looking at nothing" shape this project has hit three times.
    """
    partial_clean = {"unreachable": 0, "stale_edge": 0, "partial": 1,
                     "reason": "fsck: exceeded the deadline during listing"}
    verdict, why = stale_edge_verdict(partial_clean, detail=False)
    assert verdict == "unchecked", why
    assert "partial" in why.lower()


def test_stale_edge_on_a_partial_detail_scan_is_also_unchecked():
    """A `--detail` cross-check that did not finish covers only what it reached."""
    verdict, why = stale_edge_verdict(
        {"unreachable": 0, "stale_edge": 0, "partial": 1, "reason": "deadline"}, detail=True)
    assert verdict == "unchecked", why


def test_a_positive_finding_on_a_partial_scan_still_counts():
    """Partial weakens proofs of ABSENCE, never evidence of PRESENCE: a found stale edge is found."""
    verdict, why = stale_edge_verdict(
        {"unreachable": 5, "stale_edge": 3, "partial": 1, "reason": "deadline"}, detail=True)
    assert verdict == "found", why


def test_complete_scan_still_proves_clean():
    """The non-partial path must be unaffected."""
    verdict, _ = stale_edge_verdict({"unreachable": 0, "stale_edge": 0}, detail=False)
    assert verdict == "clean"


# ---------------------------------------------------------------------------
# The gate that `partial=True` could have broken
# ---------------------------------------------------------------------------

def test_pool_consistent_gate_does_not_count_a_partial_read_as_clean():
    """A partial scan's `dangling == 0` is a lower bound over the walked prefix, not a coherent cut.

    This is a regression guard on the FIX ITSELF. Turning on `--partial` makes a timed-out scan return
    `dangling=0, exit_code=0` instead of raising — which `wait_for_pool_consistent` would otherwise count
    as a clean read and return on. That would have converted an honest timeout into a fabricated
    consistency proof: the very failure mode the partial work exists to remove.
    """
    from soak.run import wait_for_pool_consistent, CheckpointFailure

    partial_clean = {"dangling": 0, "exit_code": 0, "reachable": 5, "unreachable": 0,
                     "partial": 1, "reason": "deadline"}
    clock = {"t": 0.0}

    def monotonic():
        return clock["t"]

    def sleep(_s):
        clock["t"] += 10.0

    with pytest.raises(CheckpointFailure, match="never reached a self-consistent state"):
        wait_for_pool_consistent(lambda: dict(partial_clean), timeout_s=30, stable=2,
                                 sleep_fn=sleep, monotonic_fn=monotonic)


def test_pool_consistent_gate_still_returns_on_complete_clean_reads():
    """The non-partial path is unchanged: two complete clean reads satisfy the gate."""
    from soak.run import wait_for_pool_consistent

    clean = {"dangling": 0, "exit_code": 0, "reachable": 5, "unreachable": 0}
    clock = {"t": 0.0}
    got = wait_for_pool_consistent(lambda: dict(clean), timeout_s=30, stable=2,
                                   sleep_fn=lambda _s: clock.__setitem__("t", clock["t"] + 1),
                                   monotonic_fn=lambda: clock["t"])
    assert got["reachable"] == 5
