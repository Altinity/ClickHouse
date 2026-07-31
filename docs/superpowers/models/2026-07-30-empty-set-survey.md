---
description: Survey of empty entity-set configurations for the CAS GC TLA+ models.
sidebar_label: Empty set survey
sidebar_position: 1
slug: /superpowers/models/empty-set-survey-2026-07-30
title: Empty entity-set survey — 2026-07-30
doc_type: reference
---

# Empty entity-set survey {#empty-entity-set-survey}

## Method {#method}

The existing `*.cfg` files were rechecked before this survey. None assigns an
empty value to an entity collection. The only empty assignments are the
auxiliary `TreeHashes = {}` and `UniqueToBuildTree = {}` sets.

For each model with an entity collection supplied as a config `CONSTANT`, this
survey adds one config based on the indicated positive config, changing only
the selected collection to `{}`. Every TLC invocation used its own metadir
under `tmp/empty-set-survey-20260730/`; no missing-metadir-file error occurred.

Models whose entity universe is fixed in the module, or whose config constants
are solely scalar scenario values, bounds, or toggles, have no config that can
empty an entity set. No config was added for those models.

## Results {#results}

| Model | Entity constants identified | Empty-set config / base | Outcome |
| --- | --- | --- | --- |
| `CaB140DangleMerge` | `Leaders`, `Trees`, `Blobs` | `m_empty_trees.cfg` / `m_merged.cfg`; `Trees` | green |
| `CaBuildRootPrecommit` | `Builds`, `Trees`, `Blobs`; `BuildTree` is a scalar scenario binding, not a set | `CaBuildRootPrecommit_empty_trees.cfg` / `CaBuildRootPrecommit_fixed.cfg`; `Trees` | TLC error |
| `CaCasMountCore` | `Actors` | `CaCasMountCore_empty_actors.cfg` / `CaCasMountCore_v9_recoverygen.cfg`; `Actors` | green |
| `CaDiskLifecycle` | none; lifecycle is a single fixed in-module scenario | no config | not applicable |
| `CaEdgeBeforeObserve` | none; `Leaves` is fixed in-module | no config | not applicable |
| `CaErasureProof` | none; `Writers` and `Objects` are fixed in-module | no config | not applicable |
| `CaGcAckFloorCore` | `Writers`, `Blobs` | `CaGcAckFloorCore_empty_blobs.cfg` / `CaGcAckFloorCore_stage1.cfg`; `Blobs` | green |
| `CaGcAckFloorZombie` | `Writers`, `Leaders`, `Blobs` | `CaGcAckFloorZombie_empty_blobs.cfg` / `CaGcAckFloorZombie_stage1.cfg`; `Blobs` | green |
| `CaGcCondemnMarkerGate` | none; one hash and one writer are fixed in-module | no config | not applicable |
| `CaGcLeaseCore` | `Actors` | `CaGcLeaseCore_empty_actors.cfg` / `CaGcLeaseCore_heartbeat.cfg`; `Actors` | green |
| `CaGcRootLocalPartManifestCore` | `Namespaces`, `Writers`, `Leaders`, `Blobs`, `ManifestInstances`, `Refs`, `Builds`, `Paths`, `BuildPrefixes`, `Shards` | `CaGcRootLocalPartManifestCore_empty_namespaces.cfg` / `CaGcRootLocalPartManifestCore_stage6_attemptscoping.cfg`; `Namespaces` | green |
| `CaGcRoundDeferCore` | `Writers`, `Blobs` | `CaGcRoundDeferCore_empty_blobs.cfg` / `CaGcRoundDeferCore_stage1.cfg`; `Blobs` | green |
| `CaGcShardIncarnationCore` | `Blobs`, `Shards`, `Writers`, `Leaders` | `CaGcShardIncarnationCore_empty_shards.cfg` / `CaGcShardIncarnationCore_design.cfg`; `Shards` | green |
| `CaIncarnationCore` | `Writers`, `Leaders`, `Shards`, `Hashes`; `TreeHashes` is an auxiliary subset | `CaIncarnationCore_empty_hashes.cfg` / `CaIncarnationCore_stage6_registry.cfg`; `Hashes` | green |
| `CaRefCatalogCore` | none; the catalog models one fixed namespace | no config | not applicable |
| `CaRefDeltaIntakeCore` | no entity-set constant; `T1` and `T2` are scalar table identities used to construct `Tables` | no config | not applicable |
| `CaRefFoldClampRecoveryCore` | none; the two bodies and log are fixed in-module | no config | not applicable |
| `CaRefLaneCore` | none; the lane is a single fixed in-module scenario | no config | not applicable |
| `CaRefNsCleanupStaleLeaderCore` | none; the model has one fixed namespace | no config | not applicable |
| `CaRefTableSnapshotLogCore` | none; the table/log scenario is fixed in-module | no config | not applicable |
| `CaRefWriterCleanupCore` | `Builds` | `CaRefWriterCleanupCore_empty_builds.cfg` / `CaRefWriterCleanupCore_safe.cfg`; `Builds` | violation — temporal property `StalePrecommitEventuallyGone` |
| `CaRelinkConfirmCore` | `Receivers`; `Namespaces` and sources are constructed from it | `CaRelinkConfirmCore_empty_receivers.cfg` / `CaRelinkConfirmCore_main.cfg`; `Receivers` | green |
| `CaRelinkLaneComposition` | none; source/receiver bindings are fixed in-module | no config | not applicable |
| `CaRetiredInRun` | `Blobs` | `CaRetiredInRun_empty_blobs.cfg` / `CaRetiredInRun.cfg`; `Blobs` | green |
| `CaRetiredInRunFoldAbortWitness` | `Blobs`; `Leaders` is fixed in-module | `CaRetiredInRunFoldAbortWitness_empty_blobs.cfg` / `CaRetiredInRunFoldAbortWitness.cfg`; `Blobs` | green |

