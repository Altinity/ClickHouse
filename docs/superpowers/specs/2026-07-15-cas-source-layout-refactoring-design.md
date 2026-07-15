---
description: 'Design spec for the CAS source-layout refactoring: a layered self-describing directory tree replaces the flat Core/, tightly-coupled micro-files merge, the CasStore/CasBuild god objects decompose into per-subsystem components, and Store/Build are renamed to Pool/PartWriteTxn. Zero behavior change.'
sidebar_label: 'CAS source layout refactoring'
sidebar_position: 61
slug: /superpowers/specs/2026-07-15-cas-source-layout-refactoring-design
title: 'CAS Source Layout Refactoring Design'
doc_type: 'reference'
---

# CAS Source Layout Refactoring Design {#cas-source-layout-refactoring-design}

**Status:** approved design, 2026-07-15.

**Goal:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` becomes a readable,
self-describing tree: the entry points sit on top, implementation lives in layered subdirectories
with a strict one-direction include rule, tightly-coupled micro-files are merged (no Java-style
fragmentation), and the two god objects (`CasStore`, `CasBuild`) are decomposed into components cut
along state/lock ownership boundaries. **Zero behavior change** — see the operational definition
in [Invariants](#invariants).

**Decisions fixed during brainstorm (2026-07-15):**

1. Full scope in one spec: file merges + directory moves + god-object decomposition + renames,
   executed in phases (layout first, surgery second).
2. Renames included: `Cas::Store` → `Cas::Pool`, `Cas::Build` → `Cas::PartWriteTxn`, last phase.
3. Merge set: both tiers (co-change-proven + conceptual), re-scoped to the post-codecs-v3
   baseline.
4. Both dependency-direction violations are fixed, not documented around. (One of them —
   `CasObjectStorageBackend.cpp` → `CasTextFormat` — dissolves by itself when codecs v3 moves
   `sealObject`/`openObject` out of the backend into the codecs; the planned `SealingBackend`
   decorator is therefore dropped.)
5. Direction enforcement: README rule only, no CI script.
6. Verification: per-phase full cas-gtest battery; final gate = CA-default stateless run plus a
   short ca-soak after the `CasStore` decomposition.
7. Phase order: approach A — layout first (merges, moves), surgery second (decomposition), renames
   last.

## Precondition: Codecs V3 Is Landed {#precondition-codecs-v3}

This refactoring starts only after **all 8 migration steps** of
`docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md` are landed and green. The baseline tree
for every phase of this spec is the post-v3 state:

- `Core/Formats/` holds the per-object format files (`CasFormat`, `CasTextFormat`,
  `CasPoolMetaFormat`, `CasRefLogFormat`, `CasRefSnapshotFormat`, `CasPartManifestFormat`,
  `CasRecordStreamFormat`, `CasFoldSealFormat`, `CasGcStateFormat`, `CasGcOutcomesFormat`,
  `CasServerRootFormats`, `CasBlobEnvelopeFormat`, `CasBlobMetaFormat`) plus the v3 registry
  `README.md`. The per-object-file principle there is a settled v3 decision; this refactoring does
  not re-merge those files.
- The physical layering rule is in force: `Formats/` includes only IO primitives and the identifier
  vocabulary — never `CasBackend.h` or subsystem headers.
- `CasCodecUtil.h` is gone, `CasRunFile` became `Formats/CasRecordStreamFormat`, protobuf and
  `Core/Proto/` are gone, `CasInspect` is gutted to a thin decompress-print or deleted,
  `sealObject`/`openObject` are called by codecs (not by the backend).

**Plans must re-verify the baseline** against the actually-landed v3 tree before executing (exact
surviving file names, `CasInspect` fate, `CasWireVocab` placement) — v3 plans allow local
interface decisions at draft time.

## Target Layout {#target-layout}

```text
ContentAddressed/
├── README.md                                 layer map, include rule, reading order (NEW)
├── ContentAddressedMetadataStorage.{h,cpp}   IMetadataStorage facade — THE entry point
├── ContentAddressedTransaction.{h,cpp}       IMetadataTransaction (+ merged write buffers)
├── ContentAddressedExchange.h                replication seam (DataPartsExchange)
│
├── Parts/                                    part semantics over the pool
│   ├── PartPathParser.{h,cpp}                pure ClickHouse-path classifier
│   └── PartFolderAccess.{h,cpp}              PartRefKey + Freshness + PartFolderValidate
│                                             + PartFolderView + CachedPartFolderAccess
│
├── Tools/                                    operator verbs (clickhouse-disks)
│   ├── CasFsck.{h,cpp}
│   └── CasDecommission.{h,cpp}               (+ CasInspect only if it survives v3 step 8)
│
├── Gc/                                       garbage collection (reunited)
│   ├── CasGcScheduler.{h,cpp}                pacing thread (+ merged GcRoundLogRecord)
│   ├── CasGc.{h,cpp}                         the round engine
│   ├── CasGcShardPlan.{h,cpp}                sharding math (+ merged CasGcCursorKey)
│   ├── CasBlobInDegree.{h,cpp}               in-degree engine
│   └── CasOrphanManifestSweep.{h,cpp}        orphan part-manifest sweep
│
├── Pool/                                     the pool engine
│   ├── CasPool.{h,cpp}                       composition root (ex-CasStore, ~400 lines)
│   ├── CasRefLedger.{h,cpp}                  ref tables: lane, recovery, publish, sweep (NEW)
│   ├── CasMountRuntime.{h,cpp}               fence, self-remount, watermark (NEW)
│   ├── CasManifestReader.{h,cpp}             readManifest + decode cache + locate (NEW)
│   ├── CasPlainObjects.{h,cpp}               namespace files + mountpoint objects (NEW)
│   ├── CasPartWriteTxn.{h,cpp}               one-part write transaction (ex-CasBuild)
│   ├── CasBlobUploader.{h,cpp}               blob upload/adopt/resurrect engine (NEW)
│   ├── CasRefProtocol.{h,cpp}                ref-table logic: replay + intake (merged)
│   ├── CasServerRoot.{h,cpp}                 mount claim protocol (+ merged slot, sweeper)
│   ├── CasPoolMeta.{h,cpp}                   pool identity admission (post-v3 logic half)
│   └── CasBlobMeta.{h,cpp}                   freshness sidecar lifecycle (post-v3 logic half)
│
├── Formats/                                  everything persisted: bytes AND keys
│   ├── README.md                             the v3 living registry (already exists)
│   ├── CasFormat.{h,cpp}  CasTextFormat.{h,cpp}  ...all v3 per-object format files...
│   └── CasLayout.{h,cpp}                     object-key schema (joins Formats — keys are
│                                             part of the persisted schema)
│
├── Backend/                                  the storage seam (token semantics)
│   ├── CasBackend.h                          the Backend contract (+ merged listing helpers)
│   ├── CasObjectStorageBackend.{h,cpp}       production backend over IObjectStorage
│   ├── CasInMemoryBackend.{h,cpp}            test/dev backend + fault injection
│   ├── CasInstrumentedBackend.{h,cpp}        ProfileEvents decorator
│   ├── CasRequestControl.{h,cpp}             conditional-write retry controller
│   └── CasProbe.{h,cpp}                      mount-time capability probe
│
└── Primitives/                               the vocabulary, zero outward dependencies
    ├── CasTypes.h                            RootNamespace, Token, BlobDigest, BlobRef,
    │                                         ManifestId, RefTxnId + hex helpers (merged)
    ├── CasBlobHasher.{h,cpp}                 pluggable content-hash machinery
    ├── CasXXH3.h                             isolated xxHash include wrapper
    └── CasEvent.{h,cpp}                      audit-event POD + sink (dependency-free after
                                              the toEventKind move)
