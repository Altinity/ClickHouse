import pytest

from soak.run import (
    parse_duration, metrics_interval_for, compute_throttle, _phase3_op_permitted,
    phase3_chaos_schedule, demote_dense_mutations, GB, METRICS_INTERVAL_S, _THROTTLE_MAX,
)
from soak.schedule import stage_plan, StageKind
from soak.ledger import Op, OpType, generate_ledger
from soak.chaos import FaultTarget, FaultAction


# --- parse_duration ----------------------------------------------------------------------------
def test_parse_duration_suffixes():
    assert parse_duration("600") == 600
    assert parse_duration(600) == 600
    assert parse_duration("600s") == 600
    assert parse_duration("90m") == 5400
    assert parse_duration("24h") == 86400
    assert parse_duration("1d") == 86400
    assert parse_duration("2h") == 7200


def test_parse_duration_bad():
    with pytest.raises((ValueError, KeyError)):
        parse_duration("")


# --- metrics_interval_for ----------------------------------------------------------------------
def test_metrics_interval_caps_at_production_60s():
    # A long (24h) run uses the full 60s production cadence.
    assert metrics_interval_for(24 * 3600) == METRICS_INTERVAL_S == 60


def test_metrics_interval_scales_down_for_short_runs():
    # A 600s self-check gets a denser tick (~30 samples) but never below 5s.
    iv = metrics_interval_for(600)
    assert 5 <= iv <= 60
    assert 600 // iv >= 25       # at least ~25 samples
    assert metrics_interval_for(60) == 5   # tiny run clamps to the 5s floor


# --- compute_throttle --------------------------------------------------------------------------
def test_throttle_unknown_pool_fail_closed_when_budget_set():
    # B204: unknown pool + budget set -> FAIL CLOSED (max throttle), not fail-open (keep current).
    assert compute_throttle(None, 40 * GB, current_sleep_s=0.0) == _THROTTLE_MAX
    assert compute_throttle(None, 40 * GB, current_sleep_s=0.25) == _THROTTLE_MAX


def test_throttle_unknown_pool_passthrough_when_no_budget():
    # No budget configured -> pool measurement is informational; keep the current throttle.
    assert compute_throttle(None, None, current_sleep_s=0.25) == 0.25
    assert compute_throttle(10 * GB, None, current_sleep_s=0.1) == 0.1
    assert compute_throttle(None, 0, current_sleep_s=0.3) == 0.3


def test_throttle_bands():
    budget = 40 * GB
    assert compute_throttle(int(0.5 * budget), budget, current_sleep_s=0.0) == 0.0
    assert compute_throttle(int(0.80 * budget), budget, current_sleep_s=0.0) == 0.05
    assert compute_throttle(int(0.95 * budget), budget, current_sleep_s=0.0) == 0.25
    assert compute_throttle(int(1.10 * budget), budget, current_sleep_s=0.0) == 1.0


def test_throttle_monotone_in_pressure():
    budget = 40 * GB
    vals = [compute_throttle(int(f * budget), budget, current_sleep_s=0.0)
            for f in (0.1, 0.8, 0.95, 1.2)]
    assert vals == sorted(vals)


# --- _phase3_op_permitted ----------------------------------------------------------------------
def _op(t):
    return Op(op_id=1, type=t, target=0, param=0)


