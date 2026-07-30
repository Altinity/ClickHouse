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