## Non-green TLC tails {#non-green-tlc-tails}

### `CaBuildRootPrecommit` {#cabuildrootprecommit}

```text
Starting... (2026-07-30 18:07:14)
Error: Assumption line 108, col 8 to line 108, col 26 of module CaBuildRootPrecommit is false.
Finished in 00s at (2026-07-30 18:07:14)
```

### `CaRefWriterCleanupCore` {#caref-writer-cleanup-core}

```text
Error: Temporal properties were violated.

Error: The following behavior constitutes a counter-example:

State 1: <Initial predicate>
/\ buildEpoch = << >>
/\ hasCommitted = << >>
/\ wrongfulReclaim = FALSE
/\ namespaceState = "Live"
/\ currentEpoch = 1
/\ buildState = << >>
/\ hasPrecommit = << >>

State 2: <Fence line 164, col 5 to line 167, col 38 of module CaRefWriterCleanupCore>
/\ buildEpoch = << >>
/\ hasCommitted = << >>
/\ wrongfulReclaim = FALSE
/\ namespaceState = "Live"
/\ currentEpoch = 2
/\ buildState = << >>
/\ hasPrecommit = << >>

State 3: <RemoveNamespace line 191, col 5 to line 196, col 76 of module CaRefWriterCleanupCore>
/\ buildEpoch = << >>
/\ hasCommitted = << >>
/\ wrongfulReclaim = FALSE
/\ namespaceState = "Removed"
/\ currentEpoch = 2
/\ buildState = << >>
/\ hasPrecommit = << >>

State 4: Stuttering
Finished checking temporal properties in 00s at 2026-07-30 18:07:21
9 states generated, 4 distinct states found, 0 states left on queue.
Finished in 00s at (2026-07-30 18:07:21)
```

## `CaRefWriterCleanupCore`: false temporal violation {#caref-writer-cleanup-core-false-temporal-violation}

This is a TLC tool defect in the supplied `tmp/tla2tools.jar`, not a model
defect and not an artefact of the liveness formula over an empty domain. The
jar identifies itself as TLC 2.19 (2024-08-08, revision `5a47802`).

