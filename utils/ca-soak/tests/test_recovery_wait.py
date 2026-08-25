"""Unit tests for the Phase-2 recovery health-wait (`wait_for_healthy`) and the failure payload's
chaos fields. Pure: fake cluster/nodes whose `ping()` flips to healthy after N polls, injected
sleep/monotonic so no real time passes."""

import pytest

from soak.run import wait_for_healthy, wait_for_pool_consistent, settle_fsck_for_dump, _last_fault_dict
from soak.checker import CheckpointFailure
from soak.chaos import Fault, FaultTarget, FaultAction


class FakeNode:
    """ping() returns False for the first `down_for` calls, then True."""

    def __init__(self, name, down_for):
        self.name = name
        self.down_for = down_for
        self.calls = 0

    def ping(self, timeout=2.0):
        self.calls += 1
        return self.calls > self.down_for

    def query(self, sql, timeout=5.0):
        # `wait_for_healthy`'s readiness gate stopped being `ping()` alone on 2026-07-22: it now proves
        # the table is actually loaded by READING it, because /ping turns 200 before async table load
        # finishes. This fake answered only `ping`, so every node looked permanently un-loaded and all
        # three tests in this file failed — a stale fake, not a product defect. The read succeeds exactly
        # when the node is up, which is the behaviour the gate is written against.
        if not self.calls > self.down_for:
            raise RuntimeError(f"{self!r} is down")
        return "1"

    def __repr__(self):
        return f"FakeNode({self.name})"


class FakeCluster:
    def __init__(self, nodes):
        self._nodes = nodes

    def nodes(self):
        return tuple(self._nodes)


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def monotonic(self):
        return self.t

    def sleep(self, s):
        self.t += s


