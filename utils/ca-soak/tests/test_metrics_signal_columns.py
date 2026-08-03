"""The metrics sqlite must carry the new signals and survive being reopened by an older schema.

Both halves matter: a soak db is routinely reused across harness revisions, and if the first INSERT
after a schema addition fails the whole metrics curve for that run is lost — which is a quiet way to
lose exactly the observability this change adds.
"""

import sqlite3

from soak import metrics as metrics_mod
from soak.signals import CAS_SIGNAL_EVENTS, parse_phase_summary
from tests.test_signals import _phase_row


def test_signal_columns_round_trip(tmp_path):
    db = str(tmp_path / "m.db")
    conn = metrics_mod.open_db(db)
    snap = {"ts": 1, "node": "ch1", "fsck_stale_edge": 0}
    for i, e in enumerate(CAS_SIGNAL_EVENTS):
        snap[e] = i
    metrics_mod.record(conn, snap)
    got = metrics_mod.rows(conn)[0]
    assert got["fsck_stale_edge"] == 0
    for i, e in enumerate(CAS_SIGNAL_EVENTS):
        assert got[e] == i


def test_a_gap_is_stored_as_null_not_zero(tmp_path):
    """A node that was unreadable at a tick must be distinguishable, in the stored curve, from a node
    whose counters were genuinely zero."""
    db = str(tmp_path / "m.db")
    conn = metrics_mod.open_db(db)
    metrics_mod.record(conn, {"ts": 1, "node": "ch1"})
    got = metrics_mod.rows(conn)[0]
    for e in CAS_SIGNAL_EVENTS:
        assert got[e] is None


def test_open_db_migrates_a_db_written_by_an_older_harness(tmp_path):
    db = str(tmp_path / "old.db")
    old = sqlite3.connect(db)
    old.execute("CREATE TABLE metrics (ts INTEGER, node TEXT, parts_active INTEGER)")
    old.commit()
    old.close()

    conn = metrics_mod.open_db(db)
    metrics_mod.record(conn, {"ts": 7, "node": "ch1", "CasRefAppendWedged": 2})
    got = metrics_mod.rows(conn)[0]
    assert got["ts"] == 7
    assert got["CasRefAppendWedged"] == 2


def test_gc_phase_rows_persist(tmp_path):
    db = str(tmp_path / "m.db")
    conn = metrics_mod.open_db(db)
    parsed = parse_phase_summary("\n".join([
        _phase_row("fold_ref_list", total_us="900", ref_folding_aborted="2",
                   metrics={"ref_keys_listed": "10"}, events={"S3ListObjects": "4"}),
        _phase_row("fold_reduce", total_us="100", txns_unapplied="1"),
    ]))
    written = metrics_mod.record_phases(
        conn, metrics_mod.phase_row_dicts(1234, "GC checkpoint", "Node(ch1)", parsed))
    assert written == 2
    rows = metrics_mod.gc_phase_rows(conn)
    by_phase = {r["phase"]: r for r in rows}
    assert by_phase["fold_ref_list"]["ref_folding_aborted"] == 2
    assert by_phase["fold_ref_list"]["checkpoint"] == "GC checkpoint"
    # The whole maps survive the run: the load study does not yet know which key it will need.
    assert '"ref_keys_listed": 10' in by_phase["fold_ref_list"]["metrics_json"]
    assert '"S3ListObjects": 4' in by_phase["fold_ref_list"]["events_json"]
    assert by_phase["fold_reduce"]["txns_unapplied"] == 1
