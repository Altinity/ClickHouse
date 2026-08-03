---
description: TLC evidence for the CA GC leader lease's advisory heartbeat guarding against a false steal from a slow-but-alive incumbent.
sidebar_label: GC lease heartbeat results
sidebar_position: 1
slug: /superpowers/models/ca-gc-lease-core-results
title: CaGcLeaseCore — TLA+ gate results
doc_type: reference
---

# `CaGcLeaseCore` — TLA+ gate results {#ca-gc-lease-core-results}

The model checks the CA GC leader lease's B160 fix: a follower observing a foreign, frozen
`(owner, seq)` window steals leadership on the theory that the incumbent is dead. A slow-but-alive
incumbent bumps `seq` only once per round, so a round that spans many steps can look frozen to a
follower's observation window even though the incumbent is still running — a false steal. The fix
adds a second register, `hb`, that an alive leader bumps on a fast, round-independent cadence; a
follower steals only when both `(owner, seq)` and `hb` are frozen across its window.

## Checker identity {#checker-identity}

- TLC: `2026.07.18.145032`, revision `30cc360`.
- SHA-256: `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Workers: 1, deterministic breadth-first search.
- Bounds (all configs): `Actors = {L1, L2}`, `None = NoneOwner`, `MaxClock = 4`, `MaxSeq = 5`,
  `MaxFence = 3`.

## Exact runner tail {#exact-runner-tail}

```text
CONFIG                   EXPECT      RESULT                                   SECONDS  VERDICT
sab_noheartbeat          violation   violation:NoFalseSteal                   0        PASS
heartbeat                green       green                                    0        PASS
safety_noheartbeat       green       green                                    0        PASS

ALL EXPECTATIONS MET
```

## Per-configuration evidence {#per-configuration-evidence}

All configs assert `TypeOK`.

| Config | `EnableHeartbeat` | Asserted | Outcome | Generated / distinct |
| --- | --- | --- | --- | --- |
| `sab_noheartbeat` | `FALSE` | `NoFalseSteal` | `NoFalseSteal` violated | 90 / 59 |
| `heartbeat` | `TRUE` | `NoEpochCollision`, `NoFalseSteal` | green | 17,495 / 8,633 |
| `safety_noheartbeat` | `FALSE` | `NoEpochCollision` | green | 26,769 / 10,777 |

## Sabotage counterexample {#sabotage-counterexample}

`sab_noheartbeat` turns the fix off (`EnableHeartbeat = FALSE`) and asserts only `NoFalseSteal`.
An incumbent's round spans multiple `clock` ticks without bumping `state.seq`; a follower's
observation window opens and closes across that span, sees `(owner, seq)` unchanged, and steals —
reproducing B160 without the heartbeat register to distinguish "slow" from "dead".

## Non-vacuity: safety survives with the fix off {#non-vacuity}

`safety_noheartbeat` runs with the same `EnableHeartbeat = FALSE` as the sabotage but asserts only
`NoEpochCollision`, and it is green: the atomic single-CAS steal and fence-epoch isolation make
every steal safe regardless of timing, heartbeat or not. This isolates the defect precisely to
`NoFalseSteal` (an efficiency property — a live leader gets wrongly deposed) rather than to
`NoEpochCollision` (a safety property — two leaders would otherwise be able to commit at the same
fence epoch). The heartbeat exists to fix the former; it must never be load-bearing for the latter,
and `safety_noheartbeat` is the witness that it is not.

## Verdict {#verdict}

All three expected rows passed under the pinned TLC jar. The heartbeat is shown load-bearing for
`NoFalseSteal` (red without it, green with it) while `NoEpochCollision` is shown to hold
independently of the heartbeat in both directions.

## Reproduction {#reproduction}

```bash
docs/superpowers/models/run_gclease.sh
```
