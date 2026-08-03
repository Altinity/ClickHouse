"""Unit tests for the CAS signal reader (`soak.signals`).

The theme these tests defend is one clause of `docs/superpowers/cas/INTENT.md`: a signal that can
degrade to "nothing observed" rather than to an error is a defect. So the assertions are mostly about
what must RAISE — an unknown counter, a server-side rejection — versus the two things that may
legitimately return a sentinel: a node that is down, and a system log that was never materialized.
"""

import json
import urllib.error

import pytest

from soak.cluster import QueryError
from soak.signals import (
    CAS_SIGNAL_EVENTS,
    DETECTOR_METRICS,
    LATE_PUT_EVIDENCE_EVENTS,
    LATE_PUT_VIOLATION_EVENTS,
    LatePutFencing,
    UNCHARACTERISED_SIGNALS,
    check_late_put_fencing,
    PhaseCoverage,
    SignalTracker,
    SignalsUnsupported,
    format_phase_summary,
    is_benign_probe_gap,
    parse_events_tsv,
    parse_phase_summary,
    parse_signal_events,
    phase_summary_sql,
    read_phase_summary,
    read_signal_events,
    signal_events_sql,
    summarize_phases,
)


class FakeNode:
    """A node whose `query` returns canned text, or raises a canned exception."""

    def __init__(self, answer=None, raises=None):
        self.answer = answer
        self.raises = raises
        self.queries = []

    def query(self, sql, timeout=None, settings=None):
        self.queries.append(sql)
        if self.raises is not None:
            raise self.raises
        if callable(self.answer):
            return self.answer(sql)
        return self.answer

    def command(self, sql, timeout=None, settings=None):
        self.query(sql, timeout=timeout)

    def __repr__(self):
        return "Node(fake)"


def _qerr(body):
    return QueryError("node", 500, body, "SELECT 1")


def _events_tsv(values):
    return "\n".join(f"{k}\t{v}" for k, v in values.items())


# ---------------------------------------------------------------------------
# system.events
# ---------------------------------------------------------------------------

def test_sql_asks_for_zero_values():
    """Without `system_events_show_zero_values`, `system.events` omits every counter that has never
    been incremented — which makes "the binary has no such counter" and "the counter is zero" the same
    answer. The whole fail-closed design rests on this setting being present."""
    sql = signal_events_sql()
    assert "system_events_show_zero_values = 1" in sql
    for e in CAS_SIGNAL_EVENTS:
        assert f"'{e}'" in sql


def test_sql_rejects_a_name_that_is_not_an_identifier():
    with pytest.raises(ValueError):
        signal_events_sql(("CasGood", "Cas'; DROP"))


def test_parse_events_tsv_raises_on_a_malformed_row():
    """A row we cannot parse came back from a query we wrote ourselves — surface it, do not drop it."""
    with pytest.raises(ValueError):
        parse_events_tsv("CASRefAppendWedged")
    with pytest.raises(ValueError):
        parse_events_tsv("CASRefAppendWedged\tnot-a-number")


def test_all_zero_is_a_valid_reading():
    text = _events_tsv({e: 0 for e in CAS_SIGNAL_EVENTS})
    got = parse_signal_events(text)
    assert got == {e: 0 for e in CAS_SIGNAL_EVENTS}


def test_missing_counter_raises_rather_than_reading_as_zero():
    """The headline case: a renamed or not-yet-existing counter must kill the run, not read as a
    permanent quiet zero for four hours."""
    values = {e: 0 for e in CAS_SIGNAL_EVENTS}
    dropped = values.pop("CASGcUnappliedFoldedTxns")
    assert dropped == 0
    with pytest.raises(SignalsUnsupported) as ei:
        parse_signal_events(_events_tsv(values))
    assert "CASGcUnappliedFoldedTxns" in str(ei.value)


def test_extra_counters_in_the_answer_are_ignored():
    values = {e: 1 for e in CAS_SIGNAL_EVENTS}
    values["SomethingElse"] = 99
    got = parse_signal_events(_events_tsv(values))
    assert "SomethingElse" not in got
    assert set(got) == set(CAS_SIGNAL_EVENTS)


def test_read_returns_none_when_the_node_is_down():
    """A node killed/paused by chaos is legitimately unreadable — a visible gap, never a faked zero."""
    node = FakeNode(raises=urllib.error.URLError(ConnectionRefusedError()))
    assert read_signal_events(node) is None


