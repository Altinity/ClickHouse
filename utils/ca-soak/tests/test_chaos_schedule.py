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

def test_rustfs_never_killed():
    # B145: the RustFS test object store (1.0.0-beta.8) is scoped to GRACEFUL faults only. A hard
    # KILL injects a transient post-restart read-visibility window (499 NoSuchKey on a referenced
    # blobs/ key) that is a backend-recovery artifact, not a CA durability defect (durability probe:
    # 0 acked-but-lost objects on kill -9; B145 capture had fsck dangling=0). CH replicas keep KILL.
    for seed in range(50):
        for f in generate_chaos_schedule(seed=seed, duration_s=7200, mean_interval_s=120):
            if f.target == FaultTarget.RUSTFS:
                assert f.action != FaultAction.KILL, (
                    f"RustFS fault must never be KILL (got {f.action} at t={f.t_offset}, seed={seed})"
                )

def test_ch_replicas_still_killed():
    # The CA-relevant crash — a ClickHouse SERVER crashing over a durable-enough store — must still be
    # exercised: at least one CH-replica KILL should appear across a reasonable seed sweep.
    saw_ch_kill = False
    for seed in range(50):
        for f in generate_chaos_schedule(seed=seed, duration_s=7200, mean_interval_s=120):
            if f.target in (FaultTarget.CH1, FaultTarget.CH2, FaultTarget.BOTH) and f.action == FaultAction.KILL:
                saw_ch_kill = True
                break
        if saw_ch_kill:
            break
    assert saw_ch_kill, "expected at least one ClickHouse-replica KILL across the seed sweep"
