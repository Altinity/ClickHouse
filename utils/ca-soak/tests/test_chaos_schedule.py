from soak.chaos import generate_chaos_schedule, FaultAction, FaultTarget

def test_schedule_reproducible():
    a = generate_chaos_schedule(seed=9, duration_s=3600, mean_interval_s=300)
    b = generate_chaos_schedule(seed=9, duration_s=3600, mean_interval_s=300)
    assert a == b
    assert generate_chaos_schedule(seed=10, duration_s=3600, mean_interval_s=300) != a

def test_schedule_within_duration_and_typed():
    s = generate_chaos_schedule(seed=1, duration_s=3600, mean_interval_s=300)
    assert all(0 <= f.t_offset < 3600 for f in s)
    assert all(isinstance(f.target, FaultTarget) and isinstance(f.action, FaultAction) for f in s)

def test_no_long_kill_of_both_replicas():
    # safety bound: never a long simultaneous KILL of BOTH replicas (must stay recoverable)
    for seed in range(20):
        for f in generate_chaos_schedule(seed=seed, duration_s=7200, mean_interval_s=120):
            if f.target == FaultTarget.BOTH and f.action == FaultAction.KILL:
                assert f.duration_s <= 60

def test_ordered_by_time():
    s = generate_chaos_schedule(seed=5, duration_s=3600, mean_interval_s=300)
    assert [f.t_offset for f in s] == sorted(f.t_offset for f in s)
