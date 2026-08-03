"""Pure unit tests for the scenario framework (no cluster, no docker).

Run: cd utils/ca-soak && python3 -m pytest scenarios/tests/ -q
"""

import pytest

from scenarios.framework import observe, base
from scenarios.framework.report import (Verdict, ScenarioResult, worst_status,
                                         PASS, FAIL, INCONCLUSIVE, SKIPPED)
from scenarios.run import parse_duration


def test_parse_duration():
    assert parse_duration("15m") == 900
    assert parse_duration("90s") == 90
    assert parse_duration("2h") == 7200
    assert parse_duration("600") == 600
    assert parse_duration("500ms") == 1  # floors to >=1s


def test_events_delta_basic_and_reset():
    before = {"CASBlobPut": 10, "CASBlobHead": 5}
    after = {"CASBlobPut": 17, "CASBlobHead": 5, "CASGcDelete": 3}
    d = observe.events_delta(before, after)
    assert d["CASBlobPut"] == 7
    assert "CASBlobHead" not in d  # zero delta dropped
    assert d["CASGcDelete"] == 3
    # counter reset (after < before) -> report post-reset absolute value
    d2 = observe.events_delta({"CASBlobPut": 100}, {"CASBlobPut": 4})
    assert d2["CASBlobPut"] == 4


def test_cluster_events_delta_total():
    before = {"ch1": {"CASBlobPut": 1}, "ch2": {"CASBlobPut": 2}}
    after = {"ch1": {"CASBlobPut": 5}, "ch2": {"CASBlobPut": 10}}
    out = observe.cluster_events_delta(before, after)
    assert out["ch1"]["CASBlobPut"] == 4
    assert out["ch2"]["CASBlobPut"] == 8
    assert out["_total"]["CASBlobPut"] == 12


def test_worst_status_ordering():
    assert worst_status([]) == INCONCLUSIVE
    assert worst_status([Verdict("a", "", "", PASS)]) == PASS
    assert worst_status([Verdict("a", "", "", PASS), Verdict("b", "", "", SKIPPED)]) == SKIPPED
    assert worst_status([Verdict("a", "", "", PASS), Verdict("b", "", "", INCONCLUSIVE)]) == INCONCLUSIVE
    assert worst_status([Verdict("a", "", "", INCONCLUSIVE), Verdict("b", "", "", FAIL)]) == FAIL


def test_verdict_helpers():
    assert Verdict.check("x", "1", 1, True).status == PASS
    assert Verdict.check("x", "1", 2, False).status == FAIL
    assert Verdict.inconclusive("x", "1", "no data").status == INCONCLUSIVE
    assert Verdict.skipped("x", "n/a").status == SKIPPED


def test_scenario_result_finalize_and_markdown():
    r = ScenarioResult(scenario="S99", title="t", priority="P0", seed=1)
    r.add(Verdict.check("fsck dangling", "0", 0, True))
    r.add(Verdict.inconclusive("dryrun", "subset", "no detail"))
    r.finalize()
    assert r.status == INCONCLUSIVE  # worst of pass + inconclusive
    md = r.to_markdown()
    assert "S99" in md and "fsck dangling" in md and "Budget verdict" in md


def test_select_by_priority_and_name():
    # Registry is populated by importing cards; select() should resolve priorities and names.
    import scenarios.cards  # noqa: F401
    p0 = base.select("P0")
    assert all(c.priority == "P0" for c in p0)
    assert base.select("S01") and base.select("S01")[0].name == "S01"
    assert base.select("all")  # non-empty


class _FakeNode:
    """Node stub whose query() returns the next value from a list (last value repeats)."""

    def __init__(self, container, values):
        self.container = container
        self._values = list(values)

    def query(self, _query):
        return self._values.pop(0) if len(self._values) > 1 else self._values[0]


class _FakeCluster:
    def __init__(self, *nodes):
        self._nodes = list(nodes)

    def nodes(self):
        return self._nodes


def test_assert_replicas_agree_waits_for_replication_lag():
    # ch2 lags one poll behind (the S06/S07 2026-07-13 false-FAIL class), then converges: PASS.
    from scenarios.cards import _common
    r = ScenarioResult(scenario="S99", title="t", priority="P0", seed=1)
    cl = _FakeCluster(_FakeNode("ch1", ["50\thash"]),
                      _FakeNode("ch2", ["0\t0", "50\thash"]))
    sleeps = []
    agree = _common.assert_replicas_agree(r, cl, "q", sleep_fn=sleeps.append)
    assert agree is True
    assert r.verdicts[-1].status == PASS
    assert sleeps == [2.0]  # exactly one bounded poll, no real sleep in tests


