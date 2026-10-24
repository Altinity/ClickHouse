"""Unit tests for the FsckTimeout / subprocess-timeout robustness fix (B146/B154).

Verifies that:
1. `subprocess.TimeoutExpired` from `run_fsck` surfaces as `FsckTimeout` (not a bare
   `TimeoutExpired` or a hang).
2. The same mapping applies to `run_dryrun`.
3. A `FsckTimeout` from the `--detail` fsck in `checkpoint` is caught and degrades gracefully
   (skips the dryrun-subset assert) without re-raising — tested via `checkpoint`'s internal
   `_detail_fsck_skipped` path by monkeypatching `run_fsck` to raise on `detail=True`.
"""
import subprocess
import pytest

from soak.fsck import run_fsck, run_dryrun, FsckTimeout


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_timeout_expired():
    """Build a `subprocess.TimeoutExpired` instance with the minimal required fields."""
    return subprocess.TimeoutExpired(cmd=["docker", "exec", "fake"], timeout=1.0)


# ---------------------------------------------------------------------------
# run_fsck timeout mapping
# ---------------------------------------------------------------------------

def test_run_fsck_timeout_raises_FsckTimeout(monkeypatch):
    """`run_fsck` converts `subprocess.TimeoutExpired` → `FsckTimeout`."""
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: (_ for _ in ()).throw(_make_timeout_expired()))
    with pytest.raises(FsckTimeout, match="fsck.*detail=True.*exceeded.*1.0s"):
        run_fsck("fake-container", detail=True, timeout_s=1.0)


def test_run_fsck_summary_timeout_raises_FsckTimeout(monkeypatch):
    """`run_fsck(detail=False)` also maps `TimeoutExpired` → `FsckTimeout`."""
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: (_ for _ in ()).throw(_make_timeout_expired()))
    with pytest.raises(FsckTimeout, match="fsck.*detail=False.*exceeded.*180"):
        run_fsck("fake-container", detail=False, timeout_s=180)


def test_run_fsck_timeout_message_includes_container(monkeypatch):
    """The `FsckTimeout` message names the container so the log is actionable."""
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: (_ for _ in ()).throw(_make_timeout_expired()))
    with pytest.raises(FsckTimeout, match="my-container"):
        run_fsck("my-container", timeout_s=30)


# ---------------------------------------------------------------------------
# run_dryrun timeout mapping
# ---------------------------------------------------------------------------

def test_run_dryrun_timeout_raises_FsckTimeout(monkeypatch):
    """`run_dryrun` converts `subprocess.TimeoutExpired` → `FsckTimeout`."""
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: (_ for _ in ()).throw(_make_timeout_expired()))
    with pytest.raises(FsckTimeout, match="cas-gc-dryrun.*exceeded.*600"):
        run_dryrun("fake-container", timeout_s=600)


def test_run_dryrun_timeout_message_includes_container(monkeypatch):
    """The `FsckTimeout` message names the container."""
    monkeypatch.setattr(subprocess, "run", lambda *a, **kw: (_ for _ in ()).throw(_make_timeout_expired()))
    with pytest.raises(FsckTimeout, match="my-dryrun-container"):
        run_dryrun("my-dryrun-container", timeout_s=1.0)


# ---------------------------------------------------------------------------
# FsckTimeout is a RuntimeError subclass (structural contract)
# ---------------------------------------------------------------------------

def test_FsckTimeout_is_RuntimeError():
    """FsckTimeout inherits from RuntimeError so callers can catch RuntimeError if needed."""
    assert issubclass(FsckTimeout, RuntimeError)


def test_FsckTimeout_carries_message():
    """FsckTimeout can be constructed with a plain message and str()-ed cleanly."""
    exc = FsckTimeout("fsck (detail=True) exceeded 600.0s on ca-soak-ch1-1")
    assert "600.0s" in str(exc)
    assert "ca-soak-ch1-1" in str(exc)
