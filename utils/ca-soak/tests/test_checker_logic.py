import pytest

from soak.checker import (
    compare_aggregates,
    drive_gc_to_fixpoint,
    fixpoint_timeout_s,
    gc_fixpoint_reached,
    poll_unreachable_to_zero,
    CheckpointFailure,
)


def test_compare_aggregates_match():
    exp = {"count": 10, "sum_fp": 123, "uniq_keys": 9, "sum_v": 5, "sum_version": 10, "min_op": 0, "max_op": 3}
    assert compare_aggregates(exp, exp, exp) is None     # model, node1, node2 all agree -> no failure


def test_compare_aggregates_mismatch_raises_with_detail():
    exp = {"count": 10, "sum_fp": 123, "uniq_keys": 9, "sum_v": 5, "sum_version": 10, "min_op": 0, "max_op": 3}
    got = dict(exp); got["count"] = 9                    # node1 lost a row
    try:
        compare_aggregates(exp, got, exp); assert False
    except CheckpointFailure as e:
        assert "count" in str(e) and "node1" in str(e)


def test_gc_fixpoint_two_stable_rounds():
    assert gc_fixpoint_reached([100, 90, 80, 80], stable=2) is True
    assert gc_fixpoint_reached([100, 90, 80, 70], stable=2) is False
    assert gc_fixpoint_reached([80], stable=2) is False   # not enough history


def _fake_clock():
    """A monotonic clock advanced only by the injected sleep_fn, so the loop is deterministic."""
    t = {"now": 0.0}
    return (lambda: t["now"]), (lambda dt: t.__setitem__("now", t["now"] + dt))


def test_poll_unreachable_drains_to_zero():
    # In-grace debris settles >0 (61,61,40) then drains to 0 -> the poll must reach 0, NOT declare a
    # false fixpoint on the stable [61,61] prefix.
    seq = iter([61, 61, 40, 0])
    mono, sleep = _fake_clock()
    assert poll_unreachable_to_zero(
        lambda: next(seq), timeout_s=180, interval_s=3, sleep_fn=sleep, monotonic_fn=mono) == 0


def test_poll_unreachable_stuck_raises_after_bound():
    # A genuine non-reclaiming leak: unreachable stays at 61 forever -> raise once the backlog-scaled
    # bound elapses (must NOT mask it by declaring "stable").
    mono, sleep = _fake_clock()
    with pytest.raises(CheckpointFailure) as ei:
        poll_unreachable_to_zero(
            lambda: 61, timeout_s=60, interval_s=3, sleep_fn=sleep, monotonic_fn=mono)
    assert "unreachable==0" in str(ei.value) and "61" in str(ei.value)


def test_fixpoint_timeout_small_backlog_hits_floor():
    # A small backlog still gets the generous floor (300s), not a tiny scaled value.
    assert fixpoint_timeout_s(100, gc_interval_s=2, floor_s=300) == 300


def test_fixpoint_timeout_large_backlog_scales():
    # A few-thousand-orphan post-TRUNCATE backlog needs many rounds: with interval 2s and the
    # default reclaim guess of 50/round, 5000 orphans -> 5 * (5000/50) * 2 = 1000s (> floor).
    assert fixpoint_timeout_s(5000, gc_interval_s=2, floor_s=300) == 1000
    # The bound is monotonic in the backlog and in the interval.
    assert fixpoint_timeout_s(5000, gc_interval_s=4) > fixpoint_timeout_s(5000, gc_interval_s=2)
    assert fixpoint_timeout_s(8000, gc_interval_s=2) > fixpoint_timeout_s(5000, gc_interval_s=2)


class _FakeCluster:
    def __init__(self, gc_interval_s=2):
        self.gc_interval_s = gc_interval_s


def test_drive_gc_to_fixpoint_zero_backlog_short_circuits():
    # No orphans at the checkpoint: returns immediately without polling.
    calls = {"n": 0}

    def fn():
        calls["n"] += 1
        return 0

    assert drive_gc_to_fixpoint(_FakeCluster(), fn) == 0
    assert calls["n"] == 1  # measured once, no poll loop


def test_drive_gc_to_fixpoint_drains_large_backlog():
    # A large post-TRUNCATE backlog that grinds down over many rounds must be driven to 0, using a
    # bound scaled to the initial reading (1751 here, the real B140 number). The first reading is
    # consumed by the up-front backlog measurement, then the poll loop drains it.
    mono, sleep = _fake_clock()
    seq = iter([1751, 1751, 1200, 600, 100, 0])
    assert drive_gc_to_fixpoint(
        _FakeCluster(gc_interval_s=2), lambda: next(seq), sleep_fn=sleep, monotonic_fn=mono) == 0
