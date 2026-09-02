"""A `docker exec` that never ran the fsck applet is an UNAVAILABLE probe, not a durability finding.

Seen on the live-GCS stand 2026-09-02: `docker exec` into a container that was not running returned
exit 1 and no `reachable=` line, `run_fsck` handed back a dict with no `dangling` key, and the
checkpoint's `f.get("dangling") != 0` read `None != 0` as data loss (`INV-NO-LOSS`) before the
`exit_code` gate could say what really happened. The probe layer must refuse to return an
unparseable result; the checkpoint then treats it like a timeout: loud skip, soak continues."""
import subprocess

import pytest

from soak.fsck import FsckUnavailable, FsckTimeout, run_dryrun, run_fsck


def _completed(rc, stdout="", stderr=""):
    return subprocess.CompletedProcess(args=["docker", "exec", "fake"], returncode=rc, stdout=stdout, stderr=stderr)


def test_run_fsck_without_summary_line_raises_FsckUnavailable(monkeypatch):
    monkeypatch.setattr(
        subprocess, "run",
        lambda *a, **kw: _completed(1, stderr="Error response from daemon: container abc is not running\n"),
    )
    with pytest.raises(FsckUnavailable, match="gone-container.*is not running"):
        run_fsck("gone-container", detail=False, timeout_s=30)


def test_run_fsck_nonzero_exit_with_summary_still_returns_the_summary(monkeypatch):
    """The applet exits nonzero on findings it reports (dangling, chain_broken); that is a RESULT."""
    monkeypatch.setattr(
        subprocess, "run",
        lambda *a, **kw: _completed(1, stdout="reachable=10 dangling=2 unreachable=0 pending_gc=0 awaiting_gc=0 unaccounted=0 stale_edge=0\n"),
    )
    res = run_fsck("c", detail=False, timeout_s=30)
    assert res["dangling"] == 2
    assert res["exit_code"] == 1


def test_run_dryrun_without_output_raises_FsckUnavailable(monkeypatch):
    monkeypatch.setattr(
        subprocess, "run",
        lambda *a, **kw: _completed(1, stderr="Error response from daemon: No such container: gone\n"),
    )
    with pytest.raises(FsckUnavailable, match="gone.*No such container"):
        run_dryrun("gone", timeout_s=30)


def test_FsckUnavailable_is_a_RuntimeError_distinct_from_FsckTimeout():
    assert issubclass(FsckUnavailable, RuntimeError)
    assert not issubclass(FsckUnavailable, FsckTimeout)
    assert not issubclass(FsckTimeout, FsckUnavailable)


def test_run_fsck_zero_exit_without_summary_is_still_unavailable(monkeypatch):
    """The discriminating signal is the missing summary, not the exit code: a run that printed no
    `reachable=` line answered nothing, whatever it exited with."""
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: _completed(0, stdout="applet banner only\n"))
    with pytest.raises(FsckUnavailable, match="no summary"):
        run_fsck("c", detail=False, timeout_s=30)


# ---------------------------------------------------------------------------
# Through the REAL `checkpoint()`: an unavailable probe is a loud skip, never a durability verdict.
# Regression coverage written after the fix; the failure it guards was observed live (2026-09-02).
# ---------------------------------------------------------------------------

from soak import run as run_mod  # noqa: E402
from soak.checker import CheckpointFailure  # noqa: E402
from tests.test_checkpoint_stale_edge import _Cluster, _Driver, _Model  # noqa: E402


def test_checkpoint_skips_loudly_when_the_probe_never_ran(monkeypatch):
    logged = []
    monkeypatch.setattr(run_mod, "quiesce", lambda cluster, table: 1000)
    monkeypatch.setattr(run_mod, "query_aggregates", lambda node, table: {"count": 0, "sum": 0})
    monkeypatch.setattr(run_mod, "compare_aggregates", lambda *a, **k: None)
    monkeypatch.setattr(run_mod, "drive_gc_to_fixpoint", lambda *a, **k: 0)
    monkeypatch.setattr(run_mod, "run_dryrun", lambda *a, **k: {"count": 0, "entries": []})
    monkeypatch.setattr(run_mod, "pool_size", lambda *a, **k: (0, 0))
    monkeypatch.setattr(run_mod, "log", lambda msg: logged.append(msg))

    def never_ran(container, disk="ca_ro", detail=True, timeout_s=600.0, **kwargs):
        raise FsckUnavailable(f"cas-fsck produced no summary on {container}: exit=1 stderr='container is not running'")

    monkeypatch.setattr(run_mod, "run_fsck", never_ran)
    before = len(run_mod.SKIPPED_FSCK_GATES)
    try:
        run_mod.checkpoint(_Driver(), _Cluster(), _Model(), phase=1)
    except CheckpointFailure as e:  # pragma: no cover - the assertion below explains the failure
        pytest.fail(f"an unavailable probe was turned into a checkpoint failure: {e}")
    assert len(run_mod.SKIPPED_FSCK_GATES) > before, "the skipped gate was not recorded for the end-of-run report"
    assert any("did not complete" in m for m in logged), logged
    assert not any("dangling != 0" in m for m in logged), logged
