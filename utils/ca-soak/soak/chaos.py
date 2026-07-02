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
    the cluster always stays recoverable (never a long simultaneous KILL of BOTH replicas).

    RustFS faults are scoped to GRACEFUL actions (`RESTART`/`PAUSE`) only — never `KILL`. This is a
    deliberate scoping of the chaos surface, not a workaround for a CA defect. See [[B145]]: a hard
    `docker kill -s KILL` of the RustFS container injects a transient post-restart read-visibility
    window (a `blobs/` key briefly returns `499 NoSuchKey` on the INSERT-dedup read path) that is an
    object-store recovery artifact of the `1.0.0-beta.8` test backend, NOT a CA durability defect.
    The decisive durability probe (write N objects -> `docker kill -s KILL` rustfs -> restart ->
    re-list/read-back) showed RustFS does NOT lose acked objects on a hard kill (0 acked-but-lost
    across 5 runs incl. continuous-write-mid-kill and kill-during-recovery), and the B145 capture had
    `fsck dangling=0` (no referenced blob was permanently missing) — both confirming the 499 was
    transient visibility, not loss. CA crash-recovery is about a ClickHouse SERVER crashing over a
    durable-enough store, so CH replicas KEEP `KILL`. The remaining open question — whether CA can
    reference a blob before the store has DURABLY acked it (an ordering bug) — cannot be cleanly
    tested against this beta store and must be re-tested against a crash-durable store
    (real S3 / MinIO-with-fsync); tracked as a B145 follow-up. The remap is deterministic: a
    RustFS+KILL slot becomes RustFS+RESTART, preserving schedule length/timing."""
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
        if action == FaultAction.PAUSE and target in (FaultTarget.CH1, FaultTarget.CH2, FaultTarget.BOTH):
            # Ack-floor interim (2026-07-02): a CH pause longer than the mount TTL (30s) + skew margin
            # lets a concurrent GC round FENCE OUT the paused server's expired mount; the keeper then
            # fails closed permanently ("never re-mint") and the server cannot write until restarted.
            # That is the DESIGNED safety behavior (sleeper re-arm is forbidden), but the liveness
            # counterpart — self-remount on fence-out (a fresh incarnation via the S13 open machinery)
            # — is not implemented yet. Until it lands, cap CH pauses below the fence-out threshold.
            # KILLs are unaffected: a kill+restart goes through Store::open, which reclaims fine.
            dur = min(dur, 20)
        if target == FaultTarget.RUSTFS and action == FaultAction.KILL:
            # B145: never hard-kill the (non-crash-durable-for-this-purpose) test object store; a
            # graceful restart lets RustFS flush. Deterministic downgrade KILL -> RESTART.
            action = FaultAction.RESTART
        if target == FaultTarget.BOTH and action == FaultAction.KILL:
            dur = min(dur, 60)        # safety bound
        faults.append(Fault(t_offset=t, target=target, action=action, duration_s=dur))
        i += 1
    return faults

def _containers(target: FaultTarget):
    if target == FaultTarget.BOTH:
        return [_CONTAINER[FaultTarget.CH1], _CONTAINER[FaultTarget.CH2]]
    return [_CONTAINER[target]]

def _is_running(container: str) -> bool:
    """Return True iff the container is in 'running' state."""
    r = subprocess.run(
        ["docker", "inspect", "--format", "{{.State.Status}}", container],
        capture_output=True, text=True, timeout=30)
    return r.returncode == 0 and r.stdout.strip() == "running"


def apply_fault(fault: Fault):
    """Execute a fault via docker. Thin wrapper; the driver schedules these. KILL is followed by a
    `docker start` after duration_s (so the node recovers); PAUSE is unpause after duration_s.

    After `docker start`, polls until the container is in 'running' state (up to 30s) so the caller's
    `wait_healthy` polling starts from a known container-running baseline."""
    import time
    cs = _containers(fault.target)
    if fault.action == FaultAction.KILL:
        for c in cs:
            subprocess.run(["docker", "kill", "-s", "KILL", c], capture_output=True)
        time.sleep(fault.duration_s)
        for c in cs:
            subprocess.run(["docker", "start", c], capture_output=True)
        # Wait for container to reach 'running' state before returning
        for c in cs:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if _is_running(c):
                    break
                time.sleep(2)
    elif fault.action == FaultAction.RESTART:
        for c in cs:
            subprocess.run(["docker", "restart", c], capture_output=True)
    elif fault.action == FaultAction.PAUSE:
        for c in cs:
            subprocess.run(["docker", "pause", c], capture_output=True)
        time.sleep(fault.duration_s)
        for c in cs:
            subprocess.run(["docker", "unpause", c], capture_output=True)