def test_read_returns_none_on_a_graceful_shutdown_cancellation():
    """The body-bearing twin of a dropped socket: a node going down cancels the in-flight query."""
    node = FakeNode(raises=_qerr("Code: 394. DB::Exception: Query was cancelled"))
    assert read_signal_events(node) is None


def test_read_raises_on_a_server_side_rejection():
    """UNKNOWN_IDENTIFIER and friends mean the QUERY is broken. That is a harness bug and must surface
    — this is the exact degradation that made every GC verdict in the scenario suite pass vacuously."""
    node = FakeNode(raises=_qerr("Code: 47. DB::Exception: Unknown expression identifier 'evnt'"))
    with pytest.raises(QueryError):
        read_signal_events(node)


def test_read_raises_on_unknown_table_for_system_events():
    """`system.events` is built in and always exists; an UNKNOWN_TABLE there is an anomaly, not the
    lazily-materialized-system-log case, so it must NOT be folded into a gap."""
    node = FakeNode(raises=_qerr("Code: 60. DB::Exception: Table system.events does not exist"))
    with pytest.raises(QueryError):
        read_signal_events(node)


def test_benign_gap_classification():
    down = urllib.error.URLError(ConnectionResetError())
    missing = _qerr("Code: 60. DB::Exception: Table ... does not exist. (UNKNOWN_TABLE)")
    broken = _qerr("Code: 47. DB::Exception: Unknown expression identifier")
    assert is_benign_probe_gap(down)
    assert not is_benign_probe_gap(missing)
    assert is_benign_probe_gap(missing, allow_missing_table=True)
    assert not is_benign_probe_gap(broken)
    assert not is_benign_probe_gap(broken, allow_missing_table=True)


# ---------------------------------------------------------------------------
# The per-phase GC log
# ---------------------------------------------------------------------------

def _phase_row(phase, **kw):
    row = {
        "phase": phase,
        "rounds": "3",
        "calls": "3",
        "total_us": "1000",
        "max_us": "600",
        "logs_accounted": "0",
        "logs_applied": "0",
        "txns_unapplied": "0",
        "ref_folding_aborted": "0",
        "metrics": {},
        "events": {},
    }
    row.update(kw)
    if "metrics" not in kw:
        # A phase that RAN emits its own detector metrics unconditionally, so a realistic summed map
        # carries them. `summarize_phases` reads exactly this to tell an absent key (renamed metric)
        # from a zero value, so a fixture with an empty map would be an unrealistic row that trips the
        # guard rather than the behaviour under test.
        row["metrics"] = {m: row[m] for p, m in DETECTOR_METRICS if p == phase}
    return json.dumps(row)


def test_phase_summary_sql_scopes_to_phase_rows_and_the_window():
    sql = phase_summary_sql(1770000000)
    assert "event_type = 'Phase'" in sql
    assert "toDateTime(1770000000)" in sql
    # round_id, not `round`: `round` is 0 on Start and nonexistent on a non-leader round, i.e. absent
    # from exactly the rounds worth correlating.
    assert "uniqExact(round_id)" in sql
    for detector in ("logs_accounted", "logs_applied", "txns_unapplied"):
        assert f"phase_metrics['{detector}']" in sql


def test_parse_phase_summary_accepts_json_quoted_64bit_integers():
    """ClickHouse quotes UInt64 in JSON by default (`output_format_json_quote_64bit_integers`)."""
    text = _phase_row("fold_reduce", total_us="123456", txns_unapplied="2",
                      metrics={"shards_reduced": "8"}, events={"S3ListObjects": "40"})
    rows = parse_phase_summary(text)
    assert rows[0]["total_us"] == 123456
    assert rows[0]["txns_unapplied"] == 2
    assert rows[0]["metrics"] == {"shards_reduced": 8}
    assert rows[0]["events"] == {"S3ListObjects": 40}


def test_summarize_ranks_the_slowest_phases():
    text = "\n".join([
        _phase_row("fold_ref_group", total_us="9000", max_us="5000"),
        _phase_row("fold_reduce", total_us="4000", max_us="2000"),
        _phase_row("lease", total_us="10", max_us="5"),
    ])
    s = summarize_phases(parse_phase_summary(text), top_n=2)
    assert [x["phase"] for x in s["slowest"]] == ["fold_ref_group", "fold_reduce"]
    assert s["phases"] == 3
    assert s["rounds"] == 3
    assert s["total_us"] == 13010