The relevant configuration assigns `Builds = {}` and selects
`StalePrecommitEventuallyGone`. The module defines that property as
`StaleExists ~> NoStale`, where `StaleExists` is an existential quantification
over `Builds` and `NoStale` is the corresponding universal quantification.
By TLA+ semantics, the former is `FALSE`, the latter is `TRUE`, and `P ~> Q`
means `[] (P => <> Q)`. Thus the property is true in every behavior. The two
weak-fairness conjuncts in `Spec` do not change that result: weak fairness of
an action that is never enabled holds vacuously.

The reported trace is consistent with those state predicates: every
build-indexed function is `<< >>`. Independent `INVARIANT NoStale` checking
passes on exactly the same four-state graph. Replacing `~>` by its expansion
`[] (StaleExists => <> NoStale)` does not change the bad result, which rules
out an operator-precedence or shorthand-expansion error.

### Prediction and confirmation {#caref-writer-cleanup-core-prediction-and-confirmation}

Prediction: if the violation is caused by the supplied TLC's temporal checker,
rather than this model, then a throwaway one-variable module whose only
property is `EventuallyTrue == <> TRUE` will also report a violation with the
supplied jar, while a current official TLC jar will accept both that module and
the original empty-build configuration.

This prediction held. The files `tmp/exp_tautology.tla` and
`tmp/exp_tautology.cfg` define `Spec == Init /\ [][Next]_x` and
`EventuallyTrue == <> TRUE`. TLC 2.19 reported the same initial-state plus
stuttering counterexample. It also did so after removing the two fairness
conjuncts from a throwaway copy of `CaRefWriterCleanupCore`, so fairness is
not the mechanism. A current official jar (`tla2tools` 2026.07.18.145032,
revision `30cc360`) completed the minimal property successfully, then
completed the unmodified `CaRefWriterCleanupCore_empty_builds.cfg`
configuration successfully: four distinct states, no temporal error.

Therefore an empty `Builds` set is semantically valid for this model and this
property. Do not add an `ASSUME Builds # {}` merely to hide this result; that
would turn a checker defect into a model restriction. `CaBuildRootPrecommit`'s
explicit `ASSUME` is the right pattern only when the model genuinely requires
a non-empty domain. Empty-set conventions should run liveness properties with
a TLC version that passes the `<> TRUE` smoke test, keeping each run's
`-metadir` unique as usual.

## Temporal-sabotage audit {#temporal-sabotage-audit}

The only three runner rows that expect a temporal violation were audited with
the same model and configuration under both jars. Every invocation used a
distinct metadir. TLC 2.19 reports only an unnamed temporal failure; official
`tla2tools` 2026.07.18.145032 identifies the property explicitly.

| Runner | Model / row | TLC 2.19 | Official 2026.07.18.145032 | Conclusion |
| --- | --- | --- | --- | --- |
| `run_buildrootprecommit.sh` | `CaBuildRootPrecommit` / `lazyleak` | violates (rc 13) | `INV_NO_LEAK` violates (rc 13) | sound sabotage |
| `run_disklifecycle.sh` | `CaDiskLifecycle` / `sab_nogcselfexit` | violates (rc 13) | `GcExitsAfterVanished` violates (rc 13) | sound sabotage |
| `run_gcrounddefer.sh` | `CaGcRoundDeferCore` / `sab_unbounded_defer` | violates (rc 13) | `EventuallyFolded` violates (rc 13) | sound sabotage |

Thus none of these expectation rows was green solely because TLC 2.19
violates temporal properties incorrectly. `TlcTemporalSmoke.tla` and the
shared `tlc_temporal_gate.sh` make that fact a runner gate: each of these
runners now refuses temporal verdicts when its selected jar violates `<> TRUE`.

Recommendation: retain the pinned TLC 2.19 jar for now, because changing it
wholesale is outside this audit. The official jar passes the smoke test and is
the candidate replacement, but first re-run every configuration that declares
`PROPERTY` or `PROPERTIES` (43 configurations in this survey's inventory),
including their safety and temporal outcomes, then review and deliberately
accept any changed result before changing the runner jar.