def test_assert_replicas_agree_genuine_divergence_still_fails():
    # A divergence that never converges must exhaust the budget and FAIL (oracle not weakened).
    from scenarios.cards import _common
    r = ScenarioResult(scenario="S99", title="t", priority="P0", seed=1)
    cl = _FakeCluster(_FakeNode("ch1", ["50\thash"]),
                      _FakeNode("ch2", ["49\tother"]))
    sleeps = []
    agree = _common.assert_replicas_agree(r, cl, "q", attempts=3, sleep_fn=sleeps.append)
    assert agree is False
    assert r.verdicts[-1].status == FAIL
    assert len(sleeps) == 2  # attempts-1 polls, then the fail verdict


def test_assert_replicas_agree_error_value_counts_as_disagreement():
    # A transient per-node ERROR (readonly/keeper blip) is retried; persistent ERROR fails.
    from scenarios.cards import _common
    r = ScenarioResult(scenario="S99", title="t", priority="P0", seed=1)
    cl = _FakeCluster(_FakeNode("ch1", ["50\thash"]),
                      _FakeNode("ch2", ["ERROR: readonly", "50\thash"]))
    agree = _common.assert_replicas_agree(r, cl, "q", sleep_fn=lambda s: None)
    assert agree is True
    r2 = ScenarioResult(scenario="S99", title="t", priority="P0", seed=1)
    cl2 = _FakeCluster(_FakeNode("ch1", ["ERROR: down"]), _FakeNode("ch2", ["ERROR: down"]))
    agree2 = _common.assert_replicas_agree(r2, cl2, "q", attempts=2, sleep_fn=lambda s: None)
    assert agree2 is False  # identical ERROR values are NOT agreement


class _FakeQuiesceNode:
    """Node stub for quiesce_cluster: command()/scalar() are no-ops, counts come from _cluster_counts."""

    def __init__(self, container):
        self.container = container

    def command(self, *args, **kwargs):
        return None

    def scalar(self, *args, **kwargs):
        return "1700000000"


def test_quiesce_cluster_tolerates_transient_errored_entry(monkeypatch):
    # A replication_queue entry that briefly carries a last_exception (e.g. a connection reset
    # under a heavy-fetch burst) and clears on its own must NOT fail quiescence outright — only a
    # PERSISTENT error past the grace window is genuine (2026-07-19 S08 false-INCONCLUSIVE finding,
    # a single transient entry during a 20000-part creation burst tripped an instant-raise with no
    # grace period, unlike the sibling backlog-stall check which already has one).
    from scenarios.framework import lifecycle

    counts = [
        {"repl": 5, "mut": 0, "merges": 1, "errored": 1, "backlog": 5},
        {"repl": 3, "mut": 0, "merges": 1, "errored": 0, "backlog": 3},
        {"repl": 0, "mut": 0, "merges": 0, "errored": 0, "backlog": 0},
    ]
    monkeypatch.setattr(lifecycle, "_cluster_counts", lambda *a, **kw: counts.pop(0))
    times = iter([0, 0, 1, 2, 3])
    monkeypatch.setattr(lifecycle.time, "time", lambda: next(times))
    monkeypatch.setattr(lifecycle.time, "sleep", lambda s: None)

    cl = _FakeCluster(_FakeQuiesceNode("ch1"), _FakeQuiesceNode("ch2"))
    result = lifecycle.quiesce_cluster(cl, ["t"], optimize=False, no_progress_grace_s=5)
    assert result == 1700000000
    assert not counts  # all three polls consumed — drain returned normally, no raise


def test_quiesce_cluster_raises_on_persistent_errored_entry(monkeypatch):
    # An errored entry that never clears past the grace window is a genuine error and must still
    # fail quiescence (the tolerance above must not become a blanket suppression).
    from scenarios.framework import lifecycle

    counts = [
        {"repl": 5, "mut": 0, "merges": 1, "errored": 1, "backlog": 5},
        {"repl": 5, "mut": 0, "merges": 1, "errored": 1, "backlog": 5},
        {"repl": 5, "mut": 0, "merges": 1, "errored": 1, "backlog": 5},
    ]
    monkeypatch.setattr(lifecycle, "_cluster_counts", lambda *a, **kw: counts.pop(0))
    times = iter([0, 0, 1, 2, 4])
    monkeypatch.setattr(lifecycle.time, "time", lambda: next(times))
    monkeypatch.setattr(lifecycle.time, "sleep", lambda s: None)

    cl = _FakeCluster(_FakeQuiesceNode("ch1"), _FakeQuiesceNode("ch2"))
    with pytest.raises(RuntimeError, match="genuine error"):
        lifecycle.quiesce_cluster(cl, ["t"], optimize=False, no_progress_grace_s=2)