def test_wait_returns_once_both_healthy():
    clock = FakeClock()
    # node2 is down for 3 polls then recovers; node1 healthy throughout.
    cluster = FakeCluster([FakeNode("n1", down_for=0), FakeNode("n2", down_for=3)])
    wait_for_healthy(cluster, timeout_s=180, settle_s=2.0,
                     sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    # Returned without raising; some virtual time elapsed while polling.
    assert clock.t > 0


def test_wait_fails_loudly_if_node_never_returns():
    clock = FakeClock()
    # node2 stays down far beyond the timeout -> crash-recovery failure, raised loudly.
    cluster = FakeCluster([FakeNode("n1", down_for=0), FakeNode("n2", down_for=10**9)])
    with pytest.raises(CheckpointFailure) as ei:
        wait_for_healthy(cluster, timeout_s=10, settle_s=2.0,
                         sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    # The message became "healthy-with-tables-loaded" when the gate stopped trusting /ping alone
    # (2026-07-22); the old "HTTP-healthy" wording was pinned here and had been failing since.
    assert "never returned healthy-with-tables-loaded" in str(ei.value)


def test_wait_requires_settle_recheck():
    # If a node is healthy on the first probe but the settle re-check would still pass, it returns.
    clock = FakeClock()
    cluster = FakeCluster([FakeNode("n1", down_for=0), FakeNode("n2", down_for=0)])
    wait_for_healthy(cluster, timeout_s=180, settle_s=2.0,
                     sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    # Both nodes were probed at least twice (initial all() + settle recheck all()).
    assert all(n.calls >= 2 for n in cluster.nodes())


def test_pool_consistent_returns_once_transient_dangling_clears():
    # B144: right after a RustFS restart fsck transiently reports dangling>0 (HEAD-absent trees),
    # then the pool settles to dangling==0. The gate must wait it out and return the clean fsck.
    clock = FakeClock()
    seq = iter([
        {"dangling": 10, "exit_code": 36, "reachable": 6512, "unreachable": 4599},  # transient
        {"dangling": 3, "exit_code": 36, "reachable": 6520, "unreachable": 4500},    # settling
        {"dangling": 0, "exit_code": 0, "reachable": 6530, "unreachable": 4400},     # clean #1
        {"dangling": 0, "exit_code": 0, "reachable": 6530, "unreachable": 4400},     # clean #2 (stable)
    ])
    out = wait_for_pool_consistent(lambda: next(seq), timeout_s=180, stable=2, interval_s=3.0,
                                   sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    assert out["dangling"] == 0 and out["exit_code"] == 0


def test_pool_consistent_persistent_never_clean_raises():
    # B152/B185: dangling>0 on EVERY read, never once clearing -> a REAL durability finding, raise.
    clock = FakeClock()
    with pytest.raises(CheckpointFailure) as ei:
        wait_for_pool_consistent(
            lambda: {"dangling": 10, "exit_code": 36, "reachable": 6512, "unreachable": 4599},
            timeout_s=20, stable=2, interval_s=3.0, sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    assert "never reached a self-consistent state" in str(ei.value)
    assert "NEVER cleared once" in str(ei.value)
    assert "REAL crash-recovery" in str(ei.value)


def test_pool_consistent_flapping_clean_does_not_raise():
    # B152/B185: the pool reaches dangling==0 at least once but keeps FLAPPING (never `stable`
    # consecutive clean reads) until the bound. This is a settling artifact, NOT data loss (the
    # aggregate oracle is authoritative and asserted separately) -> warn + return the clean reading,
    # do NOT false-fail the soak.
    clock = FakeClock()
    n = {"i": 0}

    def flapping():
        # alternate dirty/clean forever so consecutive-clean never reaches 2.
        n["i"] += 1
        if n["i"] % 2 == 1:
            return {"dangling": 4, "exit_code": 36, "reachable": 6500, "unreachable": 4600}
        return {"dangling": 0, "exit_code": 0, "reachable": 6530, "unreachable": 4400}

    out = wait_for_pool_consistent(flapping, timeout_s=20, stable=2, interval_s=3.0,
                                   sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    # Returned (no raise) with the last CLEAN reading.
    assert out["dangling"] == 0 and out["exit_code"] == 0


def test_settle_fsck_for_dump_transient_clears_to_settled():
    # The failure-dump path must NOT record a bare fsck on a churning pool (B141/B144/B145): a
    # transient dangling that clears to a stable dangling==0 is recorded as a confirmed-clean
    # "settled" verdict, not a hard dangling claim.
    clock = FakeClock()
    seq = iter([
        {"dangling": 8, "exit_code": 36, "reachable": 6512, "unreachable": 4599},   # transient
        {"dangling": 0, "exit_code": 0, "reachable": 6530, "unreachable": 4400},     # clean #1
        {"dangling": 0, "exit_code": 0, "reachable": 6530, "unreachable": 4400},     # clean #2 (stable)
    ])
    f, status = settle_fsck_for_dump(lambda: next(seq), timeout_s=180, stable=2, interval_s=3.0,
                                     sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    assert status == "settled"
    assert f["dangling"] == 0


def test_settle_fsck_for_dump_persistent_dangling_labeled_not_swallowed():
    # A PERSISTENT post-settle dangling stays a real escalation: it is recorded (labeled
    # "persistent-dangling") rather than raised — we are already on the failure path and must finish
    # writing the dump — but it is NOT silently turned into a clean verdict.
    clock = FakeClock()
    persistent = {"dangling": 12, "exit_code": 36, "reachable": 6512, "unreachable": 4599}
    f, status = settle_fsck_for_dump(lambda: persistent, timeout_s=20, stable=2, interval_s=3.0,
                                     sleep_fn=clock.sleep, monotonic_fn=clock.monotonic)
    assert status == "persistent-dangling"
    assert f["dangling"] == 12


def test_settle_fsck_for_dump_fsck_unavailable_skipped():
    # If fsck itself cannot run (container gone, etc.) the dump makes NO dangling claim: status
    # "skipped", fsck None.
    def boom():
        raise RuntimeError("no such container")

    f, status = settle_fsck_for_dump(boom, timeout_s=20, stable=2, interval_s=3.0,
                                     sleep_fn=FakeClock().sleep, monotonic_fn=FakeClock().monotonic)
    assert status == "skipped"
    assert f is None


def test_last_fault_dict_shape():
    assert _last_fault_dict(None) is None

    class C:
        last_fault = Fault(t_offset=42, target=FaultTarget.CH1,
                           action=FaultAction.KILL, duration_s=15)

    d = _last_fault_dict(C())
    assert d == {"t_offset": 42, "target": "ch1", "action": "kill", "duration_s": 15}
