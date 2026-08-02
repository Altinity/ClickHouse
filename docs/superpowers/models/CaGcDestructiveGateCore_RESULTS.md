---
description: TLC evidence for the authoritative non-empty GC destructive gate and independent exact lifecycle erasure.
sidebar_label: GC destructive gate results
sidebar_position: 1
slug: /superpowers/models/ca-gc-destructive-gate-core-results
title: CaGcDestructiveGateCore — TLA+ gate results
doc_type: reference
---

# `CaGcDestructiveGateCore` — TLA+ gate results {#ca-gc-destructive-gate-core-results}

The model separates later whole-round physical condemnation/deletion from the
earlier catalog-only pre-fold erasure of one proved `Removing` lifecycle row.
The former requires an authoritative, non-empty and fully proven frontier with
no anomalies or carried holds. The latter consumes matching cleanup evidence
and absence of a hold from the authoritative adopted-parent row, plus the
current GC leader fence, before the current round derives its physical gate.

## Checker identity and temporal smoke {#checker-identity-and-temporal-smoke}

The recorded run used the official jar at `tmp/tla2tools-official.jar`,
overlaid at the runner's required `tmp/tla2tools.jar` path inside an isolated
mount namespace. The worktree's then-existing TLC 2.19 symlink was not
modified. Task 10g subsequently adopted these same official bytes as the one
ordinary pin for both safety and temporal checking; future runs need no
dual-jar overlay.

- TLC: `2026.07.18.145032`, revision `30cc360`.
- SHA-256: `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Temporal smoke: PASS. `EventuallyTrue == <> TRUE` completed with no error,
  2 generated / 1 distinct state, depth 1, in `00s`.
- Workers: 1, deterministic breadth-first search.

The former ordinary pin, TLC 2.19 revision `5a47802`, has a known temporal
checker defect and makes the shared smoke gate fail. It is no longer an
accepted checker for either safety or temporal runs.

## Exact runner tail {#exact-runner-tail}

```text
TEMPORAL SMOKE PASS
CONFIG                                      EXPECT     RESULT                                           SECONDS  VERDICT
sab_gate_omits_anomalies                    violation  violation:PhysicalDeleteOnlyWhenGateOpen         1        PASS
sab_gate_omits_holds                        violation  violation:PhysicalDeleteOnlyWhenGateOpen         0        PASS
sab_gate_omits_frontier                     violation  violation:PhysicalDeleteOnlyWhenGateOpen         0        PASS
sab_gate_accepts_empty_universe             violation  violation:PhysicalDeleteOnlyWhenGateOpen         0        PASS
sab_lifecycle_uses_global_suppression       violation  violation:ProvedRemovalEraseIsNotPhysicalSuppression 1        PASS
healthy                                     green      green                                            0        PASS
empty_universe                              green      green                                            0        PASS
anomaly                                     green      green                                            0        PASS
carried_hold                                green      green                                            1        PASS
budget_exhausted                            green      green                                            0        PASS
witness_healthy_physical_delete             violation  violation:WITNESS_HEALTHY_PHYSICAL_DELETE        0        PASS
witness_suppressed_removal_erase            violation  violation:WITNESS_SUPPRESSED_REMOVAL_ERASE       0        PASS
witness_empty_universe_suppressed           violation  violation:WITNESS_EMPTY_UNIVERSE_SUPPRESSED      1        PASS