def test_summarize_surfaces_the_detector_values():
    text = "\n".join([
        _phase_row("fold_ref_group", ref_folding_aborted="1"),
        _phase_row("fold_ref_intake", logs_accounted="10", logs_applied="7"),
        _phase_row("fold_reduce", txns_unapplied="3"),
    ])
    s = summarize_phases(parse_phase_summary(text))
    assert s["detector"]["fold_ref_group.ref_folding_aborted"] == 1
    assert s["detector"]["fold_reduce.txns_unapplied"] == 3
    # The identity the intake pair exists to check: every position the sealed cut covers reached the
    # single cursor-advance site.
    assert s["intake_mismatch"] == 3
    assert "fold_ref_group.ref_folding_aborted=1" in format_phase_summary(s)


def test_a_renamed_phase_metric_fails_closed_instead_of_reading_zero():
    """`sum(phase_metrics['x'])` is a DEFINED zero for an absent key, so a metric renamed in the server
    would report 0 forever and `intake_mismatch` would silently become `-logs_applied` on every healthy
    round. That already happened once (`logs_intended` -> `logs_accounted`) and nothing caught it. A
    phase that RAN emits its own detector metrics unconditionally, so an absent key in the summed map
    means the NAME moved — fail closed, exactly as an unknown `system.events` counter does."""
    text = _phase_row("fold_ref_intake", logs_accounted="0", logs_applied="7",
                      metrics={"logs_applied": "7", "some_future_name": "7"})
    with pytest.raises(SignalsUnsupported) as e:
        summarize_phases(parse_phase_summary(text))
    assert "logs_accounted" in str(e.value)
    assert "some_future_name" in str(e.value), "the message must name the keys that ARE there"


def test_a_phase_that_never_ran_is_absent_not_zero():
    """A round that never led emits no `fold_*` phase at all. Reporting "0 holes" for a window in which
    the probe never ran would be a claim the data does not support."""
    s = summarize_phases(parse_phase_summary(_phase_row("lease")))
    assert s["detector"] == {}
    assert s["intake_mismatch"] is None


def test_read_phase_summary_tolerates_an_unmaterialized_log():
    """`SystemLog` creates its table on first flush, so a replica that has never held the GC lease
    genuinely has no rows. That is "nothing has happened yet", the same class as an empty result."""
    node = FakeNode(raises=_qerr("Code: 60. DB::Exception: Table ... (UNKNOWN_TABLE)"))
    assert read_phase_summary(node, 0) is None


def test_read_phase_summary_raises_when_the_server_predates_the_phase_rows():
    """A binary without `phase`/`round_id`/`phase_metrics` answers UNKNOWN_IDENTIFIER. Returning an
    empty summary for that is the vacuous-observation bug; it must surface."""
    node = FakeNode(raises=_qerr("Code: 47. DB::Exception: Unknown expression identifier 'phase'"))
    with pytest.raises(QueryError):
        read_phase_summary(node, 0)


def test_read_phase_summary_flushes_first():
    """The GC log is buffered; a checkpoint that reads without flushing systematically misses the
    rounds closest to the moment of interest."""
    node = FakeNode(answer=lambda sql: "" if "FLUSH" not in sql else "")
    read_phase_summary(node, 0)
    assert any("SYSTEM FLUSH LOGS" in q for q in node.queries)


# ---------------------------------------------------------------------------
# Run-level accounting
# ---------------------------------------------------------------------------

def test_tracker_distinguishes_a_gap_from_a_zero():
    t = SignalTracker(("A", "B"))
    t.observe("n1", None)
    assert t.reads == 0 and t.gaps == 1
    t.observe("n1", {"A": 0, "B": 3})
    assert t.reads == 1
    assert t.peak == {"A": 0, "B": 3}
    assert t.nonzero_reads == {"A": 0, "B": 1}


def test_tracker_report_warns_when_nothing_was_ever_read():
    """A green run in which a counter was never read is blind, not green — the report has to say so."""
    t = SignalTracker(("A",))
    t.observe("n1", None)
    text = "\n".join(t.report_lines())
    assert "BLIND" in text
    t2 = SignalTracker(("A",))
    t2.observe("n1", {"A": 1})
    assert "BLIND" not in "\n".join(t2.report_lines())


def test_tracker_report_labels_the_counters_as_not_gated():
    t = SignalTracker()
    t.observe("n1", {e: 0 for e in CAS_SIGNAL_EVENTS})
    text = "\n".join(t.report_lines())
    for e in CAS_SIGNAL_EVENTS:
        assert e in text
    assert "reported-not-gated" in text


