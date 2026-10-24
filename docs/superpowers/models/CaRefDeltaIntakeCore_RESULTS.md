# `CaRefDeltaIntakeCore` — Task 5b gate results {#carefdeltacore-task5b-results}

Model: `CaRefDeltaIntakeCore.tla`. Authority: §5 and §9 of
`2026-07-27-cas-ref-chain-complete-cut-design.md`, plus Task 5/5b of
`2026-07-28-cas-ref-chain-stage-b-catalog.md`.

Run: 2026-08-02, TLC 2.19, Java 21, `-workers auto`, uniform stream bound `MaxSeq = 2`.
Command: `bash docs/superpowers/models/run_deltaintake.sh`.

## Headline {#headline}

Task 5b retires the model's expected-next 404 frontier. The executable intake now composes the
catalog-built plan with the fold:

- `CatalogTargets` is derived only from the exact `Live`/`Removing` plan key set.
- `BeginRound` freezes exact `_ckpt.committed_through` as `roundCte`.
- `WalkStep` exact-reads only through that inclusive bound.
- a missing key at or below `roundCte` reasserts a hold without consulting `maxSeen` or any other LIST
  observation;
- `ScanComplete` requires every catalog target to reach CTE or resolve as held;
- a held target carried into the round must take that same resolution path even when LIST is totally
  silent.

The plan-only model and the fold are no longer disjoint: `BuildRefWalkPlans` is reachable in ordinary
configs and `BeginRound` requires `planBuilt`. The adapter actions remain isolated to `PlanOnly` so
they test construction without multiplying the fold state space.

## RED-first evidence {#red-first-evidence}

Before changing the protocol transitions, two test-only properties were added and run against the old
unguarded `ScanComplete`:

| control | log | semantic result |
|---|---|---|
| `_sab_skip_catalog_target` | `build/task5b_delta_intake_sab_skip_catalog_target_red.log` | exit 12; `EveryCatalogTargetAttempted` violated. The trace moves from `scanning` to `complete` with both `cand = csnap = 0` and both frontiers false. |
| `_sab_skip_held_retry` | `build/task5b_delta_intake_sab_skip_held_retry_red.log` | exit 12; `EveryCarriedHoldRetried` violated. The later round has `hold[t1] = TRUE`, `holdPos[t1] = 1`, `maxSeen[t1] = 0`, `cand[t1] = csnap[t1] = 0`, then completes unchanged. |

These are invariant violations, not parser/configuration failures. After the fix the same two configs
remain RED only because their isolated sabotage constants bypass the new completion guard.

## Final verdicts {#final-verdicts}

Harness log: `build/task5b_delta_intake_all_configs_final.log`. Exit 0, all 15 expectations met.

| cfg | expected / observed | generated / distinct | depth |
|---|---|---:|---:|
| `_sab_skip_catalog_target` | RED `EveryCatalogTargetAttempted` | 1,389 / 537 | 9 |
| `_sab_skip_held_retry` | RED `EveryCarriedHoldRetried` | 94,155 / 24,265 | 15 |
| `_sab_adaptermints` | RED `PlanKeySetExact` | 291 / 156 | 7 |
| `_sab_adoptbeforecommit` | RED `NoMissedFold` | 3,173 / 1,213 | 10 |
| `_sab_destroyunderhold` | RED `HoldSuppresses` | 156,116 / 36,973 | 15 |
| `_sab_rebuilddropshold` | RED `HoldReleaseRequiresFold` | 36,147 / 10,794 | 13 |
| `_sab_clearholdonabsent` | RED `HoldReleaseRequiresFold` | 8,734 / 3,148 | 12 |
| `_ctl_holdsuppresses` | GREEN | 4,624,557 / 765,417 | 35 |
| `_sab_cleanupignorescursor` | GREEN: exact CTE turns premature cleanup into a hold | 4,385,877 / 791,439 | 35 |
| `_sab_deleteignoresindeg` | RED `NoAckedLoss` | 452,574 / 106,486 | 20 |
| `_plan_safe` | GREEN | 2,307 / 257 | 10 |
| `_witness_planbuilt` | expected RED `WITNESS_PLAN_BUILT` | 2 / 2 | 2 |
| `_v9_safe` | GREEN | 1,560,985 / 300,741 | 32 |
| `_v9_hintomission` | GREEN: LIST returns nothing | 296,257 / 62,395 | 30 |
| `_v9_hold` | GREEN: LIST returns nothing and a committed key is hidden | 695,881 / 130,224 | 33 |

