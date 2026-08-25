"""Phase-3 time-driven stage schedule (CA soak §8 productionization timeline).

Phase 1 (steady) and Phase 2 (chaos) are OP-COUNT driven: the driver walks a fixed-length effective
ledger and checkpoints every N ops. Phase 3 is WALL-CLOCK driven: a 24h soak maps fractions of the
total `--duration` to a fixed sequence of STAGES, each progressively enabling more of the workload
surface (mutations, TTL pressure, GC, chaos, a truncate/drop cliff, a final converge+restart). The
op stream is the SAME deterministic ledger (`generate_ledger`); the schedule only decides, AT EACH
WALL-CLOCK MOMENT, which op CLASSES are permitted to fire and whether chaos is armed.

This module is the PURE, unit-testable core: `stage_plan` maps a duration (seconds) to an ordered
list of `Stage` windows whose `[t_start, t_end)` offsets tile `[0, duration)` exactly, and
`stage_at` resolves a wall-clock offset to its active stage. No I/O, no clock reads — the mapping is
a deterministic function of `duration` (and the stage-fraction table below). The fractions are fixed
(not seed-derived) so the timeline is reproducible and the SAME shape compresses from 24h down to a
10-minute self-check; `--seed` still drives the underlying ledger/chaos content, not the stage
boundaries.
"""

from dataclasses import dataclass
from enum import Enum


class StageKind(str, Enum):
    WARMUP = "warmup"                 # inserts only, NO chaos, NO mutations — fill the table
    STEADY = "steady"                 # inserts + merges (OPTIMIZE), steady state
    MUTATIONS = "mutations"           # + UPDATE/DELETE barriers
    TTL_PRESSURE = "ttl_pressure"     # + sustained insert pressure to exercise TTL eviction
    GC_CHECKPOINT = "gc_checkpoint"   # checkpoint + drive GC to fixpoint (quiesced)
    CHAOS = "chaos"                   # + chaos faults armed (full surface)
    CLIFF = "cliff"                   # truncate/drop cliff allowed
    CONVERGE = "converge"             # final converge + restart, no new chaos


# Stage fractions of the total duration, in timeline order. They SUM TO 1.0; `stage_plan` converts
# them to integer-second [t_start, t_end) windows that tile [0, duration) exactly (any rounding
# remainder is absorbed by the final stage so the windows always reach `duration`). Tuned so a 24h
# run spends the bulk of wall-clock in CHAOS (the hardest sustained surface) while still giving
# warmup/steady time to build a non-trivial table and a real converge tail.
_STAGE_FRACTIONS = [
    (StageKind.WARMUP, 0.05),
    (StageKind.STEADY, 0.10),
    (StageKind.MUTATIONS, 0.10),
    (StageKind.TTL_PRESSURE, 0.10),
    (StageKind.GC_CHECKPOINT, 0.05),
    (StageKind.CHAOS, 0.45),
    (StageKind.CLIFF, 0.05),
    (StageKind.CONVERGE, 0.10),
]


@dataclass(frozen=True)
class Stage:
    kind: StageKind
    t_start: int          # inclusive, seconds from run start
    t_end: int            # exclusive, seconds from run start
    # Capability flags resolved from the stage kind — what the driver may do while this stage is
    # active. CUMULATIVE in spirit (later stages enable more), but kept explicit per-stage so the
    # GC_CHECKPOINT pause and the CONVERGE tail can selectively DISARM chaos.
    allow_inserts: bool
    allow_optimize: bool
    allow_mutations: bool
    allow_cliffs: bool
    chaos_armed: bool

    def contains(self, t: float) -> bool:
        return self.t_start <= t < self.t_end


_CAPS = {
    #                         inserts optimize mutations cliffs  chaos
    StageKind.WARMUP:        (True,   False,   False,    False,  False),
    StageKind.STEADY:        (True,   True,    False,    False,  False),
    StageKind.MUTATIONS:     (True,   True,    True,     False,  False),
    StageKind.TTL_PRESSURE:  (True,   True,    True,     False,  False),
    StageKind.GC_CHECKPOINT: (False,  False,   False,    False,  False),  # quiesced GC pause
    StageKind.CHAOS:         (True,   True,    True,     False,  True),
    StageKind.CLIFF:         (True,   True,    True,     True,   True),
    StageKind.CONVERGE:      (True,   True,    True,     False,  False),  # no NEW chaos in the tail
}


def stage_plan(duration_s: int) -> list:
    """Map a total `duration_s` to the ordered list of `Stage` windows tiling `[0, duration_s)`.

    Pure function of `duration_s` and `_STAGE_FRACTIONS`. The windows are contiguous and
    non-overlapping; `t_start[0]==0` and `t_end[-1]==duration_s` exactly (the final stage absorbs any
    integer-rounding remainder so the timeline always reaches the full duration). Raises ValueError
    for a non-positive duration."""
    if duration_s <= 0:
        raise ValueError(f"duration_s must be positive, got {duration_s}")
    stages = []
    t = 0
    n = len(_STAGE_FRACTIONS)
    for i, (kind, frac) in enumerate(_STAGE_FRACTIONS):
        if i == n - 1:
            t_end = duration_s          # final stage absorbs the rounding remainder
        else:
            t_end = t + int(round(frac * duration_s))
            # Keep windows monotone and within bounds even for tiny durations.
            t_end = max(t, min(t_end, duration_s))
        caps = _CAPS[kind]
        stages.append(Stage(
            kind=kind, t_start=t, t_end=t_end,
            allow_inserts=caps[0], allow_optimize=caps[1], allow_mutations=caps[2],
            allow_cliffs=caps[3], chaos_armed=caps[4]))
        t = t_end
    return stages


def stage_at(plan: list, t: float):
    """Resolve a wall-clock offset `t` (seconds from run start) to its active `Stage`. Past the end
    of the timeline (t >= duration) returns the LAST stage so a slightly-overrunning driver keeps a
    well-defined (converge) capability set rather than indexing off the end."""
    for s in plan:
        if s.contains(t):
            return s
    return plan[-1]


def chaos_window(plan: list) -> tuple:
    """Return the (start, end) wall-clock offsets over which chaos is armed — the union of the
    chaos-armed stages (CHAOS + CLIFF). The Phase-3 chaos schedule is generated only for this window
    so faults never fire during warmup/steady/GC-checkpoint/converge. Returns (0, 0) if no stage arms
    chaos (degenerate tiny durations)."""
    armed = [s for s in plan if s.chaos_armed]
    if not armed:
        return (0, 0)
    return (armed[0].t_start, armed[-1].t_end)