def test_phase_coverage_counts_gaps_and_empty_windows_apart():
    c = PhaseCoverage()
    c.observe(None)
    c.observe(summarize_phases([]))
    c.observe(summarize_phases(parse_phase_summary(
        _phase_row("fold_reduce", max_us="900", txns_unapplied="1"))))
    assert (c.attempts, c.gaps, c.empty, c.captured) == (3, 1, 1, 1)
    assert c.worst_phase_us["fold_reduce"] == 900
    assert c.detector_peak["fold_reduce.txns_unapplied"] == 1


def test_phase_coverage_report_warns_when_never_captured():
    c = PhaseCoverage()
    c.observe(None)
    assert "NEVER captured" in "\n".join(c.report_lines())


# ---------------------------------------------------------------------------
# The late-PUT-loses invariant
# ---------------------------------------------------------------------------

def test_evidence_and_violation_families_are_disjoint_and_all_watched():
    """The split is the whole design: the evidence counters are the protocol WORKING and must never
    fail a run, the violation counters are it failing. A name in both would be read twice and gated on
    its own evidence."""
    assert not set(LATE_PUT_EVIDENCE_EVENTS) & set(LATE_PUT_VIOLATION_EVENTS)
    for e in LATE_PUT_EVIDENCE_EVENTS + LATE_PUT_VIOLATION_EVENTS:
        assert e in CAS_SIGNAL_EVENTS      # so preflight proves the binary has it
    # ... and none of them is excused as uncharacterised: the evidence half is never a failure, the
    # violation half needs no rate because its benign rate is zero by construction.
    assert not set(LATE_PUT_EVIDENCE_EVENTS + LATE_PUT_VIOLATION_EVENTS) & set(UNCHARACTERISED_SIGNALS)


def test_a_probe_gap_is_not_a_passing_invariant():
    """A node that could not be read has said nothing. Folding that to "no violations" is the exact
    silent-degradation defect this module exists to avoid, so the gap must not be a clean reading."""
    f = LatePutFencing()
    assert f.observe("n1", None) == []
    assert f.reads == 0 and f.gaps == 1
    assert not f.exercised


def test_the_late_put_losing_is_evidence_not_a_violation():
    """`CASRefAppendSealRejected` is a deposed writer being told so — the mechanism working. A run that
    fenced a hundred stragglers is a run that tested the invariant, not one that broke it."""
    f = LatePutFencing()
    assert f.observe("n1", {"CASRefRecoveryEpochSealed": 4, "CASRefAppendSealRejected": 100,
                            "CASRefRecoveryStragglerAdopted": 2, "CASRefNeedsRecovery": 0,
                            "CASGcUnappliedFoldedTxns": 0, "CASRefRecoveryStreamHole": 0}) == []
    assert f.exercised
    assert f.evidence_peak["CASRefAppendSealRejected"] == 100
    text = "\n".join(f.report_lines())
    assert "VIOLATION" not in text and "WARNING" not in text


def test_each_violation_counter_is_reported_by_name():
    f = LatePutFencing()
    for event in LATE_PUT_VIOLATION_EVENTS:
        found = f.observe("n1", {event: 1})
        assert len(found) == 1 and event in found[0]
    assert len(f.violations) == len(LATE_PUT_VIOLATION_EVENTS)
    assert "VIOLATION" in "\n".join(f.report_lines())


def test_an_inherited_counter_is_not_charged_to_this_run():
    """Soak stands reuse containers. The counters are cumulative per process, so a value carried in
    from an earlier run would otherwise red this one before it did anything."""
    baseline = {"n1": {"CASRefNeedsRecovery": 7}}
    assert check_late_put_fencing({"CASRefNeedsRecovery": 7}, baseline=baseline["n1"]) == []
    f = LatePutFencing(baseline)
    assert f.observe("n1", {"CASRefNeedsRecovery": 7}) == []
    found = f.observe("n1", {"CASRefNeedsRecovery": 8})
    assert len(found) == 1 and "CASRefNeedsRecovery=1" in found[0]


def test_a_run_that_never_sealed_an_epoch_says_so_rather_than_passing():
    """Zero violations because nothing ever happened is the absence of a test, and a report that reads
    identically to a real pass would launder it into one."""
    f = LatePutFencing()
    f.observe("n1", {e: 0 for e in LATE_PUT_EVIDENCE_EVENTS + LATE_PUT_VIOLATION_EVENTS})
    text = "\n".join(f.report_lines())
    assert "WARNING" in text and "never reached" in text
