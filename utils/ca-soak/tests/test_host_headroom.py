"""The preflight that would have caught S42 at full scale before it ate the disk.

S42 was killed 12 minutes in, consuming ~21 GB/minute on course to need ~380 GB against 218 GB free.
Nothing warned: the scenario runner has no pool-size cap of its own.

The memory half exists for a DIFFERENT reason than the disk half, and the tests pin that difference,
because it is the part a future reader is most likely to "simplify" away. Low disk RISKS the machine; low
free RAM INVALIDATES an allocation-fault experiment, since a real host OOM kill is indistinguishable from
an injected one.
"""
import builtins

from scenarios.framework.cluster_boot import check_host_headroom


def _quiet():
    lines = []
    return lines, lines.append


def test_ample_headroom_reports_no_concerns(monkeypatch):
    import shutil
    monkeypatch.setattr(shutil, "disk_usage", lambda _p: type("U", (), {"free": 500 << 30})())
    monkeypatch.setattr(builtins, "open", _fake_meminfo(64))
    lines, log = _quiet()
    assert check_host_headroom(log) == []


def test_low_disk_is_reported(monkeypatch):
    import shutil
    monkeypatch.setattr(shutil, "disk_usage", lambda _p: type("U", (), {"free": 20 << 30})())
    monkeypatch.setattr(builtins, "open", _fake_meminfo(64))
    lines, log = _quiet()
    concerns = check_host_headroom(log)
    assert any("disk" in c for c in concerns), concerns


def test_low_ram_says_the_run_is_INVALIDATED_not_merely_risky(monkeypatch):
    """The wording carries the reasoning. A future edit that flattens this into a generic 'low memory'
    warning loses the only thing that tells an operator the RESULT cannot be trusted."""
    import shutil
    monkeypatch.setattr(shutil, "disk_usage", lambda _p: type("U", (), {"free": 500 << 30})())
    monkeypatch.setattr(builtins, "open", _fake_meminfo(2))
    lines, log = _quiet()
    concerns = check_host_headroom(log)
    assert any("invalidates" in c.lower() for c in concerns), concerns


def test_unreadable_meminfo_is_a_concern_not_a_silent_pass(monkeypatch):
    """Fail loud, not closed-eyed: an unknown is recorded as unknown, never as fine."""
    import shutil
    monkeypatch.setattr(shutil, "disk_usage", lambda _p: type("U", (), {"free": 500 << 30})())

    def boom(*_a, **_k):
        raise OSError("no /proc here")

    monkeypatch.setattr(builtins, "open", boom)
    lines, log = _quiet()
    concerns = check_host_headroom(log)
    assert any("UNKNOWN" in c for c in concerns), concerns


def _fake_meminfo(avail_gb):
    import io as _io

    def _open(path, *a, **k):
        if str(path) == "/proc/meminfo":
            return _io.StringIO(f"MemTotal:       98765432 kB\nMemAvailable:   {avail_gb * (1 << 20)} kB\n")
        raise AssertionError(f"unexpected open({path})")

    return _open