ALL EXPECTATIONS MET
```

## Per-configuration evidence {#per-configuration-evidence}

All configs assert `TypeOK`. Every positive config also asserts
`PhysicalDeleteOnlyWhenGateOpen` and
`ProvedRemovalEraseIsNotPhysicalSuppression`; witness configs assert both
safety properties before their negated reachability invariant. TLC durations
below are the checker-reported durations; runner seconds are the coarser shell
measurement shown above.

| Config | Outcome | Generated / distinct | Depth | TLC duration | Runner seconds |
| --- | --- | ---: | ---: | ---: | ---: |
| `sab_gate_omits_anomalies` | `PhysicalDeleteOnlyWhenGateOpen` violated | 3 / 3 | 3 | `00s` | 1 |
| `sab_gate_omits_holds` | `PhysicalDeleteOnlyWhenGateOpen` violated | 3 / 3 | 3 | `00s` | 0 |
| `sab_gate_omits_frontier` | `PhysicalDeleteOnlyWhenGateOpen` violated | 3 / 3 | 3 | `00s` | 0 |
| `sab_gate_accepts_empty_universe` | `PhysicalDeleteOnlyWhenGateOpen` violated | 3 / 3 | 3 | `00s` | 0 |
| `sab_lifecycle_uses_global_suppression` | `ProvedRemovalEraseIsNotPhysicalSuppression` violated | 3 / 3 | 3 | `00s` | 1 |
| `healthy` | green | 18 / 13 | 6 | `00s` | 0 |
| `empty_universe` | green | 3 / 3 | 3 | `00s` | 0 |
| `anomaly` | green | 5 / 5 | 4 | `00s` | 0 |
| `carried_hold` | green | 5 / 5 | 4 | `00s` | 1 |
| `budget_exhausted` | green | 5 / 5 | 4 | `00s` | 0 |
| `witness_healthy_physical_delete` | `WITNESS_HEALTHY_PHYSICAL_DELETE` violated | 7 / 6 | 4 | `00s` | 0 |
| `witness_suppressed_removal_erase` | `WITNESS_SUPPRESSED_REMOVAL_ERASE` violated | 3 / 3 | 3 | `00s` | 0 |
| `witness_empty_universe_suppressed` | `WITNESS_EMPTY_UNIVERSE_SUPPRESSED` violated | 2 / 2 | 2 | `00s` | 1 |

## Sabotage counterexamples {#sabotage-counterexamples}

- `sab_gate_omits_anomalies`: `{Target, Other}` is authoritative and fully
  proven, but `Anomalies = {Other}`. Omitting only that conjunct makes
  `gateOpen = TRUE`; `CondemnPhysicalCandidate` then sets `condemned = TRUE`
  and violates the honest gate.
- `sab_gate_omits_holds`: the frontier is complete and anomaly-free, but
  `CarriedHolds = {Other}`. Omitting only the hold conjunct admits the same
  condemned state.
- `sab_gate_omits_frontier`: `CatalogUniverse = {Target, Other}` while
  `FrontierProven = {Target}`. Omitting only equality of the non-empty frontier
  admits condemnation with a strict subset proved.
- `sab_gate_accepts_empty_universe`: `CatalogUniverse = {}` and
  `FrontierProven = {}` make equality true. Omitting only the non-empty floor
  sets both `emptyEqualityObserved = TRUE` and `gateOpen = TRUE`, after which
  condemnation violates the property.
- `sab_lifecycle_uses_global_suppression`: the target has exact adopted-parent
  evidence, no target hold, and a current leader fence. The harness also
  constructs a later physical gate closed by an unrelated anomaly. The
  sabotage incorrectly delays or predicates the pre-fold erasure on that later
  gate, recording `lifecycleBlockedBySuppression = TRUE` instead of erasing the
  row and violating `ProvedRemovalEraseIsNotPhysicalSuppression`.

The four physical traces stop at condemnation because it is the earliest
irreversible physical arm. The healthy witness below separately reaches the
later physical deletion, so the delete arm is not hidden behind that shortest
counterexample.

## Non-vacuity witnesses {#non-vacuity-witnesses}

### Healthy condemnation and deletion {#healthy-condemnation-and-deletion}

```text
Init:                        gateOpen=FALSE condemned=FALSE physicallyDeleted=FALSE
ComputeGate:                 gateOpen=TRUE  condemned=FALSE physicallyDeleted=FALSE
CondemnPhysicalCandidate:    gateOpen=TRUE  condemned=TRUE  physicallyDeleted=FALSE
PhysicallyDeleteCandidate:   gateOpen=TRUE  condemned=TRUE  physicallyDeleted=TRUE
```

The named witness requires both facts, rather than an aggregate
`physical_work_happened` flag.

### Suppressed physical work with exact erasure {#suppressed-physical-work-with-exact-erasure}

```text
Init:                 gateOpen=FALSE removalErased=FALSE
ComputeGate:          gateOpen=FALSE removalErased=FALSE
EraseProvedRemoval:   gateOpen=FALSE removalErased=TRUE
```

`TargetHeld = FALSE` records the authoritative parent's no-hold proof for the
target. `CarriedHolds = {Other}` is a later-round physical suppressor, so the
trace cannot be explained by weakening the target's pre-fold proof.

### Empty equality remains suppressed {#empty-equality-remains-suppressed}

```text
Init:          emptyEqualityObserved=FALSE gateOpen=FALSE
ComputeGate:   emptyEqualityObserved=TRUE  gateOpen=FALSE
```

This trace directly evaluates `CatalogUniverse = {}` and
`FrontierProven = {}`. It observes their equality but keeps physical
condemnation and deletion false because authority over an empty universe is
not destructive authority.

## Code correspondence {#code-correspondence}

The authoritative implementation establishes lifecycle authority first.
Immediately after lease acquisition, `Gc::runRegularRound` invokes
`Gc::drainCompletedRemoving` before heartbeat work, DEFER, the hot stream LIST,
the authoritative catalog cut, plan construction, or fold. The drain validates
the adopted parent seal and delegates to `CatalogLifecycleReconciler` with the
current leader generation and fence callback.

`CatalogLifecycleReconciler::selectEligible` selects only an exact catalog row
in `Removing` whose same-life row in the authoritative adopted parent contains
cleanup evidence and no durable hold. The reconciler then uses
`CasRefCatalog::deleteCompletedRemovingAtSnapshot` for the exact catalog CAS,
resolves its outcome, and rescans until no eligible row remains. Only the
orthogonal result `{AuthorityStatus::Authoritative,
CatalogResolution::DrainComplete}` permits the invocation to continue. This
path performs no physical cleanup and cannot consume anomalies, carried holds,
or frontier completeness that the current fold has not produced yet.

Later, `Gc::fold` computes the physical gate in `Gc/CasGc.cpp`:

```text
frontier_complete = universe_authoritative
                 && frontier_namespaces > 0
                 && frontier_proven == frontier_namespaces

