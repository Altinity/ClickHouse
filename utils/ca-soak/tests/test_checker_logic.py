import pytest

from soak.checker import (
    compare_aggregates,
    drive_gc_to_fixpoint,
    fixpoint_timeout_s,
    gc_fixpoint_reached,
    is_genuine_hang,
    poll_unreachable_to_stable,
    scaled_admin_timeout_s,
    unreachable_stable_band,
    wait_for_pool_drain,
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
    # A transient bump (a new orphan appearing mid-quiesce) must keep the window UNSTABLE until the
    # count truly settles within the band. Report-scale magnitudes (thousands, per
    # .superpowers/sdd/task3-soak-diag-report.md Q1's real history): [6000,6000,6200,...] -- the 6200
    # bump lands outside the band of the [6000,6000,6200] window (band=max(50,62)=62, spread=200) --
    # only the final [1600,1620,1610] settles (band=max(50,16.2)=50, spread=20) -> 1610.
    seq = iter([6000, 6000, 6200, 1600, 1620, 1610])
    mono, sleep = _fake_clock()
    assert poll_unreachable_to_stable(
        lambda: next(seq), timeout_s=10000, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono) == 1610


def test_poll_unreachable_band_tolerates_small_residual_noise():
    # The band criterion's whole point (report Q1/Q6): the server's leader GC keeps oscillating by
    # thousands per round even while genuinely converging, so 3 bit-for-bit-IDENTICAL samples are
    # structurally near-unreachable. A residual that settles WITHIN the band (not bit-for-bit equal)
    # must still be accepted as stable, unlike the old equality criterion.
    seq = iter([5000, 3000, 1622, 1600, 1610])
    mono, sleep = _fake_clock()
    assert poll_unreachable_to_stable(
        lambda: next(seq), timeout_s=10000, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono) == 1610


def test_poll_unreachable_never_settles_raises_after_bound():
    # An unbounded round-to-round swing of the report's own observed magnitude (0 <-> 4748, "CA GC
    # round" Q1) never lands within any band -> raise (a true timeout: the GC is still oscillating a
    # real backlog and the bound was too small). This is a bound/harness problem, NOT the
    # non-zero-residual case.
    import itertools
    seq = itertools.cycle([4748, 0])
    mono, sleep = _fake_clock()
    with pytest.raises(CheckpointFailure) as ei:
        poll_unreachable_to_stable(
            lambda: next(seq), timeout_s=30, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono)
    assert "never stabilized" in str(ei.value)


def test_poll_unreachable_failure_message_includes_drain_history():
    # The failure message must still carry the `unreachable` history AND (per the task's format
    # requirement) the drain trajectory when the caller supplied one.
    import itertools
    seq = itertools.cycle([4748, 0])
    mono, sleep = _fake_clock()
    with pytest.raises(CheckpointFailure) as ei:
        poll_unreachable_to_stable(
            lambda: next(seq), timeout_s=10, interval_s=3, stable=3, sleep_fn=sleep, monotonic_fn=mono,
            drain_history=[818754664, 35823959])
    msg = str(ei.value)
    assert "history=" in msg
    assert "drain_history=[818754664, 35823959]" in msg


def test_unreachable_stable_band_examples():
    # Still-oscillating report-scale history -> not stable.
    assert unreachable_stable_band([2837, 3177, 6203], stable=3) is False
    # Settled within the absolute floor band (50) -> stable.
    assert unreachable_stable_band([1622, 1600, 1610], stable=3) is True
    # Not enough history yet.
    assert unreachable_stable_band([80], stable=3) is False
    # A large residual gets a proportionally wide (1%-of-max) band, not just the floor.
    assert unreachable_stable_band([10000, 9950, 9920], stable=3, band_ratio=0.01, band_floor=50) is True
    assert unreachable_stable_band([10000, 9800, 9920], stable=3, band_ratio=0.01, band_floor=50) is False


def _fake_pool_probe(sizes):
    seq = iter(sizes)
    return lambda: next(seq)


def test_wait_for_pool_drain_stops_when_shrink_slows_below_band():
    # Monotonically draining pool that flattens out -- must stop polling once the relative
    # drop between consecutive samples falls below the 1% band, not run to a timeout.
    mono, sleep = _fake_clock()
    sizes = [1_000_000_000, 500_000_000, 100_000_000, 10_000_000, 9_995_000, 9_990_500]
    history = wait_for_pool_drain(
        _fake_pool_probe(sizes), interval_s=60, sleep_fn=sleep, monotonic_fn=mono, log_fn=lambda *_: None)
    # Stops as soon as 3 consecutive samples are each within 1% of the prior one.
    assert history == [1_000_000_000, 500_000_000, 100_000_000, 10_000_000, 9_995_000, 9_990_500]


def test_wait_for_pool_drain_returns_immediately_on_probe_failure():
    # A None reading (soak.pool.pool_size's best-effort failure contract) carries no signal -- must
    # not block the checkpoint waiting on an unrelated probe outage.
    mono, sleep = _fake_clock()
    calls = {"n": 0}

    def probe():
        calls["n"] += 1
        return None

    history = wait_for_pool_drain(
        probe, interval_s=60, sleep_fn=sleep, monotonic_fn=mono, log_fn=lambda *_: None)
    assert history == [None]
    assert calls["n"] == 1  # returns after the first failed read, does not retry


def test_wait_for_pool_drain_budget_scales_with_observed_rate_and_gives_up_gracefully():
    # A pool that keeps shrinking by a CONSTANT ~2% per sample forever never closes the 1% relative-
    # drop band (it is always draining "meaningfully"), so the wait can only end via its rate-scaled
    # budget expiring -- and even then it must log and return the trajectory rather than raise: this
    # precondition is a best-effort accelerant, not a second hard-failure point.
    mono, sleep = _fake_clock()
    sizes = []
    v = 1_000_000_000.0
    for _ in range(60):
        sizes.append(int(v))
        v *= 0.98
    logged = []
    history = wait_for_pool_drain(
        _fake_pool_probe(sizes), interval_s=60, cap_s=1800, sleep_fn=sleep, monotonic_fn=mono,
        log_fn=logged.append)
    assert len(history) < len(sizes)  # gave up before exhausting the fake sample source (60 samples)
    assert None not in history  # every read succeeded; this was a genuine slow-drain give-up
    assert any("exceeded its rate-scaled budget" in line for line in logged)


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


def test_drive_gc_to_fixpoint_no_pool_bytes_fn_skips_drain_wait():
    # Omitting `pool_bytes_fn` (the default, and every pre-existing call site until this fix) must
    # reproduce the ORIGINAL behavior exactly -- no drain-wait phase -- preserving the public contract.
    mono, sleep = _fake_clock()
    seq = iter([1751, 1751, 1200, 600, 100, 61, 61, 61])
    assert drive_gc_to_fixpoint(
        _FakeCluster(gc_interval_s=2), lambda: next(seq), sleep_fn=sleep, monotonic_fn=mono) == 61


def test_drive_gc_to_fixpoint_waits_for_pool_drain_before_polling_unreachable():
    # When `pool_bytes_fn` IS supplied (the new production wiring in soak/run.py), drive_gc_to_fixpoint
    # must run the drain-completion wait FIRST. Here the pool oscillates FOREVER (never within the 1%
    # band) so the drain-wait can only end by its own rate-scaled budget expiring, logging a give-up --
    # then the (band-tolerant) unreachable poll still runs to completion afterward.
    import itertools
    mono, sleep = _fake_clock()
    unreachable_seq = iter([1751, 1751, 1200, 600, 100, 61, 61, 61])
    pool_seq = itertools.cycle([1_000_000, 1])  # wildly oscillating -> drain-wait never settles
    logged = []
    result = drive_gc_to_fixpoint(
        _FakeCluster(gc_interval_s=2), lambda: next(unreachable_seq),
        pool_bytes_fn=lambda: next(pool_seq), drain_interval_s=1, sleep_fn=sleep, monotonic_fn=mono,
        log_fn=logged.append)
    assert result == 61
    assert any("exceeded its rate-scaled budget" in line for line in logged)  # drain-wait gave up
    assert any("pool drain" in line for line in logged)  # the drain-wait phase actually ran and logged


def test_drive_gc_to_fixpoint_pool_drain_probe_failure_falls_through():
    # A `pool_bytes_fn` that raises/returns None (probe outage) must not block the checkpoint --
    # falls straight through to the unreachable poll, same result as if it had been omitted.
    mono, sleep = _fake_clock()
    unreachable_seq = iter([1751, 1751, 1200, 600, 100, 61, 61, 61])
    result = drive_gc_to_fixpoint(
        _FakeCluster(gc_interval_s=2), lambda: next(unreachable_seq),
        pool_bytes_fn=lambda: None, drain_interval_s=1, sleep_fn=sleep, monotonic_fn=mono,
        log_fn=lambda *_: None)
    assert result == 61
