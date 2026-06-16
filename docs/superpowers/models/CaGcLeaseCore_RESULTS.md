# CaGcLeaseCore — TLC results (B160 GC lease advisory heartbeat)

Model: `CaGcLeaseCore.tla`. Spec: `docs/superpowers/specs/2026-06-16-ca-gc-lease-heartbeat-design.md`.
Checker: TLC (`tmp/tla2tools.jar`), bounded `Actors={L1,L2}`, `MaxClock=4`, `MaxSeq=5`, `MaxFence=3`.
Run: `java -cp tmp/tla2tools.jar tlc2.TLC -config <cfg> CaGcLeaseCore.tla` (configs below; `CHECK_DEADLOCK FALSE` because the bounded spec legitimately terminates).

This is **bounded model checking, not a proof**: TLC exhausts all interleavings within the finite bounds. A clean check is strong evidence, not a theorem for unbounded clocks/seqs.

## What is modelled

One CAS register `state=[owner,seq,fence]` (the lease + leadership epoch) and a second register `hb=[owner,hbseq]` (the advisory pulse). Actors renew (seq++ once per round), run a multi-step round (seq frozen during it), heartbeat, retire (the terminal CAS commits only if still owner at the same fence), observe, and steal (one atomic CAS: owner:=me, seq++, fence++). A global `clock` `Tick` is one observation window and **atomically bumps an alive leader's `hb`** when `EnableHeartbeat` — this encodes the `H ≤ W` tuning structurally (an alive leader's pulse necessarily advances within any follower window; a dead leader's freezes).

## Invariants

- **`NoEpochCollision`** (SAFETY): no two distinct actors ever commit a retire at the same `fence` epoch. A displaced leader's retire is blocked (its `fence` was bumped by the steal), so a stolen epoch is never double-committed.
- **`NoFalseSteal`** (EFFICIENCY/B160): no steal ever fires against an alive, mid-round incumbent.

## Results

| config | `EnableHeartbeat` | invariants checked | distinct states | result |
|---|---|---|---|---|
| `CaGcLeaseCore_heartbeat` | TRUE (the fix) | NoEpochCollision, NoFalseSteal | 8,633 | **PASS** — no error |
| `CaGcLeaseCore_safety_noheartbeat` | FALSE | NoEpochCollision | 10,777 | **PASS** — no error |
| `CaGcLeaseCore_sab_noheartbeat` | FALSE (Sabotage) | NoFalseSteal | 618 (to first violation) | **VIOLATED** — B160 reproduced |

Conclusions:
1. **Safety is independent of the heartbeat.** `NoEpochCollision` holds with the heartbeat ON *and* OFF — the atomic single-CAS steal + `fence_seq` epoch isolation make every steal safe regardless of timing. The heartbeat addition does not weaken safety.
2. **The heartbeat eliminates the false-steal.** With `EnableHeartbeat=TRUE`, `NoFalseSteal` holds across the whole bounded space. With it FALSE, the false-steal is reachable.

## B160 counterexample (`sab_noheartbeat`, 7 states)

```
State 1  Init
State 2-3  Tick                  (clock advances; heartbeat OFF -> hb frozen)
State 4  Create(L2)              -> state=[owner=L2, seq=1], inRound[L2]=TRUE (L2 leads, mid-round)
State 5  ObserveOrSteal(L1)      -> L1 records obs=(L2, seq=1, hb frozen)
State 6  Tick                    (a full window passes; heartbeat OFF -> hb STILL frozen)
State 7  ObserveOrSteal(L1)      -> (owner,seq) frozen AND hb frozen -> L1 STEALS from the
                                    alive, mid-round L2  =>  falseSteal=TRUE
```
This is exactly B160: an alive-but-mid-round leader (its `seq` frozen for the duration of the round) is stolen from because the follower's window sees no change. With `EnableHeartbeat=TRUE`, State 6's `Tick` would have bumped L2's `hb`, so State 7 sees `hb` advanced → L1 backs off → no steal.

## Reproduce

```bash
cd docs/superpowers/models
JAR=../../../tmp/tla2tools.jar
for c in CaGcLeaseCore_heartbeat CaGcLeaseCore_safety_noheartbeat CaGcLeaseCore_sab_noheartbeat; do
  java -XX:+UseParallelGC -cp "$JAR" tlc2.TLC -workers auto -config $c.cfg CaGcLeaseCore.tla
done
```