suppress_destructive = anomalies
                    || carried_holds
                    || !frontier_complete
```

`FrontierComplete` and `PhysicalGateOpen` mirror these later-fold formulas using
finite sets, so `CatalogUniverse # {}` is the model counterpart of
`frontier_namespaces > 0`, and `FrontierProven = CatalogUniverse` is the
counterpart of equal proven and universe counts. Condemnation and physical
deletion consume this gate separately.

`TargetHasEvidence`, `TargetHeld`, and `GcFenceCurrent` abstract the earlier
adopted-parent/fence inputs owned by `CatalogLifecycleReconciler`; they are not
inferred from `PhysicalGateOpen`. Conversely, the later frontier and
suppressor inputs cannot authorize the pre-fold catalog mutation. The small
state machine co-locates both authorization facts so the forbidden coupling can
be sabotaged, but its `ComputeGate` harness step is not a claim that production
computes the fold gate before lifecycle reconciliation. The concrete pre-fold
ordering, stale-leader race, conclusive resolution, and all-row rescan are
consumed from `CaRefPreFoldDrainCore` and
`CaRefPreFoldDrainAllRowsCore`, rather than duplicated here.

## Verdict {#verdict}

All 13 expected rows passed under a smoke-capable pinned TLC jar. The four
physical gate conjunct controls and the lifecycle-level control are red by
their exact property names; all five honest safety configurations are green;
all three non-vacuity witnesses reach their named obligations.

Task 7b remains blocked unless review accepts every expected row and the
temporal-smoke gate continues to pass for the jar selected by the runner.