def test_op_gating_per_stage():
    plan = {s.kind: s for s in stage_plan(3600)}
    warmup = plan[StageKind.WARMUP]
    assert _phase3_op_permitted(_op(OpType.INSERT), warmup)
    assert not _phase3_op_permitted(_op(OpType.OPTIMIZE), warmup)
    assert not _phase3_op_permitted(_op(OpType.UPDATE), warmup)
    assert not _phase3_op_permitted(_op(OpType.TRUNCATE), warmup)

    mut = plan[StageKind.MUTATIONS]
    assert _phase3_op_permitted(_op(OpType.UPDATE), mut)
    assert _phase3_op_permitted(_op(OpType.DELETE), mut)
    assert not _phase3_op_permitted(_op(OpType.TRUNCATE), mut)

    cliff = plan[StageKind.CLIFF]
    assert _phase3_op_permitted(_op(OpType.TRUNCATE), cliff)
    assert _phase3_op_permitted(_op(OpType.DROP_PARTITION), cliff)

    gc = plan[StageKind.GC_CHECKPOINT]
    for t in OpType:
        assert not _phase3_op_permitted(_op(t), gc), f"{t} should be blocked in GC checkpoint"


# --- phase3_chaos_schedule ---------------------------------------------------------------------
def test_chaos_schedule_confined_to_window_and_has_converge_restart():
    plan = stage_plan(3600)
    sched = phase3_chaos_schedule(20260613, plan, chaos_interval_s=90)
    chaos = next(s for s in plan if s.kind == StageKind.CHAOS)
    converge = next(s for s in plan if s.kind == StageKind.CONVERGE)
    # Every fault is at or after the chaos window start.
    assert all(f.t_offset >= chaos.t_start for f in sched)
    # A final both-replica RESTART is appended inside the converge tail.
    restarts = [f for f in sched
                if f.target == FaultTarget.BOTH and f.action == FaultAction.RESTART
                and f.t_offset >= converge.t_start]
    assert restarts, "expected a converge both-replica restart"


def test_chaos_schedule_deterministic():
    plan = stage_plan(1800)
    assert phase3_chaos_schedule(7, plan, 90) == phase3_chaos_schedule(7, plan, 90)


# --- demote_dense_mutations --------------------------------------------------------------------
def _mk(types):
    return [Op(op_id=i, type=t, target=0, param=0) for i, t in enumerate(types)]


def test_demote_keeps_first_mutation_then_spaces():
    led = _mk([OpType.UPDATE, OpType.UPDATE, OpType.DELETE, OpType.INSERT, OpType.DELETE])
    out = demote_dense_mutations(led, min_ops_between_mutations=3)
    kinds = [o.type for o in out]
    # op0 kept (first), op1 demoted (gap 1<3), op2 demoted (gap 2<3), op3 insert untouched,
    # op4 kept (gap from op0 = 4 >= 3).
    assert kinds == [OpType.UPDATE, OpType.OPTIMIZE, OpType.OPTIMIZE, OpType.INSERT, OpType.DELETE]


def test_demote_preserves_op_ids_and_nonmutations():
    led = _mk([OpType.INSERT, OpType.TRUNCATE, OpType.UPDATE, OpType.OPTIMIZE])
    out = demote_dense_mutations(led, min_ops_between_mutations=100)
    assert [o.op_id for o in out] == [0, 1, 2, 3]
    # non-mutations untouched; the lone UPDATE is the first mutation so it is KEPT.
    assert [o.type for o in out] == [OpType.INSERT, OpType.TRUNCATE, OpType.UPDATE, OpType.OPTIMIZE]


def test_demote_disabled_is_identity():
    led = generate_ledger(123, 500)
    assert demote_dense_mutations(led, 0) == list(led)


def test_demote_is_deterministic_and_sparser():
    led = generate_ledger(20260613, 2000)
    a = demote_dense_mutations(led, 80)
    b = demote_dense_mutations(led, 80)
    assert a == b
    raw_mut = sum(1 for o in led if o.type in (OpType.UPDATE, OpType.DELETE))
    kept_mut = sum(1 for o in a if o.type in (OpType.UPDATE, OpType.DELETE))
    assert kept_mut < raw_mut          # thinning actually removed some
    # every kept mutation is >= 80 ops after the previous kept one
    last = None
    for o in a:
        if o.type in (OpType.UPDATE, OpType.DELETE):
            if last is not None:
                assert o.op_id - last >= 80
            last = o.op_id
