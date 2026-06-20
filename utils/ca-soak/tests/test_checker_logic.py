import pytest

from soak.checker import (
    compare_aggregates,
    drive_gc_to_fixpoint,
    fixpoint_timeout_s,
    gc_fixpoint_reached,
    is_genuine_hang,
    poll_unreachable_to_stable,
    scaled_admin_timeout_s,
    CheckpointFailure,
)


# --- merge-aware quiescence hang detection (is_genuine_hang) ---
# On CA-over-S3 a single large merge legitimately runs >10min with the rest of the queue postponed
# behind it; the backlog COUNT stays flat while real work executes. A flat count is a hang ONLY when
# NOTHING is executing.

def _hang(**over):
    base = dict(backlog_flat=True, active_merges=0, errored_queue=0,
                grace_exceeded=True, budget_exceeded=True, absolute_cap_exceeded=False)
    base.update(over)
    return is_genuine_hang(**base)


def test_hang_flat_idle_with_grace_and_budget_spent_is_hang():
    # Flat backlog, nothing executing, grace+budget spent -> true stall.
    assert _hang() == (True, "idle-flat")


def test_hang_flat_but_active_merge_is_not_a_hang():
    # The core CA/S3 case: flat COUNT but a long merge is executing -> NOT a hang, keep waiting.
    assert _hang(active_merges=1) == (False, "")


def test_hang_active_merge_overrides_absolute_cap():
    # Even past the absolute cap, an executing merge is never capped (cap only trips when idle).
    assert _hang(active_merges=1, absolute_cap_exceeded=True) == (False, "")


def test_hang_errored_queue_fails_fast_even_with_active_merge():
    # A genuine last_exception is a real error, distinct from slowness -> fail fast regardless.
    assert _hang(active_merges=2, errored_queue=1) == (True, "errored")


def test_hang_progressing_backlog_not_a_hang():
    # Backlog still shrinking (not flat) -> progress, keep waiting.
    assert _hang(backlog_flat=False) == (False, "")


def test_hang_grace_not_yet_exceeded_not_a_hang():
    assert _hang(grace_exceeded=False) == (False, "")


def test_hang_budget_not_yet_exceeded_not_a_hang():
    assert _hang(budget_exceeded=False) == (False, "")


def test_hang_absolute_cap_when_idle_is_capped():
    # Wedged-run backstop: cap tripped while nothing executes (and grace/budget not yet spent).
    assert _hang(grace_exceeded=False, budget_exceeded=False,
                 absolute_cap_exceeded=True) == (True, "capped")


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


def test_poll_unreachable_stabilizes_at_residual():
    # The incremental GC grinds down (1751,1200,600) then settles at its fixpoint residual 61 (the
    # known M-F-debris). poll-to-stable must RETURN 61 once it has settled for `stable` polls, NOT
    # require 0 (that would assert the unimplemented Full-GC) and NOT raise.
    seq = iter([1751, 1200, 600, 61, 61, 61])
    mono, sleep = _fake_clock()
    assert poll_unreachable_to_stable(
        lambda: next(seq), timeout_s=10000, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono) == 61


def test_poll_unreachable_zero_residual_returns_zero():
    # Once M-F lands the incremental GC drains fully; a stable 0 is just a residual of 0.
    seq = iter([100, 50, 0, 0, 0])
    mono, sleep = _fake_clock()
    assert poll_unreachable_to_stable(
        lambda: next(seq), timeout_s=10000, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono) == 0


def test_poll_unreachable_transient_bump_resets_stability():
    # A transient bump (a new orphan appearing mid-quiesce) must reset the stable run; the fixpoint is
    # only declared after the count has truly settled. Here [60,60,61,55,55,55] -> 55 (the 61 bump
    # breaks the early [60,60] run; only the final [55,55,55] settles).
    seq = iter([60, 60, 61, 55, 55, 55])
    mono, sleep = _fake_clock()
    assert poll_unreachable_to_stable(
        lambda: next(seq), timeout_s=10000, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono) == 55


def test_poll_unreachable_never_settles_raises_after_bound():
    # A perpetually decreasing count never settles within the bound -> raise (a true timeout: the GC
    # is still monotonically grinding and the bound was too small). This is a bound/harness problem,
    # NOT the non-zero-residual case.
    seq = iter(range(1000, 0, -1))
    mono, sleep = _fake_clock()
    with pytest.raises(CheckpointFailure) as ei:
        poll_unreachable_to_stable(
            lambda: next(seq), timeout_s=30, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono)
    assert "never stabilized" in str(ei.value)


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


def test_scaled_admin_timeout_floor_and_scaling():
    # An unknown/zero pool collapses to the generous floor.
    assert scaled_admin_timeout_s(0, floor_s=600, per_million_s=600, cap_s=3600) == 600
    assert scaled_admin_timeout_s(None, floor_s=600, per_million_s=600, cap_s=3600) == 600
    # One million objects -> floor + one increment.
    assert scaled_admin_timeout_s(1_000_000, floor_s=600, per_million_s=600, cap_s=3600) == 1200
    # The bound is monotonic in pool size, and capped.
    assert (scaled_admin_timeout_s(2_000_000, floor_s=600, per_million_s=600, cap_s=3600)
            > scaled_admin_timeout_s(1_000_000, floor_s=600, per_million_s=600, cap_s=3600))
    assert scaled_admin_timeout_s(50_000_000, floor_s=600, per_million_s=600, cap_s=3600) == 3600


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


def test_drive_gc_to_fixpoint_grinds_large_backlog_to_residual():
    # A large post-TRUNCATE backlog (1751, the real B140 number) grinds down over many rounds and
    # settles at its incremental-GC fixpoint residual 61 (the known M-F-debris). drive must RETURN 61
    # (not 0, not raise) using a bound scaled to the initial reading. The first reading is consumed by
    # the up-front backlog measurement, then the poll loop grinds it to the stable residual.
    mono, sleep = _fake_clock()
    seq = iter([1751, 1751, 1200, 600, 100, 61, 61, 61])
    assert drive_gc_to_fixpoint(
        _FakeCluster(gc_interval_s=2), lambda: next(seq), sleep_fn=sleep, monotonic_fn=mono) == 61
