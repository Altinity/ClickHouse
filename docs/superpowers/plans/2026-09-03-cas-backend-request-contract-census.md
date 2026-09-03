---
description: Mechanical census of transport call sites, Backend&-declared functions, Token-carrying declarations, fence checks, test fault-injection seams, old controller surface, and persisted token formats under ContentAddressed/, cited as checklists by the request-contract migration plan.
sidebar_label: Backend request contract census
sidebar_position: 1
slug: /superpowers/plans/2026-09-03-cas-backend-request-contract-census
title: CAS backend request contract census
doc_type: reference
---

# CAS backend request contract census {#cas-backend-request-contract-census}

Mechanical census for the plan implementing `docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md`
(read its "Where each verb goes" and "Landing order" sections first for vocabulary). All counts below come from
commands shown above each table, run against `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` at
HEAD `b9df0724639` on branch `cas-gc-rebuild`. Nothing here is estimated.

## 1. Production transport call sites {#production-transport-call-sites}

Scope: every `.cpp`/`.h` under `ContentAddressed/` (all subdirectories) EXCLUDING the backend implementations
themselves — `Backend/CasBackend.h`, `Backend/CasObjectStorageBackend.{h,cpp}`, `Backend/CasInMemoryBackend.{h,cpp}`,
`Backend/CasEmulatedSingleProcess*`, `Backend/CasInstrumentedBackend.{h,cpp}` — 129 files remained after exclusion.

Commands:
```
find src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed -type f \( -name '*.cpp' -o -name '*.h' \) \
  | grep -Ev 'Backend/CasBackend\.h|Backend/CasObjectStorageBackend\.(h|cpp)|Backend/CasInMemoryBackend\.(h|cpp)|Backend/CasEmulatedSingleProcess|Backend/CasInstrumentedBackend\.(h|cpp)'
```
followed by, per method in `{get, getStream, head, putIfAbsent, putOverwrite, casPut, deleteExact, publishBlob,
list, probeSentinelRaw, putIfAbsentControlled, putOverwriteControlled, putIfAbsentControlledMutable, slotOccupy,
resolveByExactGet, forEachListedKey}`, a Python scan of each file (block comments stripped, `//`-tail stripped,
comment-only lines skipped) that records every `METHOD(` occurrence together with the identifier chain
immediately preceding it (the "receiver").

