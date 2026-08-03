"""The `stale_edge` hard assert, exercised through the REAL `checkpoint()` rather than a copy of it.

`tests/test_dryrun_subset_check.py` re-implements the logic it tests, which cannot catch a wiring
mistake. These tests drive `soak.run.checkpoint` itself with the cluster-facing calls stubbed, so they
fail if the assert is dropped, mis-ordered, or fed the wrong fsck result.

They also pin the escalation argument the assert depends on: `runFsck` counts every
present-but-unreferenced blob in `unreachable` BEFORE classifying it, so `unreachable > 0` is what
makes `checkpoint` escalate to a `--detail` scan — which is what makes the assert non-vacuous on
exactly the pools that could have a stale edge.
"""

import pytest

from soak import run as run_mod
from soak.checker import CheckpointFailure


CLEAN_SUMMARY = {
    "reachable": 10, "dangling": 0, "unreachable": 0, "pending_gc": 0, "awaiting_gc": 0,
    "unaccounted": 0, "stale_edge": 0, "physical_bytes": 100, "distinct_blobs": 10, "exit_code": 0,
}


class _Driver:
    def drain(self):
        pass


class _Model:
    def ambiguous_band_nonempty(self, now, eps=0):
        return False

    def aggregates(self, now):
        return {"count": 0, "sum": 0}

    def prune_expired(self, now):
        return 0


class _Cluster:
    node1 = object()
    node2 = object()

    def nodes(self):
        return (self.node1, self.node2)


@pytest.fixture
def stubbed(monkeypatch):
    """Stub every cluster-facing call `checkpoint` makes, and hand the test control of `run_fsck`."""
    calls = {"fsck": []}

    monkeypatch.setattr(run_mod, "quiesce", lambda cluster, table: 1000)
    monkeypatch.setattr(run_mod, "query_aggregates", lambda node, table: {"count": 0, "sum": 0})
    monkeypatch.setattr(run_mod, "compare_aggregates", lambda *a, **k: None)
    monkeypatch.setattr(run_mod, "drive_gc_to_fixpoint", lambda *a, **k: 0)
    monkeypatch.setattr(run_mod, "run_dryrun", lambda *a, **k: {"count": 0, "entries": []})
    monkeypatch.setattr(run_mod, "pool_size", lambda *a, **k: (0, 0))
    monkeypatch.setattr(run_mod, "log", lambda msg: calls.setdefault("log", []).append(msg))

    def install(fsck_fn):
        # `**kwargs` deliberately: this stub stands in for the REAL `run_fsck`, whose signature grows
        # (it gained `partial` on 2026-07-26). A stub that pins the signature turns every such addition
        # into a fake test failure, which teaches the reader to distrust the suite.
        def wrapped(container, disk="ca_ro", detail=True, timeout_s=600.0, **kwargs):
            calls["fsck"].append(detail)
            return fsck_fn(detail)
        monkeypatch.setattr(run_mod, "run_fsck", wrapped)

    calls["install"] = install
    return calls


def _run(phase=1):
    return run_mod.checkpoint(_Driver(), _Cluster(), _Model(), phase)


def test_clean_pool_passes_without_a_detail_scan(stubbed):
    """`unreachable == 0` proves the class is empty on its own, so a clean checkpoint stays cheap."""
    stubbed["install"](lambda detail: dict(CLEAN_SUMMARY))
    _run()
    assert True not in stubbed["fsck"], "a clean summary must not trigger the O(pool) --detail scan"
    assert any("stale_edge=0" in m for m in stubbed["log"])


def test_a_stale_edge_blob_fails_the_checkpoint(stubbed):
    """The escalation is what makes this reachable: a stale-edge blob is counted in `unreachable`, the
    summary therefore shows `unreachable > 0`, `checkpoint` escalates to `--detail`, and the detail
    scan is the one that runs the cross-check and reports a nonzero count."""
    def fsck(detail):
        r = dict(CLEAN_SUMMARY, unreachable=1)
        if detail:
            r["stale_edge"] = 1
            r["detail"] = [{"class": "stale-edge", "key": "blobs/aa/deadbeef", "size": 7}]
        return r

    stubbed["install"](fsck)
    with pytest.raises(CheckpointFailure) as ei:
        _run()
    assert "stale_edge" in str(ei.value)
    assert True in stubbed["fsck"], "the assert must be read off a --detail scan"


def test_a_binary_without_the_field_fails_closed(stubbed):
    """A missing `stale_edge=` means the server predates the class. The checkpoint must refuse rather
    than read the absence as a zero — otherwise the gate silently stops existing."""
    def fsck(detail):
        r = dict(CLEAN_SUMMARY)
        del r["stale_edge"]
        return r

    stubbed["install"](fsck)
    with pytest.raises(CheckpointFailure) as ei:
        _run()
    assert "predates" in str(ei.value)


def test_an_unknown_detail_class_is_reported_loudly(stubbed):
    """`run_fsck` records classes its whitelist does not know; nothing read them until now, so a class
    the product added was dropped from the checkpoint's view in silence."""
    def fsck(detail):
        r = dict(CLEAN_SUMMARY, unreachable=1)
        if detail:
            r["detail"] = []
            r["unknown_detail_classes"] = ["brand-new-class"]
        return r

    stubbed["install"](fsck)
    _run()
    assert any("fsck-whitelist" in m and "brand-new-class" in m for m in stubbed["log"])


def test_a_corrupted_run_row_is_reported(stubbed):
    """`corrupted-run` is an ERROR class that neither cas-fsck's exit code nor its summary line
    surfaces — it is observable only as a detail row, which is why the report is keyed on one."""
    def fsck(detail):
        r = dict(CLEAN_SUMMARY, unreachable=1)
        if detail:
            r["detail"] = [{"class": "corrupted-run", "key": "gc/run/1", "size": 3}]
        return r

    stubbed["install"](fsck)
    _run()
    assert any("corrupted-run" in m for m in stubbed["log"])


def test_a_timed_out_fsck_skips_the_assert_rather_than_faking_it(stubbed):
    """A slow fsck must never wedge or fail the soak (B146/B154) — but it must also not be reported as
    a clean stale-edge reading."""
    def fsck(detail):
        raise run_mod.FsckTimeout("boom")

    stubbed["install"](fsck)
    _run()
    assert not any("stale_edge=0" in m for m in stubbed["log"])
