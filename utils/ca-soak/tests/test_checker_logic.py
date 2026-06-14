import pytest

from soak.checker import (
    compare_aggregates,
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
    # A genuine non-reclaiming leak: unreachable stays at 61 forever -> raise once the grace-aware
    # bound elapses (must NOT mask it by declaring "stable").
    mono, sleep = _fake_clock()
    with pytest.raises(CheckpointFailure) as ei:
        poll_unreachable_to_zero(
            lambda: 61, timeout_s=60, interval_s=3, sleep_fn=sleep, monotonic_fn=mono)
    assert "unreachable==0" in str(ei.value) and "61" in str(ei.value)
