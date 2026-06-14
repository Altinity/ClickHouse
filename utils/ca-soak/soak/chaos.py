import subprocess
from dataclasses import dataclass
from enum import Enum
from soak.rng import splitmix64

class FaultTarget(str, Enum):
    CH1 = "ch1"
    CH2 = "ch2"
    BOTH = "both"
    RUSTFS = "rustfs"

class FaultAction(str, Enum):
    KILL = "kill"        # docker kill -s KILL (hard crash)
    RESTART = "restart"  # docker restart
    PAUSE = "pause"      # docker pause + unpause after duration

@dataclass(frozen=True)
class Fault:
    t_offset: int        # seconds from run start
    target: FaultTarget
    action: FaultAction
    duration_s: int      # for PAUSE: how long paused; for KILL: downtime before auto-restart

# container names from docker-compose (project "ca-soak")
_CONTAINER = {FaultTarget.CH1: "ca-soak-ch1-1", FaultTarget.CH2: "ca-soak-ch2-1",
              FaultTarget.RUSTFS: "ca-soak-rustfs1-1"}

_TARGETS = [FaultTarget.CH1, FaultTarget.CH2, FaultTarget.BOTH, FaultTarget.RUSTFS]
_ACTIONS = [FaultAction.KILL, FaultAction.RESTART, FaultAction.PAUSE]

def generate_chaos_schedule(seed: int, duration_s: int, mean_interval_s: int):
    """Deterministic fault schedule from a seed. Poisson-ish inter-arrival via splitmix64. Bounded so
    the cluster always stays recoverable (never a long simultaneous KILL of BOTH replicas)."""
    faults = []
    t = 0
    i = 0
    while True:
        r = splitmix64(seed ^ (i * 0x9E3779B1))
        # inter-arrival in [0.3, 1.7] * mean (deterministic, no floats-from-clock)
        gap = (mean_interval_s * (30 + (r % 140))) // 100
        t += max(1, gap)
        if t >= duration_s:
            break
        r2 = splitmix64(r)
        target = _TARGETS[(r2 >> 3) % len(_TARGETS)]
        action = _ACTIONS[(r2 >> 7) % len(_ACTIONS)]
        dur = 5 + ((r2 >> 11) % 56)   # 5..60s
        if target == FaultTarget.BOTH and action == FaultAction.KILL:
            dur = min(dur, 60)        # safety bound
        faults.append(Fault(t_offset=t, target=target, action=action, duration_s=dur))
        i += 1
    return faults

def _containers(target: FaultTarget):
    if target == FaultTarget.BOTH:
        return [_CONTAINER[FaultTarget.CH1], _CONTAINER[FaultTarget.CH2]]
    return [_CONTAINER[target]]

def apply_fault(fault: Fault):
    """Execute a fault via docker. Thin wrapper; the driver schedules these. KILL is followed by a
    `docker start` after duration_s (so the node recovers); PAUSE is unpause after duration_s."""
    import time
    cs = _containers(fault.target)
    if fault.action == FaultAction.KILL:
        for c in cs:
            subprocess.run(["docker", "kill", "-s", "KILL", c], capture_output=True)
        time.sleep(fault.duration_s)
        for c in cs:
            subprocess.run(["docker", "start", c], capture_output=True)
    elif fault.action == FaultAction.RESTART:
        for c in cs:
            subprocess.run(["docker", "restart", c], capture_output=True)
    elif fault.action == FaultAction.PAUSE:
        for c in cs:
            subprocess.run(["docker", "pause", c], capture_output=True)
        time.sleep(fault.duration_s)
        for c in cs:
            subprocess.run(["docker", "unpause", c], capture_output=True)
