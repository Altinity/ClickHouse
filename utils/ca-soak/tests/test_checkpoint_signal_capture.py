"""The checkpoint-side capture of the two signal families, exercised through the real functions.

What is being defended here is that the capture cannot quietly produce nothing: a node that is down
yields a counted GAP with a log line saying so, and a broken query raises. Between those two, there is
no path that records zeros nobody asked for.
"""

import json
import urllib.error

import pytest

from soak import metrics as metrics_mod
from soak.cluster import QueryError
from soak.run import capture_checkpoint_signals, capture_phase_summary
from soak.signals import CAS_SIGNAL_EVENTS, PhaseCoverage, SignalTracker


# The `metrics` maps carry each phase's own detector keys, exactly as the server's `sumMap` does — the
# presence guard in `summarize_phases` reads them to tell "the value is zero" from "the name moved".
PHASE_ROWS = "\n".join([
    json.dumps({"phase": "fold_ref_list", "rounds": "2", "calls": "2", "total_us": "5000",
                "max_us": "3000", "logs_accounted": "0", "logs_applied": "0",
                "txns_unapplied": "0", "ref_folding_aborted": "0",
                "metrics": {"ref_keys_listed": "12", "ref_folding_aborted": "0"},
                "events": {"S3ListObjects": "6"}}),
    json.dumps({"phase": "fold_ref_intake", "rounds": "2", "calls": "2", "total_us": "800",
                "max_us": "500", "logs_accounted": "9", "logs_applied": "9",
                "txns_unapplied": "0", "ref_folding_aborted": "0",
                "metrics": {"logs_accounted": "9", "logs_applied": "9"}, "events": {}}),
])


class Node:
    def __init__(self, name, *, events=None, phases=None, raises=None):
        self.name = name
        self.events = events
        self.phases = phases
        self.raises = raises

    def query(self, sql, timeout=None, settings=None):
        if self.raises is not None:
            raise self.raises
        if "system.events" in sql:
            return "\n".join(f"{k}\t{v}" for k, v in self.events.items())
        return self.phases

    def command(self, sql, timeout=None, settings=None):
        self.query(sql, timeout=timeout)

    def __repr__(self):
        return f"Node({self.name})"


class Cluster:
    def __init__(self, *nodes):
        self._nodes = nodes

    def nodes(self):
        return self._nodes


def _all_zero():
    return {e: 0 for e in CAS_SIGNAL_EVENTS}


def test_signals_are_read_from_every_node_and_logged():
    a = Node("ch1", events=_all_zero())
    b = Node("ch2", events=dict(_all_zero(), CasRefAppendPreAttemptRefused=4))
    tracker = SignalTracker()
    lines = []
    capture_checkpoint_signals(Cluster(a, b), "GC checkpoint", tracker=tracker, log_fn=lines.append)
    assert tracker.reads == 2
    assert tracker.peak["CasRefAppendPreAttemptRefused"] == 4
    assert "CasRefAppendPreAttemptRefused=4" in lines[0]


def test_signals_do_not_gate_the_checkpoint():
    """Task 21 step 3: report, do not fail. Their benign rates are uncharacterised — in particular
    `CasRefAppendPreAttemptRefused` is EXPECTED to be nonzero under chaos."""
    hot = {e: 99 for e in CAS_SIGNAL_EVENTS}
    capture_checkpoint_signals(Cluster(Node("ch1", events=hot)), "cp",
                               tracker=SignalTracker(), log_fn=lambda _m: None)


def test_a_broken_signal_query_raises_rather_than_reporting_zeros():
    node = Node("ch1", raises=QueryError("n", 500, "Code: 47. Unknown expression identifier", "x"))
    with pytest.raises(QueryError):
        capture_checkpoint_signals(Cluster(node), "cp", tracker=SignalTracker(),
                                   log_fn=lambda _m: None)


def test_phase_summary_is_logged_and_persisted(tmp_path):
    conn = metrics_mod.open_db(str(tmp_path / "m.db"))
    node = Node("ch1", phases=PHASE_ROWS)
    coverage = PhaseCoverage()
    lines = []
    written = capture_phase_summary(Cluster(node), "GC checkpoint", since_ts=100,
                                    coverage=coverage, conn=conn, ts=555, log_fn=lines.append)
    assert written == 2
    assert coverage.captured == 1
    rows = {r["phase"]: r for r in metrics_mod.gc_phase_rows(conn)}
    assert rows["fold_ref_list"]["ref_folding_aborted"] == 0
    assert rows["fold_ref_list"]["ts"] == 555
    assert rows["fold_ref_list"]["checkpoint"] == "GC checkpoint"
    # The slowest phase and the detector values are both on the checkpoint's log line.
    assert "fold_ref_list=5.0ms" in lines[0]
    assert "fold_ref_list.ref_folding_aborted=0" in lines[0]


def test_a_node_that_is_down_is_a_counted_gap_not_an_empty_summary(tmp_path):
    conn = metrics_mod.open_db(str(tmp_path / "m.db"))
    node = Node("ch1", raises=urllib.error.URLError(ConnectionRefusedError()))
    coverage = PhaseCoverage()
    lines = []
    written = capture_phase_summary(Cluster(node), "cp", since_ts=0, coverage=coverage,
                                    conn=conn, ts=1, log_fn=lines.append)
    assert written == 0
    assert (coverage.gaps, coverage.captured) == (1, 0)
    assert "recorded as a gap, not as zero" in lines[0]
    assert metrics_mod.gc_phase_rows(conn) == []


def test_a_server_predating_the_phase_rows_raises(tmp_path):
    """`UNKNOWN_IDENTIFIER` on `phase`/`round_id`/`phase_metrics` means the binary cannot produce this
    signal at all. Reporting an empty summary for it is the 2026-07-25 vacuous-observation bug."""
    node = Node("ch1", raises=QueryError("n", 500, "Code: 47. Unknown expression identifier 'phase'", "x"))
    with pytest.raises(QueryError):
        capture_phase_summary(Cluster(node), "cp", since_ts=0, coverage=PhaseCoverage(),
                              conn=None, ts=1, log_fn=lambda _m: None)