**Receiver filter** (disambiguates `get(`, `list(`, etc. from unrelated methods of the same name): for the ten
legacy transport methods, a hit counts only if its receiver is literally `backend`, `Backend`, `pool_backend`,
`store->backend()`, `store.backend()`, `admin->backend()`, `b` (confirmed a `Backend &` parameter at every site —
`CasServerRoot.cpp`'s free functions), `ref_backend`, or otherwise ends in `backend`/`Backend` or contains
`backend()`. This excluded confirmed non-`Backend` receivers: `view_cache->get`, `cached.get()`, `view.get()`,
`future.get()`, `tls_split_cache.get()`, `manifest_cache->get()`, `mount_keeper.get()`, `store.get()` (a
`shared_ptr<Pool>::get`, four sites in `CasPool.cpp`), `base.get()` (two `CasRefCowMap`/`CasRefCowManifestSet`
identity helpers), `ctx.get()` (three OpenSSL `EVP_MD_CTX` sites in `CasBlobHashingWriteBuffer.cpp`), and
`disk->getMetadataStorage().get()`. For the five controller entry points, a hit counts only if its receiver is
`ref_request_controller` or `controller`, or if it is a genuine same-class self-call (verified by reading the
line); the six *definitions* inside `Backend/CasRequestControl.cpp` (`ClassName::method(`) and all declarations in
`Backend/CasRequestControl.h` are excluded as declarations, not calls. `forEachListedKey` is a free function and
is counted with no receiver filter.

22 of the 129 files have at least one hit. "notes" gives the verb from the spec's "Where each verb goes" table
where the enclosing function (found by walking backward from each hit line to the nearest top-level,
non-indented, non-`;`-terminated Allman-style function signature — verified by spot-checking against the actual
file content) names a site the spec already classifies; everything else is marked "classify at migration".

| file | counts | line numbers | notes |
|---|---|---|---|
| `Backend/CasProbe.cpp` | `casPut` 4, `deleteExact` 4, `get` 8, `head` 2, `list` 2, `putIfAbsent` 2, `putOverwrite` 2 | `casPut`: 133,142,151,166; `deleteExact`: 38,180,213,246; `get`: 65,78,106,123,159,171,185,226; `head`: 36,244; `list`: 194,231; `putIfAbsent`: 58,73; `putOverwrite`: 101,115 | runCapabilityProbe: standard on every call; delete-marker refusal keeps operator message |
| `Backend/CasRequestControl.cpp` | `get` 4, `putIfAbsent` 3, `putOverwrite` 1, `resolveByExactGet` 1 | `get`: 320,667,795,895; `putIfAbsent`: 390,767,854; `putOverwrite`: 611; `resolveByExactGet`: 422 | putIfAbsentControlled: old controller internals, not a migration site; putIfAbsentControlledMutable: old controller internals, not a migration site; putOverwriteControlledImpl: old controller internals, not a migration site; resolveByExactGet: old controller internals, not a migration site; slotOccupy: old controller internals, not a migration site |
| `Backend/CasSentinelProbe.cpp` | `get` 1, `list` 1, `probeSentinelRaw` 1 | `get`: 91; `list`: 50; `probeSentinelRaw`: 11 | probePoolBootstrapResidual: forEachListedKey with early stop on first page, then one read, standard; probeSentinel: classify at migration |
| `ContentAddressedTransaction.cpp` | `getStream` 1 | `getStream`: 292 | uploadPendingBlobs: classify at migration |
| `Gc/CasBlobInDegree.cpp` | `get` 1, `getStream` 1, `putIfAbsent` 1 | `get`: 339; `getStream`: 281; `putIfAbsent`: 337 | openSourceEdgeRun: classify at migration; putDeterministicArtifact: create, standard; compare-adopt on Conflict's Object |
| `Gc/CasGc.cpp` | `casPut` 6, `deleteExact` 6, `forEachListedKey` 6, `get` 14, `head` 7, `list` 1, `putIfAbsent` 1 | `casPut`: 944,4246,4373,4397,4418,4472; `deleteExact`: 671,1061,1111,3330,3434,3438; `forEachListedKey`: 1347,1470,3654,3918,3955,4151; `get`: 330,845,1183,1294,2306,3304,3553,3739,4280,4363,4382,4430,4481,4515; `head`: 686,1689,1737,3280,3436,3800,4306; `list`: 3423; `putIfAbsent`: 843 | acquireOrRenewLease: readModifyWrite, standard; cleanupRefObjects: classify at migration; deletePrefixWholesale: classify at migration; drainCompletedRemoving: classify at migration; enumerateRefPrefix: classify at migration; fold: classify at migration; foldManifestEdges: classify at migration; newestFoldSealRef: classify at migration; previewDeletes: classify at migration; probeGenerationForSeal: classify at migration; pulseHeartbeat: read, standard then replace/create, once; readCheckpointWitnesses: classify at migration; readFoldSeal: classify at migration; rebuildBaseline: GC rebuild commit: replace, standard (no re-decide) at casPut; other calls classify at migration; runNamespaceJanitorPage: classify at migration; runRegularRound: GC round commit: replace, standard (no re-decide) at casPut; other calls classify at migration |
| `Gc/CasGcMaintenanceState.cpp` | `casPut` 1, `get` 1 | `casPut`: 34; `get`: 14 | casGcMaintenanceState: create on absence else replace, standard (catch-path write is once); readGcMaintenanceState: read (paired with casGcMaintenanceState) |
| `Gc/CasNamespaceJanitor.cpp` | `deleteExact` 1, `head` 1, `list` 1 | `deleteExact`: 111; `head`: 91; `list`: 25 | runOnePage: classify at migration |
| `Gc/CasOrphanManifestSweep.cpp` | `deleteExact` 1, `forEachListedKey` 1, `get` 6, `head` 1, `list` 1 | `deleteExact`: 586; `forEachListedKey`: 557; `get`: 58,97,101,255,304,642; `head`: 583; `list`: 616 | activeManifestKeys: classify at migration; floorForNamespace: classify at migration; planManifestCursorPage: classify at migration; readAdoptedFoldSeal: classify at migration; sweepNamespace: classify at migration |
| `Pool/CasBlobMeta.cpp` | `deleteExact` 1, `get` 1 | `deleteExact`: 43; `get`: 19 | deleteMetaExact: classify at migration; loadMeta: classify at migration |
| `Pool/CasManifestReader.cpp` | `get` 1 | `get`: 55 | readManifestShared: classify at migration |
| `Pool/CasPartWriteTxn.cpp` | `deleteExact` 1, `get` 1, `head` 2, `publishBlob` 1 | `deleteExact`: 1139; `get`: 737; `head`: 331,1137; `publishBlob`: 417 | cleanupStagedManifestDebrisBestEffort: classify at migration; ensureBlobPresent: publication loop: head/publish/create, shared standard, dependency-proof behind op.admitted(); promote: classify at migration |
| `Pool/CasPlainObjects.cpp` | `deleteExact` 1, `get` 1, `head` 3, `list` 1, `putIfAbsent` 1, `putOverwrite` 1 | `deleteExact`: 83; `get`: 61; `head`: 42,79,146; `list`: 108; `putIfAbsent`: 46; `putOverwrite`: 51 | casGetObject: read (CasPlainObjects data-plane read); casPutObject: readModifyWriteOnPresence, standard (HEAD, never a body); casRemoveObject: removeCurrent, standard; listNamespaceFiles: classify at migration; mountpointObjectExists: classify at migration |
| `Pool/CasPool.cpp` | `get` 4, `list` 1 | `get`: 256,847,1640,1687; `list`: 1767 | currentGcRound: classify at migration; listMirroredChildren: classify at migration; openForDecommission: classify at migration; refreshAdmittedAlgos: classify at migration; reportImpossibleInterference: classify at migration |
| `Pool/CasPoolMeta.cpp` | `casPut` 2, `get` 3 | `casPut`: 92,155; `get`: 96,124,160 | admitOrValidate: readModifyWrite, standard; createOrValidate: read, standard; create on absence; conflict as admitOrValidate |
| `Pool/CasRefCatalog.cpp` | `casPut` 2, `get` 2, `putIfAbsent` 1 | `casPut`: 127,414; `get`: 27,63; `putIfAbsent`: 56 | casUpdateImpl: readModifyWrite, standard; deleteCompletedRemovingAtSnapshot: hand-written read+replace+post-write read, standard (deleteCompletedRemoving family); initializeEmptyForNewPool: classify at migration; readOptionalForBootstrap: classify at migration |
| `Pool/CasRefCkpt.cpp` | `casPut` 1, `get` 1 | `casPut`: 306; `get`: 189 | publishCkpt: readModifyWrite, standard; readCkpt: classify at migration |
| `Pool/CasRefLedger.cpp` | `get` 5, `putIfAbsentControlled` 3, `putIfAbsentControlledMutable` 1, `putOverwriteControlled` 1, `slotOccupy` 2 | `get`: 998,1041,1123,3784,5202; `putIfAbsentControlled`: 266,3748,4550; `putIfAbsentControlledMutable`: 278; `putOverwriteControlled`: 273; `slotOccupy`: 1195,2354 | commitRefChunk: ref lane: create, standard, on resume(g, liveness) handle; dropNamespaceImpl: classify at migration; resolveWedgeOnce: ref lane: create, standard, on resume(g, liveness) handle; runRecoveryWalkOnce: ref lane recovery walk: create, standard, on resume(g, liveness) handle; stagingConditionalOverwrite: classify at migration; stagingPutIfAbsent: classify at migration; stagingPutIfAbsentMutable: classify at migration; tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntimeImpl: classify at migration |
| `Pool/CasRefProtocol.cpp` | `get` 5 | `get`: 885,974,1012,1035,1080 | crossEpochFromSeal: classify at migration; readCheckpointSnapshotBase: classify at migration; recoverRefTableDetailedFromAuthority: classify at migration |
| `Pool/CasServerRoot.cpp` | `casPut` 1, `get` 11, `head` 1, `list` 4, `probeSentinelRaw` 1, `putIfAbsent` 3, `putOverwrite` 5, `putOverwriteControlled` 1 | `casPut`: 803; `get`: 509,722,909,1083,1164,1266,1316,1381,1476,1516,1838; `head`: 1462; `list`: 63,1141,1256,1310; `probeSentinelRaw`: 745; `putIfAbsent`: 682,915,1465; `putOverwrite`: 950,982,1215,1513,1835; `putOverwriteControlled`: 1735 | allocateWriterEpoch: readModifyWrite, standard; claim: MountLeaseKeeper::claim: read, then create/replace, standard; conflict terminal, adopt path 3->2 requests; claimMount: read, then create/replace, standard; conflict terminal; claimMountAwaitingExpiry: read, then create/replace, standard; conflict terminal; claimOwnerOrThrow: read, then create/replace, standard; conflict terminal; computeHeartbeatFloor: readModifyWrite, standard (fence-out); isCreatorFenceTerminal: classify at migration; listMounts: classify at migration; prefixHasAnyKey: classify at migration; probeNonTerminalMountSlots: classify at migration; readOwnerObject: classify at migration; renew: MountLeaseKeeper::renew: replace, untilLeaseSafe; terminate: MountLeaseKeeper::terminate (farewell): replace, within(10s), on operation admitted against open fence |
| `Tools/CasDecommission.cpp` | `deleteExact` 2, `forEachListedKey` 2, `get` 5, `head` 2, `putOverwriteControlled` 1 | `deleteExact`: 68,92; `forEachListedKey`: 50,222; `get`: 296,318,380,390,408; `head`: 59,179; `putOverwriteControlled`: 422 | decommissionPoolMember: classify at migration; deleteListedPrefix: classify at migration; deleteSlotObject: classify at migration |
| `Tools/CasFsck.cpp` | `forEachListedKey` 2, `get` 5, `head` 4 | `forEachListedKey`: 60,528; `get`: 378,643,817,833,922; `head`: 654,754,958,1056 | checkRefStream: classify at migration; listAll: classify at migration; runFsckImpl: classify at migration |

## 2. Functions declared over `Backend &` {#functions-declared-over-backend}

Scope: same 129 files. Command:
```
rg -n --type-add 'ch:*.{cpp,h}' -t ch '\bBackend\s*&|\bBackend\s*\*|\bBackendPtr\b' \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed \
  | grep -Ev '/Backend/CasBackend\.h:|/Backend/CasObjectStorageBackend\.(h|cpp):|/Backend/CasInMemoryBackend\.(h|cpp):|/Backend/CasEmulatedSingleProcess|/Backend/CasInstrumentedBackend\.(h|cpp):'
```
(161 raw hits). Excluded as not "declared over": local aliases (`Backend & backend = store->backend();`, 19
sites, all `Backend & backend = store[.->]backend();` in `CasGc.cpp`/`CasOrphanManifestSweep.cpp`/`CasFsck.cpp`),
class field declarations (`Backend & backend;` / `BackendPtr backend;` / `BackendPtr pool_backend;` /
`BackendPtr inner;`, 9 sites), the `using BackendPtr = std::shared_ptr<Backend>;` typedef, and 4 comment lines.
127 lines kept; one (`Backend/CasInstrumentedBackend.h:85`, `InstrumentedBackend`'s constructor) is itself inside
an excluded backend-implementation file and is dropped, leaving 126 lines naming 123 distinct functions/
constructors/methods (grouped below by function; a function whose signature spans multiple lines gets one row
listing every line it appears on). `Backend/CasBackend.h:378` (`forEachListedKey`'s own declaration) is kept as a
special case: it is the interface's own free function, not a caller, but it is exactly what steps 3/5 of the
migration touch, so it is listed rather than silently dropped.

Multi-line signatures without a name on the matched line were resolved by reading the nearest preceding
`identifier(` line in the same file (or, where that failed for 19 lines — all constructors or continuation lines
whose name is 2+ lines above the match — by reading the surrounding class declaration directly).

| file | function | lines |
|---|---|---|
| `Backend/CasBackend.h` | `forEachListedKey` | 378 |
| `Backend/CasProbe.cpp` | `runCapabilityProbe` | 15 |
| `Backend/CasProbe.h` | `runCapabilityProbe` | 31 |
| `Backend/CasRequestControl.cpp` | `CasRequestController::CasRequestController (ctor)` | 258 |
| `Backend/CasRequestControl.h` | `CasRequestController (ctor)` | 452 |
| `Backend/CasSentinelProbe.cpp` | `probePoolBootstrapResidual` | 32 |
| `Backend/CasSentinelProbe.cpp` | `probeSentinel` | 9 |
| `Backend/CasSentinelProbe.h` | `probePoolBootstrapResidual` | 53 |
| `Backend/CasSentinelProbe.h` | `probeSentinel` | 16 |
| `Formats/CasPoolMetaFormat.h` | `createOrValidate` | 53,60 |
| `Gc/CasBlobInDegree.cpp` | `PriorEdgeCursor` | 56 |
| `Gc/CasBlobInDegree.cpp` | `foldDeltasIntoGeneration` | 348 |
| `Gc/CasBlobInDegree.cpp` | `openSourceEdgeRun` | 277 |
| `Gc/CasBlobInDegree.cpp` | `putDeterministicArtifact` | 335 |
| `Gc/CasBlobInDegree.cpp` | `zeroInDegree` | 689 |
| `Gc/CasBlobInDegree.h` | `foldDeltasIntoGeneration` | 384 |
| `Gc/CasBlobInDegree.h` | `openSourceEdgeRun` | 115,127 |
| `Gc/CasBlobInDegree.h` | `putDeterministicArtifact` | 137 |
| `Gc/CasBlobInDegree.h` | `zeroInDegree` | 411 |
| `Gc/CasGc.cpp` | `deletePrefixWholesale` | 84,3413 |
| `Gc/CasGcMaintenanceState.cpp` | `casGcMaintenanceState` | 32 |
| `Gc/CasGcMaintenanceState.cpp` | `readGcMaintenanceState` | 12 |
| `Gc/CasGcMaintenanceState.h` | `casGcMaintenanceState` | 27 |
| `Gc/CasGcMaintenanceState.h` | `readGcMaintenanceState` | 25 |
| `Gc/CasGcMetaWriter.cpp` | `deleteConfirmedMeta` | 76 |
| `Gc/CasGcShardPlan.cpp` | `reduce` | 43 |
| `Gc/CasGcShardPlan.h` | `reduce` | 103 |
| `Gc/CasNamespaceJanitor.h` | `NamespaceJanitor` | 23 |
| `Gc/CatalogLifecycleReconciler.cpp` | `CatalogLifecycleReconciler` | 21 |
| `Gc/CatalogLifecycleReconciler.h` | `CatalogLifecycleReconciler` | 46 |
| `Pool/CasBlobMeta.cpp` | `deleteMetaExact` | 39 |
| `Pool/CasBlobMeta.cpp` | `loadMeta` | 16 |
| `Pool/CasBlobMeta.h` | `deleteMetaExact` | 63 |
| `Pool/CasBlobMeta.h` | `loadMeta` | 38 |
| `Pool/CasManifestReader.cpp` | `CasManifestReader` | 33 |
| `Pool/CasManifestReader.h` | `CasManifestReader` | 41 |
| `Pool/CasMountRuntime.cpp` | `CasMountRuntime::CasMountRuntime (ctor)` | 55 |
| `Pool/CasMountRuntime.h` | `CasMountRuntime (ctor)` | 138 |
| `Pool/CasPlainObjects.h` | `CasPlainObjects` | 36 |
| `Pool/CasPool.cpp` | `Pool::Pool (ctor)` | 181 |
| `Pool/CasPool.cpp` | `Pool::open` | 368 |
| `Pool/CasPool.cpp` | `Pool::openForDecommission` | 818 |
| `Pool/CasPool.cpp` | `probePoolLifecycleGate` | 131 |
| `Pool/CasPool.h` | `Pool (ctor)` | 829 |
| `Pool/CasPool.h` | `Pool::backend` | 711 |
| `Pool/CasPool.h` | `Pool::open` | 393 |
| `Pool/CasPool.h` | `Pool::openForDecommission` | 401 |
| `Pool/CasPool.h` | `Pool::poolBackendPtr` | 716 |
| `Pool/CasPoolMeta.cpp` | `admitOrValidate` | 76 |
| `Pool/CasPoolMeta.cpp` | `createOrValidate` | 109 |
| `Pool/CasRefCatalog.cpp` | `beginRemoving` | 290 |
| `Pool/CasRefCatalog.cpp` | `cancelStalledCreating` | 458 |
| `Pool/CasRefCatalog.cpp` | `casAdmitEntry` | 260 |
| `Pool/CasRefCatalog.cpp` | `casUpdate` | 235 |
| `Pool/CasRefCatalog.cpp` | `casUpdateImpl` | 115 |
| `Pool/CasRefCatalog.cpp` | `completeCreation` | 495 |
| `Pool/CasRefCatalog.cpp` | `createNamespace` | 544 |
| `Pool/CasRefCatalog.cpp` | `createNamespaceStep1` | 201 |
| `Pool/CasRefCatalog.cpp` | `deleteCompletedRemoving` | 337 |
| `Pool/CasRefCatalog.cpp` | `deleteCompletedRemovingAtSnapshot` | 362 |
| `Pool/CasRefCatalog.cpp` | `initializeEmptyForNewPool` | 52 |
| `Pool/CasRefCatalog.cpp` | `lifeIfCataloged` | 77 |
| `Pool/CasRefCatalog.cpp` | `liveUniverse` | 86 |
| `Pool/CasRefCatalog.cpp` | `read` | 41 |
| `Pool/CasRefCatalog.cpp` | `readOptionalForBootstrap` | 25 |
| `Pool/CasRefCatalog.cpp` | `reconcileStaleCreator` | 605 |
| `Pool/CasRefCatalog.h` | `beginRemoving` | 126 |
| `Pool/CasRefCatalog.h` | `cancelStalledCreating` | 192 |
| `Pool/CasRefCatalog.h` | `casAdmitEntry` | 111 |
| `Pool/CasRefCatalog.h` | `casUpdate` | 98 |
| `Pool/CasRefCatalog.h` | `completeCreation` | 291 |
| `Pool/CasRefCatalog.h` | `createNamespace` | 253 |
| `Pool/CasRefCatalog.h` | `deleteCompletedRemoving` | 167 |
| `Pool/CasRefCatalog.h` | `deleteCompletedRemovingAtSnapshot` | 175 |
| `Pool/CasRefCatalog.h` | `initializeEmptyForNewPool` | 41 |
| `Pool/CasRefCatalog.h` | `lifeIfCataloged` | 55 |
| `Pool/CasRefCatalog.h` | `liveUniverse` | 61 |
| `Pool/CasRefCatalog.h` | `read` | 35 |
| `Pool/CasRefCatalog.h` | `reconcileStaleCreator` | 329 |
| `Pool/CasRefCkpt.cpp` | `publishCkpt` | 197 |
| `Pool/CasRefCkpt.cpp` | `readCkpt` | 187 |
| `Pool/CasRefCkpt.h` | `publishCkpt` | 121 |
| `Pool/CasRefCkpt.h` | `readCkpt` | 138 |
| `Pool/CasRefLedger.cpp` | `CasRefLedger::CasRefLedger (ctor)` | 201 |
| `Pool/CasRefLedger.h` | `CasRefLedger (ctor)` | 92 |
| `Pool/CasRefProtocol.cpp` | `crossEpochFromSeal` | 857 |
| `Pool/CasRefProtocol.cpp` | `readCheckpointSnapshotBase` | 955 |
| `Pool/CasRefProtocol.cpp` | `recoverRefTableDetailedFromAuthority` | 1050 |
| `Pool/CasRefProtocol.h` | `crossEpochFromSeal` | 739 |
| `Pool/CasRefProtocol.h` | `readCheckpointSnapshotBase` | 782 |
| `Pool/CasRefProtocol.h` | `recoverRefTableDetailedFromAuthority` | 798 |
| `Pool/CasServerRoot.cpp` | `MountLeaseKeeper::MountLeaseKeeper (ctor)` | 1420 |
| `Pool/CasServerRoot.cpp` | `allocateWriterEpoch` | 712 |
| `Pool/CasServerRoot.cpp` | `claimMount` | 904 |
| `Pool/CasServerRoot.cpp` | `claimMountAwaitingExpiry` | 1046 |
| `Pool/CasServerRoot.cpp` | `claimOwnerOrThrow` | 645 |
| `Pool/CasServerRoot.cpp` | `computeHeartbeatFloor` | 1122 |
| `Pool/CasServerRoot.cpp` | `isCreatorFenceTerminal` | 1378 |
| `Pool/CasServerRoot.cpp` | `listMounts` | 1303 |
| `Pool/CasServerRoot.cpp` | `prefixHasAnyKey` | 61 |
| `Pool/CasServerRoot.cpp` | `probeNonTerminalMountSlots` | 1245 |
| `Pool/CasServerRoot.cpp` | `readOwnerObject` | 507 |
| `Pool/CasServerRoot.cpp` | `readOwnerUuid` | 636 |
| `Pool/CasServerRoot.cpp` | `serverRootSubtreeEmpty` | 620 |
| `Pool/CasServerRoot.h` | `MountLeaseKeeper (ctor)` | 488 |
| `Pool/CasServerRoot.h` | `allocateWriterEpoch` | 174 |
| `Pool/CasServerRoot.h` | `claimMount` | 259 |
| `Pool/CasServerRoot.h` | `claimMountAwaitingExpiry` | 299 |
| `Pool/CasServerRoot.h` | `claimOwnerOrThrow` | 145 |
| `Pool/CasServerRoot.h` | `computeHeartbeatFloor` | 369 |
| `Pool/CasServerRoot.h` | `isCreatorFenceTerminal` | 468 |
| `Pool/CasServerRoot.h` | `listMounts` | 412 |
| `Pool/CasServerRoot.h` | `probeNonTerminalMountSlots` | 397 |
| `Pool/CasServerRoot.h` | `readOwnerUuid` | 134 |
| `Pool/CasServerRoot.h` | `serverRootSubtreeEmpty` | 124 |
| `Tools/CasDecommission.cpp` | `decommissionPoolMember` | 110 |
| `Tools/CasDecommission.cpp` | `deleteListedPrefix` | 47 |
| `Tools/CasDecommission.cpp` | `deleteSlotObject` | 88 |
| `Tools/CasDecommission.h` | `decommissionPoolMember` | 51 |
| `Tools/CasFsck.cpp` | `checkRefStream` | 311 |
| `Tools/CasFsck.cpp` | `listAll` | 54 |
| `Tools/CasFsck.cpp` | `manifestStillReferenced` | 220 |
| `Tools/CasFsck.cpp` | `recoverLateRefTable` | 127 |

## 3. Token-carrying declarations {#token-carrying-declarations}

Scope: the same 129 files plus `Formats/` (already included in that set). Command:
```
rg -n --type-add 'ch:*.{cpp,h}' -t ch '\bToken\b' \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed \
  | grep -Ev '/Backend/CasBackend\.h:|/Backend/CasObjectStorageBackend\.(h|cpp):|/Backend/CasInMemoryBackend\.(h|cpp):|/Backend/CasEmulatedSingleProcess|/Backend/CasInstrumentedBackend\.(h|cpp):'
```
(110 raw hits after backend-implementation exclusion; 172 before it — the naive `grep -Ev … $` anchor does not
match inside `file:line:content` lines, so the exclusion was re-applied against `file:line:` instead). A second
Python pass kept only lines matching a type-position use of `Token` — a field (`Token token;`), a parameter
(`Token & x`, `Token * x`, `const Token & x`), a return type (`Token method(...)`, `-> Token`),
`std::optional<Token>`, `std::vector<Token>`, `std::map<K, Token>`, or `struct Token` itself — and dropped
comments and value-position uses (`Token{...}` construction, `rec.token = …`, `return Token{...}`). 101 lines
kept across 32 files.

| file | count | lines |
|---|---|---|
| `Backend/CasProbe.cpp` | 6 | 56,100,113,131,150,179 |
| `Backend/CasRequestControl.cpp` | 6 | 314,347,386,465,489,498 |
| `Backend/CasRequestControl.h` | 7 | 393,431,492,504,524,534,610 |
| `Formats/CasGcOutcomesFormat.h` | 1 | 43 |
| `Formats/CasRecordStreamFormat.h` | 1 | 88 |
| `Formats/CasWireVocab.cpp` | 2 | 48,110 |
| `Formats/CasWireVocab.h` | 2 | 55,156 |
| `Gc/CasBlobInDegree.h` | 3 | 29,77,202 |
| `Gc/CasGc.cpp` | 9 | 358,1054,1055,1171,2398,3878,4029,4365,4376 |
| `Gc/CasGc.h` | 5 | 456,515,540,700,789 |
| `Gc/CasGcMaintenanceState.cpp` | 1 | 32 |
| `Gc/CasGcMaintenanceState.h` | 3 | 15,22,27 |
| `Gc/CasGcMetaWriter.cpp` | 7 | 137,190,196,202,208,213,218 |
| `Gc/CasGcMetaWriter.h` | 7 | 36,57,58,59,72,73,74 |
| `Gc/CasNamespaceJanitor.cpp` | 1 | 86 |
| `Gc/CasOrphanManifestSweep.h` | 1 | 140 |
| `Pool/CasBlobMeta.cpp` | 2 | 32,39 |
| `Pool/CasBlobMeta.h` | 3 | 23,57,63 |
| `Pool/CasPartWriteTxn.cpp` | 1 | 581 |
| `Pool/CasPool.cpp` | 2 | 1901,1906 |
| `Pool/CasPool.h` | 2 | 722,724 |
| `Pool/CasPoolMeta.cpp` | 1 | 76 |
| `Pool/CasRefCatalog.h` | 1 | 29 |
| `Pool/CasRefCkpt.cpp` | 3 | 297,298,362 |
| `Pool/CasRefCkpt.h` | 2 | 133,164 |
| `Pool/CasRefLedger.cpp` | 3 | 262,269,886 |
| `Pool/CasRefLedger.h` | 2 | 294,298 |
| `Pool/CasServerRoot.cpp` | 7 | 725,905,1064,1080,1460,1548,1695 |
| `Pool/CasServerRoot.h` | 5 | 238,260,314,508,532 |
| `Primitives/CasTypes.h` | 2 | 259,265 |
| `Tools/CasDecommission.cpp` | 2 | 54,88 |
| `Tools/CasInspect.cpp` | 1 | 290 |
| **total** | **101** | |

**Connected components.** Grouping method: transitive closure over three kinds of edges, each verified by a
targeted `grep`/`rg` (not assumed) — (a) a header declaration and its `.cpp` definition of the same qualified
name are the same node's two files; (b) file A is connected to file B when A calls a function B declares whose
signature carries `Token` (verified: the call site was grepped, not inferred from the declaration alone); (c) file
A is connected to file B when A reads a field of a `Token`-carrying struct B declares. `struct Token` itself
(`Primitives/CasTypes.h`) is treated as foundational infrastructure every file includes, not as an edge — using it
as an edge would trivially weld all 32 files into one component and say nothing useful.

Verified edges (representative, not exhaustive): `Gc/CasGc.cpp:1595` reads `catalog_snapshot.token` —
`Pool/CasRefCatalog.h:29`'s `Snapshot::token`. `Formats/CasGcOutcomesFormat.cpp` and
`Formats/CasRecordStreamFormat.cpp` both call `Formats/CasWireVocab.{h,cpp}`'s `writeTokenFields`/`TokenFields`.
`Pool/CasPartWriteTxn.cpp:582` calls `store->stagingPutIfAbsent(key, encoded, &manifest_token)`, declared in both
`Pool/CasPool.h:722` and `Pool/CasRefLedger.h:294` (`Pool::stagingPutIfAbsent` delegates to
`CasRefLedger::stagingPutIfAbsent`). `Pool/CasBlobMeta.cpp:36` calls `pool.stagingConditionalOverwrite(...)`,
same delegation pair. `deleteMetaExact`/`loadMeta`/`casMeta` (`Pool/CasBlobMeta.{h,cpp}`) are called from both
`Pool/CasPartWriteTxn.cpp` and `Gc/CasGcMetaWriter.cpp`/`Gc/CasGc.cpp`. `Tools/CasInspect.cpp` `#include`s
`Pool/CasBlobMeta.h`, `Pool/CasServerRoot.h`, and `Formats/CasWireVocab.h` directly. `putIfAbsentControlled` /
`putOverwriteControlled` / `putIfAbsentControlledMutable` / `slotOccupy` (`Backend/CasRequestControl.{h,cpp}`) are
called from `Pool/CasRefLedger.cpp`, `Pool/CasServerRoot.cpp`, and `Tools/CasDecommission.cpp` (section 1).
`publishCkpt` (`Pool/CasRefCkpt.{h,cpp}`) is called from both `Pool/CasRefCatalog.cpp` and
`Pool/CasRefLedger.cpp`.

Following that closure, **30 of the 32 files form one connected component** — named here `CasRequestControl`
after its busiest hub (`Backend/CasRequestControl.h`, 7 declarations, and the 5 controller entry points every
migrated write eventually funnels through): `Backend/CasRequestControl.{h,cpp}`, `Backend/CasProbe.cpp` (only via
its local `Token` variables against the raw `Backend` interface — see below), `Formats/CasGcOutcomesFormat.h`,
`Formats/CasRecordStreamFormat.h`, `Formats/CasWireVocab.{h,cpp}`, `Gc/CasBlobInDegree.h`, `Gc/CasGc.{h,cpp}`,
`Gc/CasGcMaintenanceState.{h,cpp}`, `Gc/CasGcMetaWriter.{h,cpp}`, `Gc/CasNamespaceJanitor.cpp`,
`Gc/CasOrphanManifestSweep.h`, `Pool/CasBlobMeta.{h,cpp}`, `Pool/CasPartWriteTxn.cpp`, `Pool/CasPool.{h,cpp}`,
`Pool/CasPoolMeta.cpp`, `Pool/CasRefCatalog.h`, `Pool/CasRefCkpt.{h,cpp}`, `Pool/CasRefLedger.{h,cpp}`,
`Pool/CasServerRoot.{h,cpp}`, `Primitives/CasTypes.h` (foundational, included everywhere), `Tools/CasDecommission.cpp`,
`Tools/CasInspect.cpp`.

**`Backend/CasProbe.cpp` is its own singleton component**: its 6 kept lines are all locally-scoped `Token`
variables (`t1`, `t2`, `ct1`, `wrong_token`, `stale`) constructed inside `runCapabilityProbe` and passed straight
into raw `backend.putOverwrite(...)`/`backend.casPut(...)`/`backend.deleteExact(...)` calls (section 1) — it
shares no application-level `Token`-carrying struct or named signature with any other file in the tree, only the
universal `Backend` interface itself (which every file shares and is therefore not treated as a distinguishing
edge, per the exclusion above). If a reviewer instead treats the raw `Backend` interface's own `Token`-taking
signatures as a qualifying edge, this file joins the big component too — flagged here as the one methodological
choice that changes the answer.

`benchmarks/benchmark_cas_ref_protocol.cpp` has one `Token`-typed value construction (`rec.token = Token{...}`)
but no type-position declaration of its own, so it does not appear in the table above; it is a *consumer* of
`Formats/CasRecordStreamFormat.h`'s `TokenFields` (confirmed via its `#include`), i.e. it exercises the big
component without adding a node to the census.

## 4. Fence checks {#fence-checks}

Command:
```
rg -n --type-add 'ch:*.{cpp,h}' -t ch \
  'check_fence_or_throw|checkFenceOrThrow|refAppendFenceOk|mayMutate|fenceGeneration|fence_generation_fn|LeaderFenceStatus' \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```
(161 raw hits, no file exclusion — this command was intentionally run over the whole tree including the backend
implementations, none of which matched; all 161 hits are already outside the excluded backend files). A Python
pass kept only genuine call/use sites — `check_fence_or_throw(...)`/`checkFenceOrThrow(...)`/`check_fence(...)`
invocations, `mayMutate()`/`fenceGeneration()`/`refAppendFenceOk()` calls in an expression, and
`LeaderFenceStatus::Moved`/`Held` comparisons — and dropped comments, pure declarations (`bool mayMutate() const;`),
and the `std::function<...> check_fence...` callback-type declarations/field decls/constructor member-init-list
wiring (`, fence_generation_fn(std::move(fence_generation_fn_))`). 59 lines kept.

Of those 59, **13 are not call *sites* in the sense this table wants** and are footnoted separately below rather
than force-classified: `Pool/CasMountRuntime.cpp:126` is `checkFenceOrThrow`'s own implementation body (not a
caller); `Pool/CasMountRuntime.cpp:135` is `refAppendFenceOk`'s definition line; `Pool/CasPool.cpp:201,202,220,
221,222,224` are constructor-injection wiring that binds `mount_runtime`'s primitives into the callbacks passed to
`CasRefLedger`/`CasRefCatalog`; `Pool/CasPool.cpp:288,296,298` are `Pool::mayMutate`/`Pool::refAppendFenceOk`'s own
delegate-definition bodies; `Gc/CasGc.cpp:4522,4523` are the two `return` arms of the `check_fence` callback body
`Gc` wires into `CasRefCatalog`, not a call to it.

The remaining 46 are classified "pre-request" (the check immediately precedes an outbound `CasRefCatalog::read`/
`store->…` request within the next line or two, verified by reading the surrounding 5 lines) or "verdict point"
(the check gates a `throwCasWriteRetryLater`, an early return, a post-read re-validation before trusting an
already-fetched observation — the recurring "check → read → check" trio documented in
`Pool/CasRefCkpt.h:93`'s "FENCE DISCIPLINE" comment — or a local-state transition under a lock), each judged from
its surrounding 5 lines (shown in the "context" column). Two (`Pool/CasPartWriteTxn.cpp:265`,
`ContentAddressedTransaction.cpp:912`) are a third kind, "admission capture": a bare `fenceGeneration()` read that
captures `admitted_generation` at transaction/txn admission, neither itself a request nor a guard.

| file | line | classification | context |
|---|---|---|---|
| `Pool/CasRefCkpt.cpp` | 250 | verdict point | try/catch around check inside publishCkpt's mutate-retry; catch classifies fence-loss |
| `Pool/CasRefCkpt.cpp` | 273 | verdict point | same pattern, second call site inside publishCkpt |
| `Pool/CasRefCkpt.cpp` | 287 | verdict point | same pattern, outer publishCkpt guard |
| `Pool/CasRefCkpt.cpp` | 331 | verdict point | same pattern, readCkpt-adjacent guard |
| `Pool/CasRefCatalog.cpp` | 301 | verdict point | casUpdateImpl's mutate lambda: try/catch converts fence loss to CatalogFenceMovedMarker |
| `Pool/CasRefCatalog.cpp` | 404 | verdict point | deleteCompletedRemovingAtSnapshot: check_fence(...) == Moved decides FencedOut outcome |
| `Pool/CasRefCatalog.cpp` | 429 | verdict point | same function, second fence probe before FencedOut return |
| `Pool/CasRefCatalog.cpp` | 470 | verdict point | casAdmitEntry's mutate lambda, same try/catch pattern |
| `Pool/CasRefCatalog.cpp` | 521 | verdict point | completeCreation's mutate lambda, same pattern |
| `Pool/CasRefCatalog.cpp` | 621 | verdict point | reconcileStaleCreator's mutate lambda, same pattern |
| `Pool/CasPartWriteTxn.cpp` | 265 | admission capture | fenceGeneration() read captures admitted_generation at txn admission, not a guard itself |
| `Pool/CasPartWriteTxn.cpp` | 362 | verdict point | post mandatory-HEAD gate before trusting the observed state |
| `Pool/CasPartWriteTxn.cpp` | 371 | verdict point | guards proceeding after body-put-avoided decision, before event emit |
| `Pool/CasPartWriteTxn.cpp` | 383 | verdict point | guards the early BlobUploadResult return |
| `Pool/CasPartWriteTxn.cpp` | 392 | pre-request | gates entry to beginPublication() / the publish attempt |
| `Pool/CasPartWriteTxn.cpp` | 445 | pre-request | gates reconcileMetaClean (readModifyWrite site) |
| `Pool/CasPartWriteTxn.cpp` | 447 | verdict point | post reconcileMetaClean, guards the subsequent event emit |
| `Pool/CasPartWriteTxn.cpp` | 464 | verdict point | guards the terminal BlobUploadResult return |
| `Gc/CatalogLifecycleReconciler.cpp` | 80 | verdict point | check_fence(...) == Moved decides AuthorityStatus::FencedOut |
| `ContentAddressedTransaction.cpp` | 912 | admission capture | fenceGeneration() read captures admitted_generation before constructing the write buffer |
| `ContentAddressedTransaction.cpp` | 926 | pre-request | checkFenceOrThrow wired into a deferred finalize callback, gates the eventual publish |
| `Pool/CasRefLedger.cpp` | 584 | verdict point | function-entry gate in acquireRefTableRuntime, before any read |
| `Pool/CasRefLedger.cpp` | 618 | verdict point | guards a throwCasWriteRetryLater on identity conflict |
| `Pool/CasRefLedger.cpp` | 637 | verdict point | guards proceeding into removal-admission local state |
| `Pool/CasRefLedger.cpp` | 652 | pre-request | precedes CasRefCatalog::read (first_catalog) |
| `Pool/CasRefLedger.cpp` | 654 | verdict point | post-read re-check before trusting first_catalog (fence trio pattern) |
| `Pool/CasRefLedger.cpp` | 674 | verdict point | post second_catalog read, ambiguity-window guard |
| `Pool/CasRefLedger.cpp` | 926 | verdict point | guards a throwCasWriteRetryLater on catalog-life invalidation |
| `Pool/CasRefLedger.cpp` | 1494 | verdict point | guards preserving the retained recovery attempt |
| `Pool/CasRefLedger.cpp` | 1576 | verdict point | guards a throwCasWriteRetryLater during recovery walk |
| `Pool/CasRefLedger.cpp` | 2407 | verdict point | inside check_wedge_admitted decide callback, guards invalidated/superseded throw |
| `Pool/CasRefLedger.cpp` | 2478 | verdict point | try/catch admission check in resolveWedgeOnce |
| `Pool/CasRefLedger.cpp` | 3414 | verdict point | post CasRefCatalog::read, guards throwIfAmbiguous |
| `Pool/CasRefLedger.cpp` | 3444 | verdict point | post CasRefCatalog::read, guards entry lookup |
| `Pool/CasRefLedger.cpp` | 3883 | verdict point | inside check_commit_admitted decide callback, guards invalidated/superseded throw |
| `Pool/CasRefLedger.cpp` | 4587 | verdict point | guards invalidated/superseded throw before commit |
| `Pool/CasRefLedger.cpp` | 4927 | verdict point | guards proceeding into removal-close local state |
| `Pool/CasRefLedger.cpp` | 4954 | pre-request | precedes CasRefCatalog::read (catalog) |
| `Pool/CasRefLedger.cpp` | 4956 | verdict point | post-read re-check before trusting catalog (fence trio pattern) |
| `Pool/CasRefLedger.cpp` | 4968 | verdict point | guards proceeding to acquireRefTableRuntime after life resolution |
| `Pool/CasRefLedger.cpp` | 5000 | verdict point | guards proceeding into removal-close local state |
| `Pool/CasRefLedger.cpp` | 5021 | pre-request | precedes CasRefCatalog::read (first_catalog) |
| `Pool/CasRefLedger.cpp` | 5023 | verdict point | post-read re-check (fence trio pattern) |
| `Pool/CasRefLedger.cpp` | 5044 | verdict point | post second_catalog read, terminal find_entry decision |
| `Pool/CasRefLedger.cpp` | 5078 | pre-request | precedes CasRefCatalog::read (post_terminal_catalog) |
| `Pool/CasRefLedger.cpp` | 5250 | verdict point | guards clearing removal_admission_closed under lock |

Footnoted (excluded from the table above — not call sites):

| file | line | why excluded from the call-site table |
|---|---|---|
| `Pool/CasMountRuntime.cpp` | 126 | primitive's own implementation (checkFenceOrThrow's body), not a call site |
| `Pool/CasMountRuntime.cpp` | 135 | definition line of refAppendFenceOk, not a call |
| `Pool/CasPool.cpp` | 201 | wiring: binds mount_runtime.fenceGeneration into a constructor-injected callback |
| `Pool/CasPool.cpp` | 202 | wiring: binds mount_runtime.checkFenceOrThrow into a constructor-injected callback |
| `Pool/CasPool.cpp` | 220 | wiring: binds refAppendFenceOk into a constructor-injected callback |
| `Pool/CasPool.cpp` | 221 | wiring: binds mount_runtime.fenceGeneration into a constructor-injected callback |
| `Pool/CasPool.cpp` | 222 | wiring: binds mount_runtime.checkFenceOrThrow into a constructor-injected callback |
| `Pool/CasPool.cpp` | 224 | wiring: binds mayMutate into a constructor-injected callback |
| `Pool/CasPool.cpp` | 288 | definition body of Pool::mayMutate (delegates to mount_runtime) |
| `Pool/CasPool.cpp` | 296 | definition line of Pool::refAppendFenceOk |
| `Pool/CasPool.cpp` | 298 | definition body of Pool::refAppendFenceOk (delegates to mount_runtime) |
| `Gc/CasGc.cpp` | 4522 | defines the check_fence callback body Gc wires into CasRefCatalog (Moved arm) |
| `Gc/CasGc.cpp` | 4523 | defines the check_fence callback body Gc wires into CasRefCatalog (Held arm) |

## 5. Test files {#test-files}

Scope: `find src/Disks/tests -iname "gtest_cas*.cpp"` returns 134 matches, of which **one is a false positive of
the naming pattern**: `gtest_cascade_and_memory_write_buffer.cpp` (about a cascading/memory write buffer, not
content-addressed storage; confirmed it has zero `ContentAddressed` includes) — excluded, noted here as the
ambiguity the task asked to flag rather than silently dropped. One more file matches "any other gtest file that
includes a CAS backend header" without matching the `gtest_cas*` glob: `gtest_ca_wiring.cpp`. Final scope: 133 +
1 = **134 files**.

### 5a. Transport call counts {#test-transport-call-counts}

Same per-method regex scan as section 1 (comments stripped, method name followed by `(`), run over the 134 test
files. **Unlike section 1, this table is NOT receiver-filtered file-by-file** — at 134 files a full per-line
receiver audit was out of budget. A representative receiver audit (grouping all 2422 raw hits by the identifier
immediately preceding each call) found the overwhelming majority use plausible `Backend`-typed receivers
(`backend`, `b`, `b1`, `b2`, `inner`, `controller`, `ctrl`, `foreign_backend`, `storage->store()->backend()`,
override-declaration lines with no receiver at all) and turned up a **known, exact list of non-`Backend` false
positives** to exclude when the plan builds exact per-test-file checklists: `future.get()`/`.fut.get()` patterns
(async test helpers), `single_attempt_client->getClientConfiguration()`, `reason_col.type`, `oracle`, `columns`,
`stop`, `running`, `done`, `destroyed`, `buffer`, `delayed_buffer`, `direct`, `seed` (list-only), `cached_manifest`,
`view1`/`view1->manifest()`, `channel`, `drain`, `owned_client`. These are all single-digit counts against a
751-strong `get` total; "classify at migration" applies to disambiguating them from the genuine `Backend` calls
sharing the same file. 85 of 134 files have at least one hit.

Command (per method, over the file list from above):
```
for m in get getStream head putIfAbsent putOverwrite casPut deleteExact publishBlob list probeSentinelRaw \
         putIfAbsentControlled putOverwriteControlled putIfAbsentControlledMutable slotOccupy resolveByExactGet \
         forEachListedKey; do
  rg -n --type-add 'ch:*.cpp' -t ch "\b${m}\s*\(" src/Disks/tests -g 'gtest_cas*.cpp' -g 'gtest_ca_wiring.cpp'
done
```
(comment-stripped in Python, not raw `rg`, to match section 1's methodology exactly).

| file | counts | production headers included |
|---|---|---|
| `gtest_ca_wiring.cpp` | `deleteExact` 2, `get` 14, `head` 12, `putIfAbsent` 1, `putOverwrite` 1 | Backend/CasBackend.h, ContentAddressedExchange.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasFormat.h, Formats/CasLayout.h, Gc/CasBlobInDegree.h, Parts/PartPathParser.h, Pool/CasPartWriteTxn.h, Tools/CasFsck.h |
| `gtest_cas_b140_dangle.cpp` | `head` 2 | Backend/CasInMemoryBackend.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_backend.cpp` | `casPut` 11, `deleteExact` 17, `get` 39, `getStream` 6, `head` 19, `list` 6, `publishBlob` 19, `putIfAbsent` 38, `putOverwrite` 7 | Backend/CasBackend.h, Backend/CasInMemoryBackend.h, Backend/CasInstrumentedBackend.h, Backend/CasObjectStorageBackend.h |
| `gtest_cas_backend_contract.cpp` | `casPut` 5, `deleteExact` 5, `get` 13, `head` 3, `list` 3, `putIfAbsent` 12, `putOverwrite` 3 | Backend/CasBackend.h, Backend/CasInMemoryBackend.h, Backend/CasObjectStorageBackend.h |
| `gtest_cas_backend_generation.cpp` | `casPut` 2, `get` 4, `head` 6, `publishBlob` 2, `putIfAbsent` 2 | Backend/CasObjectStorageBackend.h |
| `gtest_cas_backend_listing.cpp` | `forEachListedKey` 2, `putIfAbsent` 3 | Backend/CasBackend.h, Backend/CasInMemoryBackend.h |
| `gtest_cas_blob_indegree.cpp` | `get` 6, `putIfAbsent` 4 | Backend/CasInMemoryBackend.h, Formats/CasGcStateFormat.h, Formats/CasLayout.h, Formats/CasRecordStreamFormat.h, Gc/CasBlobInDegree.h |
| `gtest_cas_blob_meta.cpp` | `head` 1, `putIfAbsent` 2, `putOverwrite` 2 | Formats/CasLayout.h, Pool/CasBlobMeta.h, Tools/CasInspect.h |
| `gtest_cas_bootstrap_ordering.cpp` | `casPut` 2, `deleteExact` 3, `get` 8, `head` 4, `list` 2, `putIfAbsent` 15, `putOverwrite` 2 | Backend/CasInMemoryBackend.h, Formats/CasLayout.h, Formats/CasRefCatalogFormat.h, Pool/CasPool.h |
| `gtest_cas_confirm_exact_ref.cpp` | `get` 2 | Backend/CasInMemoryBackend.h, Backend/CasRequestControl.h, ContentAddressedExchange.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefLedger.h, Pool/CasRefProtocol.h |
| `gtest_cas_decommission.cpp` | `casPut` 3, `deleteExact` 9, `get` 51, `getStream` 2, `head` 12, `list` 5, `publishBlob` 2, `putIfAbsent` 15, `putOverwrite` 13 | Gc/CasGc.h, Pool/CasServerRoot.h, Tools/CasDecommission.h |
| `gtest_cas_decommission_catalog_duties.cpp` | `get` 3, `head` 4, `list` 9, `putIfAbsent` 1 | Gc/CasGc.h, Tools/CasDecommission.h |
| `gtest_cas_detached_work.cpp` | `casPut` 2, `get` 12 | Backend/CasInMemoryBackend.h, ContentAddressedMetadataStorage.h, Pool/CasPool.h |
| `gtest_cas_event_log.cpp` | `deleteExact` 1, `get` 8, `head` 1, `putOverwrite` 3 | Backend/CasInMemoryBackend.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Primitives/CasEvent.h |
| `gtest_cas_fence_generation.cpp` | `head` 9, `publishBlob` 2, `putIfAbsent` 2 | ContentAddressedTransaction.h, Formats/CasBlobEnvelopeFormat.h, Pool/CasBlobMeta.h, Pool/CasMountRuntime.h, Pool/CasPartWriteTxn.h |
| `gtest_cas_forget.cpp` | `deleteExact` 1, `get` 8, `head` 2, `list` 2, `putOverwrite` 2 | Backend/CasInMemoryBackend.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasPoolMetaFormat.h, Formats/CasServerRootFormats.h, Gc/CasGcScheduler.h, Pool/CasPool.h |
| `gtest_cas_fsck.cpp` | `deleteExact` 5, `get` 4, `head` 10, `list` 6, `putIfAbsent` 9, `putOverwrite` 6 | Backend/CasInMemoryBackend.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasRefSnapshotFormat.h, Gc/CasGc.h, Pool/CasPool.h, Pool/CasRefProtocol.h, Tools/CasFsck.h |
| `gtest_cas_gc_ack_floor.cpp` | `casPut` 3, `deleteExact` 5, `get` 12, `head` 18, `list` 1, `putIfAbsent` 1, `putOverwrite` 2 | Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasServerRoot.h, Primitives/CasEvent.h, Tools/CasFsck.h |
| `gtest_cas_gc_arithmetic_intake.cpp` | `get` 3, `putIfAbsent` 1 | Formats/CasFoldSealFormat.h, Formats/CasRefLogFormat.h, Gc/CasGc.h, Gc/CasGcScheduler.h, Pool/CasPool.h |
| `gtest_cas_gc_attempt.cpp` | `casPut` 2, `get` 4, `head` 3 | Formats/CasGcStateFormat.h, Gc/CasGc.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_gc_bounded_walk.cpp` | `get` 3, `head` 1 | Formats/CasFoldSealFormat.h, Formats/CasRefLogFormat.h, Gc/CasGc.h, Pool/CasPool.h |
| `gtest_cas_gc_fold.cpp` | `get` 11, `head` 11, `putIfAbsent` 2, `putOverwrite` 1 | Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_gc_frontier_gate.cpp` | `casPut` 6, `deleteExact` 5, `get` 27, `head` 52, `list` 8, `putIfAbsent` 14, `putOverwrite` 5 | Formats/CasFoldSealFormat.h, Formats/CasGcStateFormat.h, Formats/CasRefCkptFormat.h, Formats/CasRefLogFormat.h, Gc/CasGc.h, Gc/CatalogLifecycleReconciler.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefProtocol.h |
| `gtest_cas_gc_hold_grammar.cpp` | `deleteExact` 6, `get` 13, `head` 16, `list` 5, `putIfAbsent` 2, `putOverwrite` 6 | Formats/CasByteBudget.h, Formats/CasFoldSealFormat.h, Formats/CasGcStateFormat.h, Formats/CasRefCkptFormat.h, Formats/CasRefLogFormat.h, Gc/CasGc.h, Gc/CasGcShardPlan.h, Pool/CasPool.h |
| `gtest_cas_gc_leak.cpp` | `head` 7 | Backend/CasInMemoryBackend.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_gc_log.cpp` | `casPut` 4, `get` 2, `head` 2, `list` 6 | Backend/CasInMemoryBackend.h, Gc/CasGc.h, Gc/CasGcScheduler.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h |
| `gtest_cas_gc_maintenance_state_format.cpp` | `casPut` 3, `get` 7, `putIfAbsent` 3 | Formats/CasGcMaintenanceStateFormat.h, Gc/CasGcMaintenanceState.h |
| `gtest_cas_gc_meta_writer.cpp` | `get` 1 | Gc/CasGc.h, Pool/CasBlobMeta.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h |
| `gtest_cas_gc_rebuild.cpp` | `deleteExact` 6, `get` 9, `head` 14, `putIfAbsent` 3 | Formats/CasGcStateFormat.h, Formats/CasRefCkptFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Gc/CasGcShardPlan.h, Pool/CasPool.h, Pool/CasServerRoot.h |
| `gtest_cas_gc_resume.cpp` | `casPut` 2, `get` 4, `head` 2 | Formats/CasGcStateFormat.h, Gc/CasGc.h, Pool/CasPool.h |
| `gtest_cas_gc_round.cpp` | `casPut` 2, `deleteExact` 1, `get` 12, `head` 16, `list` 17, `putIfAbsent` 8, `putOverwrite` 1 | Backend/CasInMemoryBackend.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Gc/CasGcShardPlan.h, Pool/CasPool.h |
| `gtest_cas_gc_round_defer.cpp` | `deleteExact` 1, `get` 9, `head` 8, `putIfAbsent` 9 | Backend/CasInMemoryBackend.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Gc/CasGcMaintenanceState.h, Gc/CasNamespaceJanitor.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_gc_shard_incarnation.cpp` | `get` 1, `head` 11, `list` 1, `putIfAbsent` 5, `putOverwrite` 5 | Backend/CasInMemoryBackend.h, Formats/CasGcStateFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_gc_shard_plan.cpp` | `get` 1, `head` 4 | Backend/CasInMemoryBackend.h, Formats/CasFoldSealFormat.h, Formats/CasGcStateFormat.h, Formats/CasPartManifestFormat.h, Gc/CasGc.h, Gc/CasGcShardPlan.h, Pool/CasPool.h, Primitives/CasTypes.h |
| `gtest_cas_gc_stop_start.cpp` | `get` 3, `putOverwrite` 1 | Backend/CasInMemoryBackend.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasServerRootFormats.h, Gc/CasGcScheduler.h, Pool/CasPool.h |
| `gtest_cas_gc_undercount_repro.cpp` | `casPut` 4, `get` 2, `head` 1 | Formats/CasGcStateFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPool.h |
| `gtest_cas_heartbeat.cpp` | `deleteExact` 1, `get` 16, `head` 4, `putOverwrite` 12 | Backend/CasInMemoryBackend.h, Formats/CasLayout.h, Pool/CasServerRoot.h, Primitives/CasTypes.h |
| `gtest_cas_holey_list_detector.cpp` | `forEachListedKey` 1, `get` 1, `head` 2, `list` 2 | Backend/CasInMemoryBackend.h, Formats/CasRefLogFormat.h, Formats/CasTextFormat.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefProtocol.h |
| `gtest_cas_lifecycle_condition.cpp` | `deleteExact` 1, `get` 6, `head` 2, `list` 2, `putIfAbsent` 2, `putOverwrite` 3 | Backend/CasInMemoryBackend.h, Formats/CasPoolMetaFormat.h, Formats/CasServerRootFormats.h, Pool/CasPool.h |
| `gtest_cas_lifecycle_snapshot.cpp` | `deleteExact` 1, `get` 1 | Backend/CasInMemoryBackend.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Pool/CasPool.h |
| `gtest_cas_list_liar_end_to_end.cpp` | `head` 5 | Formats/CasFoldSealFormat.h, Formats/CasGcStateFormat.h, Formats/CasRefCkptFormat.h, Formats/CasRefLogFormat.h, Gc/CasGc.h, Pool/CasPool.h, Pool/CasRefProtocol.h, Tools/CasFsck.h |
| `gtest_cas_mount.cpp` | `casPut` 5, `deleteExact` 7, `get` 55, `getStream` 2, `head` 12, `list` 2, `probeSentinelRaw` 3, `publishBlob` 2, `putIfAbsent` 16, `putOverwrite` 15 | Formats/CasLayout.h, Pool/CasServerRoot.h |
| `gtest_cas_mount_claim_conflicts.cpp` | `deleteExact` 2, `get` 3, `putOverwrite` 1 | Pool/CasServerRoot.h |
| `gtest_cas_namespace_file_request_profile.cpp` | `get` 3 | ContentAddressedMetadataStorage.h |
| `gtest_cas_namespace_janitor.cpp` | `casPut` 5, `deleteExact` 2, `get` 36, `head` 4, `list` 10, `putIfAbsent` 36 | Gc/CasGcMaintenanceState.h, Gc/CasNamespaceJanitor.h |
| `gtest_cas_ns_file_incarnation.cpp` | `get` 3, `head` 3, `putIfAbsent` 2 | Formats/CasFormat.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefCatalog.h |
| `gtest_cas_ns_file_read_contract.cpp` | `get` 5, `head` 2 | ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasRefCatalogFormat.h, Pool/CasRefCatalog.h |
| `gtest_cas_observability.cpp` | `head` 2, `putOverwrite` 2 | Backend/CasInMemoryBackend.h, Formats/CasGcStateFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Primitives/CasEvent.h, Tools/CasInspect.h |
| `gtest_cas_operation_gate.cpp` | `get` 1, `putOverwrite` 1 | ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasServerRootFormats.h, Gc/CasGcScheduler.h, Pool/CasPool.h |
| `gtest_cas_orphan_manifest_sweep.cpp` | `casPut` 3, `deleteExact` 2, `get` 6, `head` 22, `list` 2, `putIfAbsent` 6 | Formats/CasRefCatalogFormat.h, Gc/CasGcShardPlan.h, Gc/CasOrphanManifestSweep.h, Pool/CasPool.h, Pool/CasRefCatalog.h, Pool/CasRefCkpt.h, Primitives/CasEvent.h |
| `gtest_cas_orphan_nomination.cpp` | `deleteExact` 2, `get` 8, `head` 3, `putOverwrite` 3 | Backend/CasInMemoryBackend.h, Formats/CasGcStateFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPool.h |
| `gtest_cas_part_folder_access.cpp` | `casPut` 2, `deleteExact` 2, `get` 8, `head` 1, `list` 1, `putIfAbsent` 7, `putOverwrite` 2 | ContentAddressedMetadataStorage.h, Parts/PartFolderAccess.h, Pool/CasPartWriteTxn.h, Primitives/CasEvent.h |
| `gtest_cas_part_write.cpp` | `casPut` 10, `deleteExact` 15, `get` 25, `getStream` 10, `head` 53, `list` 13, `publishBlob` 15, `putIfAbsent` 20, `putOverwrite` 10 | Backend/CasInMemoryBackend.h, Formats/CasBlobEnvelopeFormat.h, Formats/CasGcStateFormat.h, Formats/CasPartManifestFormat.h, Formats/CasTextFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Primitives/CasTypes.h |
| `gtest_cas_part_write_root_dangle.cpp` | `deleteExact` 1, `head` 5 | Backend/CasInMemoryBackend.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_pluggable_hash.cpp` | `casPut` 2, `get` 11, `head` 14, `putIfAbsent` 2 | Backend/CasInMemoryBackend.h, ContentAddressedTransaction.h, Formats/CasBlobEnvelopeFormat.h, Formats/CasFormat.h, Formats/CasLayout.h, Formats/CasPartManifestFormat.h, Formats/CasPoolMetaFormat.h, Formats/CasTextFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Primitives/CasBlobHashingWriteBuffer.h, Primitives/CasCodecUtil.h, Tools/CasFsck.h |
| `gtest_cas_pool.cpp` | `casPut` 10, `deleteExact` 11, `get` 43, `getStream` 8, `head` 13, `list` 8, `publishBlob` 8, `putIfAbsent` 25, `putOverwrite` 17 | Backend/CasInMemoryBackend.h, Formats/CasLayout.h, Formats/CasPartManifestFormat.h, Formats/CasPoolMetaFormat.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasServerRoot.h, Primitives/CasTypes.h, Tools/CasFsck.h |
| `gtest_cas_probe.cpp` | `casPut` 2, `deleteExact` 7, `get` 3, `getStream` 2, `head` 3, `list` 7, `publishBlob` 2, `putIfAbsent` 4, `putOverwrite` 2 | Backend/CasInMemoryBackend.h, Backend/CasInstrumentedBackend.h, Backend/CasObjectStorageBackend.h, Backend/CasProbe.h, Pool/CasPool.h |
| `gtest_cas_protocol_scenarios.cpp` | `deleteExact` 2, `get` 1, `head` 23 | Backend/CasInMemoryBackend.h, Formats/CasBlobEnvelopeFormat.h, Formats/CasGcStateFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Tools/CasFsck.h |
| `gtest_cas_rebuild_condemn_nothing.cpp` | `deleteExact` 2, `get` 15, `head` 3, `putIfAbsent` 2, `putOverwrite` 3 | Formats/CasFoldSealFormat.h, Formats/CasGcStateFormat.h, Formats/CasRefCkptFormat.h, Formats/CasRefLogFormat.h, Formats/CasRefSnapshotFormat.h, Gc/CasBlobInDegree.h, Gc/CasGc.h, Pool/CasPool.h, Pool/CasRefProtocol.h, Tools/CasFsck.h |
| `gtest_cas_recovery_grounding.cpp` | `casPut` 2, `list` 2, `putIfAbsent` 9 | Backend/CasInMemoryBackend.h, Formats/CasFormat.h, Formats/CasRefCatalogFormat.h, Formats/CasRefCkptFormat.h, Pool/CasRefCatalog.h, Pool/CasRefCkpt.h, Pool/CasRefProtocol.h |
| `gtest_cas_recovery_streaming.cpp` | `get` 10, `list` 6 | Backend/CasInMemoryBackend.h, Formats/CasTextFormat.h, Pool/CasPool.h, Pool/CasRefProtocol.h, Tools/CasFsck.h |
| `gtest_cas_ref_carve.cpp` | `get` 4, `list` 2 | Backend/CasInMemoryBackend.h, Formats/CasFormat.h, Formats/CasRefLogFormat.h, Formats/CasTextFormat.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefLedger.h, Pool/CasRefProtocol.h |
| `gtest_cas_ref_catalog.cpp` | `casPut` 7, `deleteExact` 2, `get` 5, `head` 2, `putIfAbsent` 12 | Formats/CasRefCatalogFormat.h, Pool/CasRefCatalog.h |
| `gtest_cas_ref_catalog_birth_wiring.cpp` | `casPut` 2, `deleteExact` 2, `get` 5, `head` 14, `putIfAbsent` 7, `putOverwrite` 1 | Backend/CasInMemoryBackend.h, Formats/CasLayout.h, Pool/CasPool.h, Pool/CasRefCatalog.h |
| `gtest_cas_ref_chunked_flush.cpp` | `get` 17, `list` 1 | Backend/CasInMemoryBackend.h, Formats/CasFormat.h, Formats/CasRefLogFormat.h, Formats/CasTextFormat.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefLedger.h, Pool/CasRefProtocol.h |
| `gtest_cas_ref_ckpt.cpp` | `casPut` 13, `get` 8, `head` 9, `putOverwrite` 4 | Backend/CasInMemoryBackend.h, Formats/CasFormat.h, Formats/CasLayout.h, Formats/CasRefCkptFormat.h, Pool/CasPool.h, Pool/CasRefCkpt.h, Pool/CasRefProtocol.h |
| `gtest_cas_ref_contiguous_alloc.cpp` | `deleteExact` 1, `get` 3, `head` 2, `list` 1 | Backend/CasInMemoryBackend.h, Backend/CasRequestControl.h, Formats/CasFormat.h, Formats/CasPoolMetaFormat.h, Pool/CasPool.h, Pool/CasRefLedger.h, Pool/CasRefProtocol.h |
| `gtest_cas_ref_gc.cpp` | `casPut` 4, `deleteExact` 3, `get` 6, `head` 24, `putIfAbsent` 3 | Backend/CasInMemoryBackend.h, Formats/CasTextFormat.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefProtocol.h, Tools/CasFsck.h |
| `gtest_cas_ref_install_safety.cpp` | `head` 2, `putIfAbsent` 1 | Backend/CasInMemoryBackend.h, Backend/CasRequestControl.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefLedger.h, Pool/CasRefProtocol.h |
| `gtest_cas_ref_read_contract.cpp` | `get` 2, `head` 2 | Backend/CasInMemoryBackend.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefCatalog.h |
| `gtest_cas_ref_recovery_cas_walk.cpp` | `casPut` 9, `get` 30, `list` 3, `putIfAbsent` 14, `putOverwrite` 4 | Backend/CasInMemoryBackend.h, Formats/CasFormat.h, Formats/CasLayout.h, Formats/CasRefCkptFormat.h, Formats/CasRefLogFormat.h, Pool/CasPool.h, Pool/CasRefCkpt.h, Pool/CasRefProtocol.h, Pool/CasServerRoot.h |
| `gtest_cas_ref_statemachine.cpp` | `head` 1 | Formats/CasRefLogFormat.h, Formats/CasRefSnapshotFormat.h, Pool/CasRefProtocol.h, Primitives/CasTypes.h |
| `gtest_cas_ref_wedge_every_attempt.cpp` | `casPut` 4, `get` 29, `putIfAbsent` 6, `putIfAbsentControlled` 2 | Backend/CasInMemoryBackend.h, Backend/CasRequestControl.h, Formats/CasRefLogFormat.h, Formats/CasTextFormat.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefCatalog.h, Pool/CasRefProtocol.h |
| `gtest_cas_ref_writer.cpp` | `casPut` 8, `forEachListedKey` 1, `get` 54, `head` 1, `list` 14, `putIfAbsent` 19, `putIfAbsentControlled` 3, `putOverwrite` 4 | Backend/CasInMemoryBackend.h, Formats/CasRefCkptFormat.h, Formats/CasRefLogFormat.h, Formats/CasRefSnapshotFormat.h, Formats/CasTextFormat.h, Gc/CasGc.h, Gc/CasOrphanManifestSweep.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasRefProtocol.h, Primitives/CasEvent.h |
| `gtest_cas_request_control.cpp` | `get` 10, `head` 1, `putIfAbsent` 7, `putIfAbsentControlled` 13, `putOverwrite` 6, `putOverwriteControlled` 27 | Backend/CasInMemoryBackend.h, Backend/CasObjectStorageBackend.h, Backend/CasRequestControl.h |
| `gtest_cas_retirement_sweep.cpp` | `get` 1, `head` 4, `list` 5, `putIfAbsent` 3, `putOverwrite` 1 | Backend/CasInMemoryBackend.h, ContentAddressedSettings.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasServerRoot.h |
| `gtest_cas_s3_staging.cpp` | `deleteExact` 7, `get` 6, `getStream` 3, `head` 11, `publishBlob` 5, `putIfAbsent` 9 | Backend/CasObjectStorageBackend.h, ContentAddressedMetadataStorage.h, ContentAddressedTransaction.h, Formats/CasLayout.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Pool/CasServerRoot.h |
| `gtest_cas_sentinel_probe.cpp` | `get` 2, `head` 2, `list` 2, `putIfAbsent` 4 | Backend/CasBackend.h, Backend/CasInMemoryBackend.h, Backend/CasInstrumentedBackend.h, Backend/CasObjectStorageBackend.h, Backend/CasSentinelProbe.h |
| `gtest_cas_settings.cpp` | `get` 1 | ContentAddressedSettings.h |
| `gtest_cas_shutdown_context.cpp` | `get` 1 | Backend/CasInMemoryBackend.h, ContentAddressedMetadataStorage.h, Pool/CasPool.h |
| `gtest_cas_slot_occupy.cpp` | `deleteExact` 1, `get` 2, `head` 4, `putIfAbsent` 7, `slotOccupy` 12 | Backend/CasInMemoryBackend.h, Backend/CasRequestControl.h |
| `gtest_cas_sweep_deletion_premise.cpp` | `head` 6, `putIfAbsent` 1 | Formats/CasFoldSealFormat.h, Gc/CasOrphanManifestSweep.h, Pool/CasPool.h |
| `gtest_cas_upload_detached.cpp` | `deleteExact` 1, `get` 5, `getStream` 1, `head` 12, `publishBlob` 2, `putIfAbsent` 4 | Backend/CasInMemoryBackend.h, Formats/CasBlobEnvelopeFormat.h, Formats/CasLayout.h, Pool/CasBlobMeta.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Primitives/CasTypes.h |
| `gtest_cas_upload_fanout.cpp` | `deleteExact` 1, `get` 3, `head` 8, `publishBlob` 2, `putIfAbsent` 9 | Backend/CasInMemoryBackend.h, ContentAddressedTransaction.h, Formats/CasBlobEnvelopeFormat.h, Formats/CasLayout.h, Gc/CasGc.h, Pool/CasBlobMeta.h, Pool/CasBlobUploadPool.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h, Primitives/CasTypes.h |
| `gtest_cas_writer_duties.cpp` | `get` 1, `head` 4 | Backend/CasInMemoryBackend.h, Formats/CasServerRootFormats.h, Gc/CasGc.h, Pool/CasPartWriteTxn.h, Pool/CasPool.h |

### 5b. Fault-injection subclasses {#fault-injection-subclasses}

Command (multi-line-tolerant, catches `class X final\n : public InMemoryBackend` as well as single-line forms):
```
rg -n -U --type-add 'ch:*.cpp' -t ch --pcre2 \
  'class\s+\w+(\s+final)?\s*\n?\s*:\s*public\s+(Cas::)?(Backend|InMemoryBackend|ObjectStorageBackend|InstrumentedBackend)\b' \
  src/Disks/tests -g 'gtest_cas*.cpp' -g 'gtest_ca_wiring.cpp'
```
75 classes derive from `Backend` (2) or `InMemoryBackend` (73); zero derive directly from `ObjectStorageBackend`
or `InstrumentedBackend` in test code (a separate targeted `rg` for `public\s+(Cas::)?(ObjectStorageBackend|
InstrumentedBackend)\b` over the same file set returned nothing — tests build fault injection over the in-memory
backend, not the real S3 one). For each class, its body was extracted by brace-depth tracking from the `class`
line to the matching closing brace, and every `override`-marked method whose name is one of the 15 transport/
controller methods was recorded — including 15 cases where the method name and the `override` keyword sit on
different lines of a multi-line signature (found by reading the class body directly, not by grep) and one case
(`gtest_cas_probe.cpp:101`, `PreconditionRefusingBackend`) that overrides `checkPoolPreconditions()`, a
production hook that is not one of the 15 named methods, and one (`gtest_cas_ns_creation_lifecycle.cpp:67`,
`InitializedCatalogBackend`) that overrides nothing at all — it is a plain `InMemoryBackend` that seeds the
catalog in its constructor.

**These 75 classes are exactly the fault-injection seams the spec's step 3 says must move with their production
site, in the same commit, with the commit showing the fault still fires.** Each row's "methods overridden" column
names the legacy transport primitive(s) whose new-verb equivalent (from section 1's per-file notes, where
resolved) that seam's override must be re-pointed at.

| file:line | class | base | methods overridden |
|---|---|---|---|
| `gtest_cas_backend.cpp:109` | `PublishCountingInMemoryBackend` | `InMemoryBackend` | publishBlob |
| `gtest_cas_blob_meta.cpp:104` | `ControlledMetaWriteFaultBackend` | `InMemoryBackend` | putIfAbsent,putOverwrite |
| `gtest_cas_bootstrap_ordering.cpp:125` | `CatalogMissingAfterListBackend` | `InMemoryBackend` | get |
| `gtest_cas_bootstrap_ordering.cpp:44` | `RecordingBackend` | `InMemoryBackend` | casPut,deleteExact,list,putIfAbsent,putOverwrite |
| `gtest_cas_decommission.cpp:1176` | `FailDeletesUnderPrefixBackend` | `Backend` | casPut,deleteExact,get,getStream,head,list,publishBlob,putIfAbsent,putOverwrite |
| `gtest_cas_decommission.cpp:146` | `SuccessorReclaimAfterFarewellBackend` | `InMemoryBackend` | get |
| `gtest_cas_decommission.cpp:229` | `SuccessorReclaimAfterEpochDeleteBackend` | `InMemoryBackend` | deleteExact |
| `gtest_cas_decommission.cpp:304` | `SuccessorOwnerRewriteBeforeTombstoneBackend` | `InMemoryBackend` | deleteExact,get |
| `gtest_cas_decommission.cpp:358` | `AmbiguousOwnerTombstoneBackend` | `InMemoryBackend` | putOverwrite |
| `gtest_cas_decommission.cpp:49` | `FailingDeleteBackend` | `InMemoryBackend` | deleteExact |
| `gtest_cas_decommission.cpp:74` | `CatalogChangesAfterFirstReadBackend` | `InMemoryBackend` | get |
| `gtest_cas_decommission_catalog_duties.cpp:56` | `AddVictimEntryDuringRootDrainBackend` | `InMemoryBackend` | list |
| `gtest_cas_decommission_catalog_duties.cpp:89` | `MutateCatalogBetweenRetirementReadsBackend` | `InMemoryBackend` | get,list |
| `gtest_cas_event_log.cpp:38` | `RenewalEventBackend` | `InMemoryBackend` | get |
| `gtest_cas_fence_generation.cpp:148` | `BlobPublicationFenceBackend` | `InMemoryBackend` | head,publishBlob |
| `gtest_cas_fence_generation.cpp:43` | `TripOnHeadBackend` | `InMemoryBackend` | head |
| `gtest_cas_fence_generation.cpp:59` | `TripOnSecondHeadBackend` | `InMemoryBackend` | head,putIfAbsent |
| `gtest_cas_fsck.cpp:125` | `FsckListingBackend` | `InMemoryBackend` | list |
| `gtest_cas_fsck.cpp:155` | `FailExactGetBackend` | `InMemoryBackend` | get |
| `gtest_cas_fsck.cpp:45` | `RepublishOnListBackend` | `InMemoryBackend` | list |
| `gtest_cas_fsck.cpp:500` | `AdmitLifeAfterNamespaceListingBackend` | `InMemoryBackend` | list |
| `gtest_cas_fsck.cpp:83` | `MutateOnFirstGetBackend` | `InMemoryBackend` | get |
| `gtest_cas_gc_ack_floor.cpp:74` | `TokenMismatchOnAbsentBackend` | `InMemoryBackend` | deleteExact |
| `gtest_cas_gc_ack_floor.cpp:94` | `CkptReplacementConflictBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_gc_arithmetic_intake.cpp:415` | `AlternatingGetBackend` | `InMemoryBackend` | get |
| `gtest_cas_gc_attempt.cpp:85` | `InterruptRoundCasBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_gc_hold_grammar.cpp:1485` | `BroadListHoleBackend` | `InMemoryBackend` | list |
| `gtest_cas_gc_hold_grammar.cpp:769` | `AlternatingGetBackend` | `InMemoryBackend` | get |
| `gtest_cas_gc_log.cpp:183` | `ThrowingBackend` | `InMemoryBackend` | get,head,list |
| `gtest_cas_gc_log.cpp:312` | `NetworkThrowingBackend` | `InMemoryBackend` | list |
| `gtest_cas_gc_log.cpp:358` | `StateCommitThrowingBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_gc_log.cpp:412` | `ModalThrowingBackend` | `InMemoryBackend` | list |
| `gtest_cas_gc_maintenance_state_format.cpp:18` | `FailingMaintenanceReadBackend` | `InMemoryBackend` | get |
| `gtest_cas_gc_resume.cpp:59` | `InterruptRoundCasBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_gc_round.cpp:95` | `GcStateCasFaultBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_gc_undercount_repro.cpp:155` | `InterruptRoundCasBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_gc_undercount_repro.cpp:252` | `DropAtCommitBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_heartbeat.cpp:42` | `RenewalScriptBackend` | `InMemoryBackend` | get,putOverwrite |
| `gtest_cas_holey_list_detector.cpp:56` | `HoleyListBackend` | `InMemoryBackend` | list |
| `gtest_cas_lifecycle_condition.cpp:63` | `ToggleableTransportFaultBackend` | `InMemoryBackend` | get,head,list |
| `gtest_cas_mount.cpp:113` | `RenewalLogBackend` | `InMemoryBackend` | putOverwrite |
| `gtest_cas_mount.cpp:1592` | `RenewOnFenceBackend` | `InMemoryBackend` | putOverwrite |
| `gtest_cas_mount.cpp:529` | `IndeterminateProbeBackend` | `InMemoryBackend` | probeSentinelRaw |
| `gtest_cas_mount.cpp:573` | `ProbeCountingBackend` | `InMemoryBackend` | probeSentinelRaw |
| `gtest_cas_mount.cpp:66` | `OwnerConflictRevealsManifestBackend` | `InMemoryBackend` | putIfAbsent |
| `gtest_cas_mount.cpp:85` | `EpochConflictRevealsManifestBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_ns_creation_lifecycle.cpp:67` | `InitializedCatalogBackend` | `InMemoryBackend` | (none — plain InMemoryBackend, seeds catalog in ctor, no override) |
| `gtest_cas_observability.cpp:43` | `RenewalCounterBackend` | `InMemoryBackend` | putOverwrite |
| `gtest_cas_orphan_manifest_sweep.cpp:110` | `ReplacingManifestAfterObservationBackend` | `InMemoryBackend` | get,list |
| `gtest_cas_orphan_manifest_sweep.cpp:63` | `CatalogChangingOnSecondReadBackend` | `InMemoryBackend` | get |
| `gtest_cas_orphan_nomination.cpp:83` | `NominationBackend` | `InMemoryBackend` | deleteExact |
| `gtest_cas_part_folder_access.cpp:115` | `PromoteConflictOnceBackend` | `InMemoryBackend` | putIfAbsent |
| `gtest_cas_part_folder_access.cpp:152` | `PromoteDefiniteFailureBackend` | `InMemoryBackend` | putIfAbsent |
| `gtest_cas_part_folder_access.cpp:69` | `RollbackFaultBackend` | `InMemoryBackend` | casPut,deleteExact,putIfAbsent,putOverwrite |
| `gtest_cas_part_write.cpp:1918` | `RefLogConflictOnceBackend` | `InMemoryBackend` | putIfAbsent |
| `gtest_cas_part_write.cpp:217` | `RacingBlobPublicationBackend` | `InMemoryBackend` | head,publishBlob |
| `gtest_cas_part_write.cpp:2214` | `ManifestPutFaultBackend` | `InMemoryBackend` | putIfAbsent |
| `gtest_cas_part_write.cpp:2355` | `BlobPutFaultBackend` | `InMemoryBackend` | head,publishBlob |
| `gtest_cas_pool.cpp:557` | `RacingBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_probe.cpp:101` | `PreconditionRefusingBackend` | `InMemoryBackend` | (none of the 15 transport methods — overrides checkPoolPreconditions(), a different production hook) |
| `gtest_cas_probe.cpp:186` | `IgnoresDeleteTokenBackend` | `InMemoryBackend` | deleteExact |
| `gtest_cas_probe.cpp:197` | `RejectsDeleteTokenBackend` | `InMemoryBackend` | deleteExact |
| `gtest_cas_probe.cpp:240` | `DialectGatedCountingBackend` | `Backend` | casPut,deleteExact,get,getStream,head,list,publishBlob,putIfAbsent,putOverwrite |
| `gtest_cas_probe.cpp:31` | `EmptyTokenDeleteRecorder` | `InMemoryBackend` | deleteExact |
| `gtest_cas_recovery_streaming.cpp:143` | `VanishMidTailOnceBackend` | `InMemoryBackend` | get,list |
| `gtest_cas_recovery_streaming.cpp:172` | `CorruptLogOnGetBackend` | `InMemoryBackend` | get,list |
| `gtest_cas_recovery_streaming.cpp:202` | `BlockingFirstLogGetBackend` | `InMemoryBackend` | get,list |
| `gtest_cas_ref_gc.cpp:91` | `DeposeRoundCommitBackend` | `InMemoryBackend` | casPut |
| `gtest_cas_request_control.cpp:263` | `ScriptedControllerBackend` | `InMemoryBackend` | get,putIfAbsent,putOverwrite |
| `gtest_cas_retirement_sweep.cpp:114` | `RefPrefixListCountingBackend` | `InMemoryBackend` | list |
| `gtest_cas_retirement_sweep.cpp:138` | `UnresolvedPutBackend` | `InMemoryBackend` | putIfAbsent |
| `gtest_cas_retirement_sweep.cpp:63` | `HoleyListBackend` | `InMemoryBackend` | list |
| `gtest_cas_sentinel_probe.cpp:39` | `TransportFaultBackend` | `InMemoryBackend` | get,head,list |
| `gtest_cas_upload_detached.cpp:106` | `ProtocolRecordingBackend` | `InMemoryBackend` | get,head,publishBlob |
| `gtest_cas_upload_fanout.cpp:224` | `RejectFirstStagedCopyBackend` | `InMemoryBackend` | publishBlob |

### 5c. Production component exercised {#test-production-component-exercised}

Because section 3 found one 30-file connected component (`CasRequestControl`) plus a `CasProbe.cpp` singleton, a
per-test-file "which component" column would read "`CasRequestControl`" almost everywhere — not useful on its
own. The "production headers included" column in section 5a is the actual signal: it lists, per test file, every
`ContentAddressed/…` header it `#include`s (found by `grep -oE 'ContentAddressed/[A-Za-z/]+\.h'`), which names the
exact production files (and hence exact token-flow component members, cross-referenced against section 3's file
list) each test exercises. No test file includes *only* `Backend/CasProbe.h`/`Backend/CasSentinelProbe.h` without
also including `Pool/CasPool.h` or another big-component header (checked directly: `gtest_cas_probe.cpp` and
`gtest_cas_sentinel_probe.cpp` both also include `Pool/CasPool.h` or the full backend set) — so in practice every
test file in this census exercises the big `CasRequestControl` component; none exercises the `CasProbe.cpp`
singleton in isolation.

## 6. Old controller surface {#old-controller-surface}

### Files including `CasRequestControl.h` {#files-including-casrequestcontrol-h}

Command: `grep -rl '#include.*CasRequestControl.h' <tree>`.

Production (13 files): `ContentAddressedMetadataStorage.cpp`, `Pool/CasBlobMeta.h`, `Pool/CasRefLedger.h`,
`Pool/CasServerRoot.h`, `Pool/CasRefCkpt.cpp`, `Pool/CasMountRuntime.h`, `Backend/CasRequestControl.cpp`,
`Pool/CasPartWriteTxn.cpp`, `Pool/CasRefCatalog.cpp`, `Pool/CasPool.h`, `Backend/CasObjectStorageBackend.cpp`,
`Gc/CatalogLifecycleReconciler.cpp`, `Pool/CasRefLedger.cpp`.

Test (6 files): `gtest_cas_slot_occupy.cpp`, `gtest_cas_ref_wedge_every_attempt.cpp`,
`gtest_cas_ref_install_safety.cpp`, `gtest_cas_confirm_exact_ref.cpp`, `gtest_cas_request_control.cpp`,
`gtest_cas_ref_contiguous_alloc.cpp`.

### Symbol references {#old-controller-symbol-references}

Command (per symbol, over the full `ContentAddressed/` tree, no exclusion — every hit below is already outside
the excluded backend-implementation files except where a file is explicitly named):
```
rg -n --type-add 'ch:*.{cpp,h}' -t ch '\b<SYMBOL>\b' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```
for `<SYMBOL>` in `CasRequestBudget`, `CasUnresolvedReason`, `CasOverwriteOperationContext`,
`CasOverwriteDiagnostics`, `unresolvedProvesNothingWasSent`, `classifyConditionalWriteResult`,
`isDeterministicLocalFailure`, `isDeterministicBlobPublicationFailure`, `validateCasRequestBudget`,
`pauseBeforeReissue`. Note `classifyConditionalWriteResult` and `isDeterministicLocalFailure` do have production
hits inside `Backend/CasInMemoryBackend.{h,cpp}` and `Backend/CasObjectStorageBackend.cpp` — the backend
implementations themselves define/use these classifiers, so unlike sections 1–4 they are *not* excluded here
(the task scopes this section as "every reference", not "every caller outside the backend").

| symbol | file | lines |
|---|---|---|
| `CasRequestBudget` | `Backend/CasRequestControl.cpp` | 163,258 |
| `CasRequestBudget` | `Backend/CasRequestControl.h` | 146,223,452,630 |
| `CasRequestBudget` | `Pool/CasMountRuntime.cpp` | 60 |
| `CasRequestBudget` | `Pool/CasMountRuntime.h` | 143,425 |
| `CasRequestBudget` | `Pool/CasPool.h` | 231 |
| `CasRequestBudget` | `Pool/CasRefLedger.cpp` | 205 |
| `CasRequestBudget` | `Pool/CasRefLedger.h` | 96,704 |
| `CasRequestBudget` | `Pool/CasServerRoot.cpp` | 1657 |
| `CasRequestBudget` | `Pool/CasServerRoot.h` | 499 |
| `CasRequestBudget` | `Tools/CasDecommission.cpp` | 421 |
| `CasUnresolvedReason` | `Backend/CasRequestControl.cpp` | 134,287,295,306,348,363,370,378,379,381,382,414,438,439,454,461,533,549,553,554,555,599,630,723,849,872,890,906,909 |
| `CasUnresolvedReason` | `Backend/CasRequestControl.h` | 46,72,77,81,83,84,85,86,87,89,96,100,101,104,106,108,110,112,377,424,433,493,624 |
| `CasUnresolvedReason` | `Pool/CasMountRuntime.cpp` | 490,491 |
| `CasUnresolvedReason` | `Pool/CasRefLedger.cpp` | 3745 |
| `CasUnresolvedReason` | `Pool/CasServerRoot.cpp` | 99,235,239,240,241,242,243,244,245,290,346,380,381,382,383,387,388,396 |
| `CasOverwriteOperationContext` | `Backend/CasRequestControl.cpp` | 136,469,490,499 |
| `CasOverwriteOperationContext` | `Backend/CasRequestControl.h` | 364,535,611 |
| `CasOverwriteOperationContext` | `Pool/CasServerRoot.cpp` | 1723 |
| `CasOverwriteDiagnostics` | `Backend/CasRequestControl.cpp` | 504 |
| `CasOverwriteDiagnostics` | `Backend/CasRequestControl.h` | 373,394 |
| `CasOverwriteDiagnostics` | `Pool/CasServerRoot.cpp` | 1561,1625 |
| `CasOverwriteDiagnostics` | `Pool/CasServerRoot.h` | 54,509,512 |
| `unresolvedProvesNothingWasSent` | `Backend/CasRequestControl.h` | 43,77,412,417,473 |
| `unresolvedProvesNothingWasSent` | `Pool/CasRefLedger.cpp` | 1263,2520,4066,4085 |
| `classifyConditionalWriteResult` | `Backend/CasInMemoryBackend.cpp` | 98 |
| `classifyConditionalWriteResult` | `Backend/CasInMemoryBackend.h` | 101 |
| `classifyConditionalWriteResult` | `Backend/CasObjectStorageBackend.cpp` | 220,225 |
| `classifyConditionalWriteResult` | `Backend/CasRequestControl.cpp` | 44,401,621,774,866 |
| `classifyConditionalWriteResult` | `Backend/CasRequestControl.h` | 123,131,137,586 |
| `classifyConditionalWriteResult` | `Pool/CasPartWriteTxn.cpp` | 75 |
| `isDeterministicLocalFailure` | `Backend/CasInMemoryBackend.cpp` | 98,100 |
| `isDeterministicLocalFailure` | `Backend/CasRequestControl.cpp` | 113,620,772,864 |
| `isDeterministicLocalFailure` | `Backend/CasRequestControl.h` | 521,587 |
| `isDeterministicBlobPublicationFailure` | `Pool/CasPartWriteTxn.cpp` | 73,421 |
| `validateCasRequestBudget` | `Backend/CasRequestControl.cpp` | 163 |
| `validateCasRequestBudget` | `Backend/CasRequestControl.h` | 143,175,194,223 |
| `validateCasRequestBudget` | `Pool/CasPool.cpp` | 91,792 |
| `validateCasRequestBudget` | `Pool/CasPool.h` | 227 |
| `pauseBeforeReissue` | `Backend/CasRequestControl.cpp` | 286,430,440,824 |
| `pauseBeforeReissue` | `Backend/CasRequestControl.h` | 623 |

## 7. Persisted token formats {#persisted-token-formats}

Command:
```
rg -n --type-add 'ch:*.{cpp,h}' -t ch '\b<SYMBOL>\b' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
```
for `<SYMBOL>` in `TokenFields`, `writeTokenFields`, `tokenTypeFromWord`, `token_type` — all four live entirely
under `Formats/CasWireVocab.{h,cpp}` (definitions) and its two callers `Formats/CasGcOutcomesFormat.cpp` and
`Formats/CasRecordStreamFormat.{cpp,h}` (plus one mention inside `Gc/CasBlobInDegree.{h,cpp}`'s condemned-row
binary layout comment, which documents the same `token_type` byte but does not call `writeTokenFields`/
`tokenTypeFromWord` directly — it hand-encodes the byte itself).

| symbol | file | line | context |
|---|---|---|---|
| `TokenFields` | `Formats/CasWireVocab.h` | 149 | `struct TokenFields` definition |
| `TokenFields` | `Formats/CasWireVocab.h` | 180 | `matchTokenFields(std::string_view key, JsonObjectReader & r, TokenFields & fields)` |
| `TokenFields` | `Formats/CasWireVocab.cpp` | 110 | `Token TokenFields::build(std::string_view what) const` |
| `TokenFields` | `Formats/CasRecordStreamFormat.cpp` | 280 | local `TokenFields token_fields;` in the record decoder |
| `TokenFields` | `Formats/CasGcOutcomesFormat.cpp` | 104 | local `TokenFields token_fields;` in the outcomes decoder |
| `writeTokenFields` | `Formats/CasWireVocab.h` | 55 | declaration |
| `writeTokenFields` | `Formats/CasWireVocab.h` | 61 | doc comment naming it |
| `writeTokenFields` | `Formats/CasWireVocab.cpp` | 48 | `void writeTokenFields(CasJsonWriter & out, bool & first, const Token & t)` definition |
| `writeTokenFields` | `Formats/CasRecordStreamFormat.cpp` | 201 | called encoding a ref-log record |
| `writeTokenFields` | `Formats/CasGcOutcomesFormat.cpp` | 58 | called encoding a GC outcome |
| `tokenTypeFromWord` | `Formats/CasWireVocab.h` | 38 | declaration |
| `tokenTypeFromWord` | `Formats/CasWireVocab.cpp` | 28 | `TokenType tokenTypeFromWord(std::string_view w, std::string_view what)` definition |
| `tokenTypeFromWord` | `Formats/CasWireVocab.cpp` | 114 | called inside `TokenFields::build` |
| `token_type` (wire key) | `Formats/CasWireVocab.h` | 67 | `inline constexpr WireKey token_type{"token_type"};` — the wire-key constant |
| `token_type` | `Formats/CasWireVocab.h` | 53,61,148 | doc comments |
| `token_type` | `Formats/CasWireVocab.h` | 182 | `matchTokenFields`'s field dispatch: `if (key == SharedWire::token_type) …` |
| `token_type` | `Formats/CasWireVocab.cpp` | 50 | `writeTokenFields` emits the field via `SharedWire::token_type` |
| `token_type` | `Formats/CasWireVocab.cpp` | 113 | error message on missing `token_type`/`token` |
| `token_type` | `Formats/CasGcOutcomesFormat.cpp` | 58 | comment, same call as `writeTokenFields` above |
| `token_type` | `Formats/CasRecordStreamFormat.cpp` | 201 | comment, same call as `writeTokenFields` above |
| `token_type` | `Formats/CasRecordStreamFormat.h` | 68,76 | doc comments giving the JSON shape |
| `token_type` (hand-encoded byte, not via `writeTokenFields`) | `Gc/CasBlobInDegree.cpp` | 207,219 | binary condemned-row layout comment and `unknown token_type {}` error |
| `token_type` (hand-encoded byte) | `Gc/CasBlobInDegree.h` | 72 | binary layout doc comment |

**`PersistedIncarnation`-to-be**: `PersistedIncarnation { String dialect; String value; }` does not exist in
production code today — `rg -n 'PersistedIncarnation' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`
returns zero hits. It is the spec's target name (design doc lines 726 and 1027) for what `TokenFields`/
`writeTokenFields`/`tokenTypeFromWord`/`token_type` become; the five rows above are exactly its migration sites.

## 8. Totals {#totals}

### Transport/controller call sites, production vs. test {#totals-transport-calls}

| method | production call sites | test call sites (raw, unfiltered receiver) |
|---|---|---|
| `get` | 80 | 751 |
| `getStream` | 2 | 34 |
| `head` | 23 | 536 |
| `putIfAbsent` | 12 | 389 |
| `putOverwrite` | 9 | 151 |
| `casPut` | 17 | 139 |
| `deleteExact` | 17 | 143 |
| `publishBlob` | 1 | 61 |
| `list` | 12 | 154 |
| `probeSentinelRaw` | 2 | 3 |
| `putIfAbsentControlled` | 3 | 18 |
| `putOverwriteControlled` | 3 | 27 |
| `putIfAbsentControlledMutable` | 1 | 0 |
| `slotOccupy` | 2 | 12 |
| `resolveByExactGet` | 1 | 0 |
| `forEachListedKey` | 11 | 4 |
| **total** | **196** | **2422** |

Production totals sum section 1's receiver-filtered counts (196 total). Test totals sum section 5a's raw,
unfiltered-by-receiver counts (2422 total) — see 5a's note on the known false-positive receiver list before
treating these as exact per-method budgets.

### Test subclasses by base {#totals-subclasses}

| base class | test subclasses |
|---|---|
| `InMemoryBackend` | 73 |
| `Backend` | 2 |
| **total** | **75** |

### Fence checks by kind {#totals-fence-checks}

| classification | count |
|---|---|
| verdict point | 37 |
| pre-request | 7 |
| admission capture | 2 |
| **call sites (subtotal)** | **46** |
| footnoted (not call sites: definitions, wiring, callback bodies) | 13 |
| **all matched lines** | **59** |

### Section cross-reference {#totals-cross-reference}

| section | scope | count |
|---|---|---|
| 1 | production files with a transport/controller call | 22 of 129 |
| 1 | total production transport/controller call sites | 196 |
| 2 | distinct functions/constructors declared over `Backend &`/`Backend *`/`BackendPtr` | 123 |
| 3 | files with a `Token`-carrying declaration | 32 |
| 3 | connected components | 2 (one 30-file component named `CasRequestControl`; one 1-file singleton, `CasProbe.cpp`) |
| 4 | fence-check call sites classified | 46 (37 verdict point, 7 pre-request, 2 admission capture) |
| 5a | test files (of 134) with a transport/controller call | 85 |
| 5b | fault-injection subclasses | 75 (73 over `InMemoryBackend`, 2 over `Backend` directly) |
| 6 | files including `CasRequestControl.h` | 13 production + 6 test |
| 7 | persisted-token-format symbol references | 22 (all confined to `Formats/CasWireVocab.{h,cpp}` and its two callers, plus 3 hand-encoded-byte mentions in `Gc/CasBlobInDegree.{h,cpp}`) |

## Unresolved ambiguities {#unresolved-ambiguities}

- Section 3's `CasProbe.cpp` singleton classification depends on treating the raw `Backend` interface's own
  `Token`-taking signatures as *not* a qualifying edge (see section 3's closing note) — the opposite choice
  merges it into the big component.
- Section 5a's transport counts are not receiver-filtered per line; the false-positive receiver list given there
  is exact for what was checked but the check itself was a representative audit, not exhaustive over all 2422 hits.
- `gtest_cascade_and_memory_write_buffer.cpp` matches the `gtest_cas*.cpp` glob but is unrelated to
  content-addressed storage; excluded from section 5, flagged rather than silently dropped.
- Section 4's "pre-request" vs. "verdict point" split for the 13 footnoted lines (definitions, constructor
  wiring, callback bodies) has no natural home in either bucket; they are reported separately rather than forced
  into one.
