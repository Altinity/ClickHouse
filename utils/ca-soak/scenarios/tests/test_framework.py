"""Pure unit tests for the scenario framework (no cluster, no docker).

Run: cd utils/ca-soak && python3 -m pytest scenarios/tests/ -q
"""

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
    before = {"CasBlobPut": 10, "CasBlobHead": 5}
    after = {"CasBlobPut": 17, "CasBlobHead": 5, "CasGcDelete": 3}
    d = observe.events_delta(before, after)
    assert d["CasBlobPut"] == 7
    assert "CasBlobHead" not in d  # zero delta dropped
    assert d["CasGcDelete"] == 3
    # counter reset (after < before) -> report post-reset absolute value
    d2 = observe.events_delta({"CasBlobPut": 100}, {"CasBlobPut": 4})
    assert d2["CasBlobPut"] == 4


def test_cluster_events_delta_total():
    before = {"ch1": {"CasBlobPut": 1}, "ch2": {"CasBlobPut": 2}}
    after = {"ch1": {"CasBlobPut": 5}, "ch2": {"CasBlobPut": 10}}
    out = observe.cluster_events_delta(before, after)
    assert out["ch1"]["CasBlobPut"] == 4
    assert out["ch2"]["CasBlobPut"] == 8
    assert out["_total"]["CasBlobPut"] == 12


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