## Properties {#properties}

```tla
PlanKeySetExact ==
    planBuilt => (gcRefLives = WalkablePlanIds)

EveryCatalogTargetAttempted ==
    (gcPhase = "complete") => (\A t \in CatalogTargets : targetResolved[t])

EveryCarriedHoldRetried ==
    (gcPhase = "complete") =>
        (\A t \in CatalogTargets : heldAtStart[t] => targetResolved[t])

CteExact == \A t \in Tables : committedThrough[t] = MaxDurable(t)
RoundCteNoRegression == \A t \in Tables : roundCte[t] <= committedThrough[t]
HoldReleaseRequiresFold == ~AnyDebt
```

The original pool-wide properties remain checked: `NoMissedFold`, `NoAckedLoss`, `ExactlyOnce`,
`LosingCommitAdoptsNothing`, and `HoldSuppresses`.

## Total-omission held-gap non-vacuity {#total-omission-held-gap-non-vacuity}

`_v9_hold` binds `HintSilent = TRUE` and `EnableHiddenHole = TRUE`. Its coverage run is
`build/task5b_delta_intake_cov_v9_hold.log`: exit 0, 695,881 generated / 130,224 distinct, depth
33. The hidden-log action is enabled in 31,311 states and the committed-gap `ProbeAbsent` branch is
taken 6,120 times. Thus the green is not a quiet model: CTE observes a required missing key while LIST
returns nothing, the gap is held, and `EveryCarriedHoldRetried` remains invariant.

## Retired configurations {#retired-configurations}

| deleted cfg | old obligation | why it is unconstructible now | replacement |
|---|---|---|---|
| `_fix_ckptwitness` | checkpoint-snapshot coverage partially detects a LIST-hidden gap | `_ckpt.committed_through`, not `_ckpt.checkpoint`, is now the exact bound for every acknowledged key | `_sab_cleanupignorescursor` GREEN and `_v9_hold` GREEN |
| `_witness_corruptgap` | a gap is invisible when LIST omits the higher witness | CTE is a durable witness independent of LIST; every missing key at or below it holds | `_v9_hold` with `HintSilent = TRUE` |
| `_sab_skipquietprobe` | destruction treats an unhinted namespace as quiet | there is no quiet/expected-next branch; every catalog target must resolve through its CTE | `_sab_skip_catalog_target` RED |

The old result counts and two-round narratives that let a later hint omission reinterpret the same 404
as an ordinary frontier are deleted, not retained as current evidence.

## Bounds {#bounds}

The amended fold uses `MaxSeq = 2`. That is the smallest complete stream bound for the required
cross-namespace scenario: T1 has the hidden predecessor plus acknowledged `+1`, and T2 has the visible
`-1`. `PlanIds = 1..3` is independent of the stream bound so the catalog model still contains one
`Live`, one `Removing`, and one non-walkable `Creating` id for `_sab_adaptermints`.

A first `MaxSeq = 3` smoke was stopped as a non-result after two minutes at 643,001,400 generated /
65,191,484 distinct states, depth 23, with 19,289,920 queued. Its preserved log is
`build/task5b_delta_intake_v9_hintomission_green.log`; it is not cited as a verdict. The uniform bound
was shrunk according to the TLA-phase plan rather than dropping a property or scenario.

## Reproduce {#reproduce}

```bash
bash docs/superpowers/models/run_deltaintake.sh
```

The runner writes `build/task5b_delta_intake_<cfg>.log` and validates both the expected verdict and the
name of every violated invariant. It includes `_plan_safe`, `_sab_adaptermints`, and
`_witness_planbuilt`; the earlier runner's claim to run every config while omitting those three is fixed.