```

`Core/` disappears entirely. Net file-count effect (counted on the pre-v3 tree, ~101 files; v3
shifts the base slightly): merges remove ~26 files, the decomposition adds ~10 — the added files
are per-subsystem components with their own locks (needed structure), the removed ones were
vocabulary fragments and single-consumer satellites (needless fragmentation).

### Include Direction Rule {#include-direction-rule}

Canonical layer order, recorded in the new root `README.md` (README rule only — no CI
enforcement):

```text
Primitives → Formats → Backend → Pool → Gc → Tools ≈ Parts → facade (top level)
```

A file may include only its own layer and layers to the left. `Tools` and `Parts` are siblings
with no edges between them. Named exceptions (deliberate, documented in the README): the staging
sweeper and `probeConditionalCopy` bypass `Backend` straight into `IObjectStorage`; `Backend` may
read `Formats` traits for the v3 step-8 provider-metadata mirror.

Post-v3 this order has exactly one violation left to fix in this refactoring: `CasEvent.h`
includes the envelope header only for the `toEventKind` mapper. Fix: move `toEventKind` into
`Formats/CasBlobEnvelopeFormat.h` (legal direction: `Formats` → `Primitives`); `CasEvent` becomes
dependency-free and lands in `Primitives/`.

## Merges {#merges}

Rule everywhere: **merge files, not classes** — type names and APIs do not change in the merge
phase (renames are phase 5). Composition order inside merged files: types/enums on top, then
codecs, then logic — the file reads top-down like a page. All merges stay within one layer, so the
direction rule is unaffected. Tests keep their per-test granularity; only include paths change.

| # | Result file | Absorbs | Evidence / motive |
|---|---|---|---|
| 1 | `Primitives/CasTypes.h` | `CasIds.h`, `CasToken.h`, `CasBlobDigest.h`, `CasBlobRef.h`, `CasManifestId.h`, `CasRefIds.h` | identity vocabulary: 6 micro-headers only ever read together; hex helpers stay here (their old target `CasCodecUtil` dies in v3) |
| 2 | `ContentAddressedTransaction` | `ContentAddressedWriteBuffers.{h,cpp}` | 9 of 9 buffer commits co-touch the transaction; buffers are created only by `writeFile` |
| 3 | `Pool/CasServerRoot` | `CasSingleWriterSlot.{h,cpp}`, `CasStagingSweeper.{h,cpp}` | slot base class has exactly one subclass (`MountLeaseKeeper`, lives here); sweeper is mount-scoped, ~45 lines |
| 4 | `Backend/CasBackend.h` | `CasBackendListing.h` | seam helpers live at the seam |
| 5 | `Gc/CasGcShardPlan` | `CasGcCursorKey.h` (39 lines) | same-domain micro-header |
| 6 | `Gc/CasGcScheduler.h` | `CasGcRoundLogRecord.h` | the POD record is the scheduler's output; the Interpreters decoupling survives (record keeps zero dependencies) |
| 7 | `Parts/PartFolderAccess` | `PartRefKey.h`, `PartFolderView.{h,cpp}`, `CachedPartFolderAccess.{h,cpp}` + `PartFolderValidate` (from the cache header) | key + value + cache of one mechanism, ~900 lines reading top-down |
| 8 | `Pool/CasRefProtocol` | `CasRefStateMachine.{h,cpp}`, `CasRefIntake.{h,cpp}` | the two pure-logic halves of the ref protocol (replay + intake planning); the two codecs stay in `Formats/` per the v3 physical rule |

Merges #7 and #8 also tidy internal declaration order (the only two merges whose diff is not
line-mechanical).

**Deliberately NOT merged:** the per-object `Formats/` files (settled v3 decision);
`ContentAddressedExchange.h` (external seam — must stay minimal); `CasXXH3.h` (build isolation);
`CasInMemoryBackend` vs `CasObjectStorageBackend` (test vs prod); `CasFsck` / `CasInspect` /
`CasDecommission` (independent operator verbs); `CasBlobInDegree` / `CasOrphanManifestSweep` into
`CasGc` (the engine is already 2430 lines); `CasPoolMeta` + `CasBlobMeta` (different objects);
watermark apart from fence (both are "epoch/incarnation", one heartbeat already carries them).

## Decomposition Of `CasStore` And `CasBuild` {#decomposition}

Cutting principle: **state/lock ownership boundaries** — every component owns its mutexes
wholesale; no lock is shared between components. Everything is a mechanical move: no lock changes
its covered state, no acquisition order changes, only the owning file/class changes.

Measured composition of `CasStore.cpp` (3156 lines, measured on the pre-v3 tree — v3 barely
touches these two files, so the proportions hold): ref-ledger ≈ 1730 (55%), mount lifecycle +
self-remount + fence + watermark ≈ 790 (25%), manifest read path ≈ 200, plain objects ≈ 130,
caches/misc ≈ 150. `CasBuild.cpp` (1237 lines): blob upload engine ≈ 540 (45%), W-protocol
choreography ≈ 550.

### `Pool/CasRefLedger` — The Ref Journal Subsystem {#casrefledger}

Takes from `CasStore`: the `ref_tables` map + `RefTableRuntime` wholesale, both mutexes
(`ref_queue_mutex`, per-table `state_mutex`), `allocateRefTxnId` + `next_ref_sequence`,
`ref_request_controller`; the append lane (`appendRefOps`, `runRefQueueLeader`, `flushRefBatch`,
wedge semantics); recovery + seal (`ensureRefTableRecovered`); snapshot publication
(`maybeScheduleSnapshotPublish`, `trySnapshotPublishOnce`, publish backoff pair); the
stale-precommit sweep (all four methods + its backoff pair); cache-budget eviction
(`enforceRefTableCacheBudget`); remount/shutdown coordination (`quiesceRefTablesForRemount`,
`refLanesSettledForRemount`, `drainRefLanesForShutdown`); the read side (`resolveRef`, `listRefs`,
`namespaceIsRemoved`, `observedNamespaceCleanupMarker`, `publishRemovedSnapshotNow`); the ref
lifecycle entry points (`dropRef`, `updateRefPayload`, `dropNamespace`); `wedgedRefLaneCount` and
the ~15 lane `*ForTest` seams. The carrier types `MutationScope`, `RootMutationOrigin`,
`RootMutationKind`, `Resolved`, `RefPayloadUpdate`, `DropNamespaceStats` move with it.

Environment via constructor — never a `Pool &` back-reference: `Backend &`, `const Layout &`, its
config slice, and callbacks `live_epoch`, `fence_ok`, `emit_event`,
`on_impossible_interference` (reaches up into the remount machinery), `boot_ms`, `wait_sleep`.

### `Pool/CasMountRuntime` — The Live Writer Incarnation {#casmountruntime}

Takes: `MountFence` + `mayMutate` / `tripMountLost` / `setMountDeadline` / `armMountFence` /
`bootMs` / `bootMsNow`; ownership of `MountLeaseKeeper`; the self-remount machinery
(`scheduleRemount` thread, its 5 atomics + 4 mutexes, `live_writer_epoch`,
`unclean_epoch_boundary_seen_at`, `ownsAndSawUncleanBoundaryFor`); the watermark/build-seq surface
(`process_epoch`, `next_build_seq`, `active_build_seqs`, `inflight_builds`, `minActive`,
`renewWatermarkOnce`, `allocateBuildSeq` / `retireBuildSeq`). Orchestration of `open` /
`openForDecommission` / `tryRemountOnce` (claim → ledger quiesce → fence re-arm ordering) stays in
`CasPool`; the mechanics live here.

### `Pool/CasManifestReader` And `Pool/CasPlainObjects` {#casmanifestreader-casplainobjects}

`CasManifestReader`: `readManifest` / `readManifestShared`, the token-gated byte-weighted decode
cache (`ManifestCacheKey`, weight functor, LRU), `locate`. `CasPlainObjects`: `casPutObject` /
`casGetObject` / `casRemoveObject` and the namespace-file + mountpoint-object surfaces; stateless
over `Backend &` + `const Layout &`.

### `CasPool` After The Extraction {#caspool-after}

The composition root (~400 lines): the `open` protocol, ownership of backend / pool meta / layout
and the four components, the dedup cache and admitted-algos cache (~30 lines each, stay as
members), the event sink, `startBuild`, `currentGcRound`, and thin delegating wrappers so the
external API (wiring, GC, tools, tests) is unchanged by the extraction phase.

### `Pool/CasBlobUploader` — Out Of `CasBuild` {#casblobuploader}

Takes the byte-delivery engine: the `putBlob` core, both `observeAndAdmit` overloads,
`uploadFromSource`, HEAD-first, the S3-staging promote/resurrect paths. The **decision** logic
(admit / adopt / resurrect choice, dep-set recording, the `promote` gate) stays in the transaction
class — it is transactional state; only the **execution** moves.

### Friendships Die {#friendships-die}

`friend class Build` and `friend class Gc` on `CasStore` are removed: the transaction and GC reach
the ref journal through `CasRefLedger`'s public surface (`appendRefOps` is already public; the
friendship only covered internals that move out).

### `PoolConfig` Slices {#poolconfig-slices}

`PoolConfig` (40+ fields) splits by owner: `RefLedgerConfig` (snapshot thresholds, both backoff
pairs, `ref_table_cache_bytes`, admission budgets), `MountConfig` (lease TTL, renew period,
`materialization_grace_ms`, `boot_ms_fn`, `wait_sleep_fn`), the rest stays on the pool. The public
`PoolConfig` remains as the aggregate of the slices — external callers unchanged.

**Deliberately NOT split:** `CasRefLedger` any further (publish/sweep/lane share `state_mutex` —
a state-boundary cut does not exist; sections within one file instead); `promote` with its
mini-spec comment (that IS the protocol); watermark apart from the fence.

## Renames {#renames}

**Boundary rule:** only C++ identifiers and file names change. Persisted bytes, key layouts,
log/error/event message texts, ProfileEvents/metric names, and protocol-spec vocabulary do NOT
change. In particular the protocol term "build" (`build_seq`, `buildSeq`, `BuildPrefix`, upload
stamps) survives — it is watermark-spec and durable-context vocabulary, not a class name.

| Old | New |
|---|---|
| `Cas::Store`, `StorePtr`, `CasStore.{h,cpp}` | `Cas::Pool`, `PoolPtr`, `Pool/CasPool.{h,cpp}` |
| `Cas::Build`, `BuildPtr`, `BuildInfo`, `Store::startBuild` | `Cas::PartWriteTxn`, `PartWriteTxnPtr`, `PartWriteInfo`, `Pool::beginPartWrite` |
| `gtest_cas_store.cpp`, `gtest_cas_build.cpp`, `gtest_cas_build_root_dangle.cpp` | `gtest_cas_pool.cpp`, `gtest_cas_part_write.cpp`, `gtest_cas_part_write_root_dangle.cpp` |

The namespace `DB::Cas` and the wiring naming convention (`ContentAddressed*`, `Part*` without the
`Cas` prefix) are untouched. The `Cas` file-name prefix stays tree-wide (grep-ability, matches the
v3 decision).

## Migration Phases And Gates {#migration-phases}

Precondition gate: codecs v3 steps 1–8 landed and green. Then:

| Phase | Content | Gate |
|---|---|---|
| 1. Merges | The 8 merges, in place, one commit per merge | build + full cas-gtest battery each |
| 2. Moves | `git mv` to the target tree (mv commits contain NO content edits, so `git log --follow` works), include-path sed (~70 gtests, 5 `clickhouse-disks` commands, wiring), `add_headers_and_sources` update in `src/CMakeLists.txt`, the `toEventKind` move, the new root `README.md`, path sweep in `docs/superpowers/cas/*.md` and any scripts; `Core/` dies | build + battery |
| 3. `CasStore` decomposition | Risk order: `CasPlainObjects` + `CasManifestReader` (warm-up) → `CasRefLedger` (the big one) → `CasMountRuntime`. One component = one commit. Friendships removed, `PoolConfig` sliced, `*ForTest` grouped per component. Thin delegates keep the external API stable | build + battery per commit |
| 4. `CasBuild` decomposition | `CasBlobUploader` extraction | build + battery |
| 5. Renames | `Store` → `Pool`, `Build` → `PartWriteTxn` (+ symmetric derivatives), 3 gtest file renames | build + battery |
| Final gate | CA-default stateless run + short ca-soak, time-driven phase-3 profile, exact duration set in the plan (exercises what gtests cannot: the lane under load, remount, GC concurrency) | both green |

An independent review pass (umbrella review) runs on the phase-3 diffs before the final gate —
concurrency surgery gets a second pair of eyes by policy.

## Invariants {#invariants}

The operational definition of "no logic changes", binding for every phase:

1. No mutex changes its covered state; no lock-acquisition order changes.
2. Construction/destruction order is preserved verbatim — the ordered teardown of `~Store` (drain
   lanes → keeper terminate → …) must be reproduced exactly when members move into components.
3. No changes to persisted bytes, object keys, log/error/event texts, ProfileEvents, metrics.
4. Move commits (`git mv`) contain no content edits; content commits contain no moves.
5. No drive-by improvements: anything worth improving goes to the backlog
   (`docs/superpowers/cas/BACKLOG.md`), never into these diffs.

## Risks And Mitigations {#risks-and-mitigations}

- **Concurrency surgery in phase 3** — the real risk. Mitigations: wholesale moves of state +
  locks with no reordering; per-component commits; the mandatory umbrella review on phase-3 diffs;
  the final short soak.
- **Conflicts with parallel work** — start at a quiet point right after v3 lands; one branch,
  sequential commits, no rebase (project rule).
- **History loss** — mv commits strictly separated from content commits.
- **Stale path references** — repo-wide grep for `MetadataStorages/ContentAddressed/Core` (code,
  docs, scripts, `.claude/`) at the end of phase 2.
- **Baseline drift** — plans re-verify the post-v3 tree before executing (see
  [Precondition](#precondition-codecs-v3)).

## Deferred To Plans {#deferred-to-plans}

- Member-by-member move inventories for phases 3–4 (the method groups above are authoritative;
  the field-level enumeration is plan work against the post-v3 tree).
- The exact include-sed mechanics and the new `add_headers_and_sources` directory list.
- Internal declaration order for merges #7 and #8.
- The root `README.md` initial content (phase-2 deliverable): layer map, include rule, named
  exceptions, reading order (`ContentAddressedMetadataStorage` → `route` / `PartRefKey` →
  `PartFolderAccess` → `CasPool` → `CasPartWriteTxn` → `CasGc`).
- The backlog list of improvements deliberately NOT done here (constructor config struct for the
  facade's ~25 positional parameters, flattening `SingleWriterSlot` into `MountLeaseKeeper` if no
  second subclass appears, retiring vestiges like `ShardIncarnation` and the unreachable
  `CasNs::Server`).
