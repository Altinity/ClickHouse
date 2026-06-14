from soak.schedule import (
    stage_plan, stage_at, chaos_window, StageKind, _STAGE_FRACTIONS,
)
import pytest


def test_fractions_sum_to_one():
    assert abs(sum(f for _, f in _STAGE_FRACTIONS) - 1.0) < 1e-9


def test_plan_tiles_duration_exactly():
    for dur in (600, 3600, 24 * 3600, 7, 100000):
        plan = stage_plan(dur)
        assert plan[0].t_start == 0
        assert plan[-1].t_end == dur, f"final stage must reach {dur}, got {plan[-1].t_end}"
        # contiguous, non-overlapping, monotone
        for a, b in zip(plan, plan[1:]):
            assert a.t_end == b.t_start
            assert a.t_start <= a.t_end


def test_plan_stage_order():
    kinds = [s.kind for s in stage_plan(24 * 3600)]
    assert kinds == [
        StageKind.WARMUP, StageKind.STEADY, StageKind.MUTATIONS, StageKind.TTL_PRESSURE,
        StageKind.GC_CHECKPOINT, StageKind.CHAOS, StageKind.CLIFF, StageKind.CONVERGE,
    ]


def test_plan_deterministic():
    assert stage_plan(600) == stage_plan(600)


def test_plan_compresses_24h_to_600s_same_shape():
    big = stage_plan(24 * 3600)
    small = stage_plan(600)
    assert [s.kind for s in big] == [s.kind for s in small]
    # The fraction of the timeline each stage occupies is preserved (within rounding).
    for sb, ss in zip(big, small):
        fb = (sb.t_end - sb.t_start) / (24 * 3600)
        fs = (ss.t_end - ss.t_start) / 600
        assert abs(fb - fs) < 0.05


def test_capabilities_progressive():
    plan = {s.kind: s for s in stage_plan(3600)}
    assert plan[StageKind.WARMUP].allow_inserts and not plan[StageKind.WARMUP].allow_mutations
    assert not plan[StageKind.WARMUP].chaos_armed
    assert plan[StageKind.MUTATIONS].allow_mutations
    assert plan[StageKind.CHAOS].chaos_armed
    assert plan[StageKind.CLIFF].allow_cliffs and plan[StageKind.CLIFF].chaos_armed
    # converge does NOT arm new chaos; GC checkpoint is a quiesced pause (no inserts).
    assert not plan[StageKind.CONVERGE].chaos_armed
    assert not plan[StageKind.GC_CHECKPOINT].allow_inserts


def test_only_chaos_and_cliff_arm_chaos():
    armed = {s.kind for s in stage_plan(3600) if s.chaos_armed}
    assert armed == {StageKind.CHAOS, StageKind.CLIFF}


def test_stage_at_resolves_each_window():
    plan = stage_plan(3600)
    for s in plan:
        mid = (s.t_start + s.t_end) // 2
        assert stage_at(plan, mid).kind == s.kind
    # at exactly t_start of a stage -> that stage (inclusive lower bound)
    assert stage_at(plan, plan[1].t_start).kind == plan[1].kind
    # past the end -> last stage (converge)
    assert stage_at(plan, 999999).kind == StageKind.CONVERGE
    assert stage_at(plan, 0).kind == StageKind.WARMUP


def test_chaos_window_is_chaos_through_cliff():
    plan = stage_plan(3600)
    start, end = chaos_window(plan)
    chaos = next(s for s in plan if s.kind == StageKind.CHAOS)
    cliff = next(s for s in plan if s.kind == StageKind.CLIFF)
    assert start == chaos.t_start
    assert end == cliff.t_end


def test_nonpositive_duration_raises():
    with pytest.raises(ValueError):
        stage_plan(0)
    with pytest.raises(ValueError):
        stage_plan(-5)
