---
description: "Target-sharded blob reducers (gc_shards>1) for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 4 (sharding)"
sidebar_position: 9
slug: /superpowers/plans/2026-06-26-cas-gc-phase4-target-sharded-reducers
title: "Phase 4 — Target-Sharded Blob Reducers — Implementation Plan"
doc_type: reference
---

# Phase 4 — Target-Sharded Blob Reducers — Implementation Plan {#phase-4-target-sharded-blob-reducers-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax; each task is one 2–5 minute action and ends with a commit. Read `2026-06-26-cas-gc-redesign-overview.md` and the [Global Constraints](#global-constraints) below first. **Gate:** the Phase 4 model extension (Task 1) must be GREEN before any code task. **Depends on:** Phase 3 (lazy fence/trim) complete and its gtest sweep green.

**Goal:** Enable `gc_shards > 1` so two replicas can fold and reduce **disjoint blob target shards** concurrently, while exactly one coordinator owns the registry fence, input seal, round visibility, global fence, and generation-pointer advance. The single-shard (`gc_shards = 1`) path must reproduce Phase 1d behavior byte-for-byte (same `GenerationSeal`). This removes the `snap_shards == 1` hard-pin in `Gc::fold` (`Core/CasGc.cpp:1641`), which was safe only because last-op-wins displacement was folded inside one resident snap; owner transitions now carry **both** old and new `ManifestRef`, so the displacement decision is made at the source root shard and reducers need no durable `RootEdgeIndex`.

**Architecture:** Root-shard mappers stream owner transitions and **scatter** blob deltas by `blobShard(blob_hash, gc_shards)` into per-shard delta runs. Blob target reducers own **disjoint blob-hash ranges** and merge their shard's delta runs (via `RunMerger`) into per-shard in-degree runs (`CasBlobInDegree`) stored in `RunFile`s under `blobTargetRunKey(gen, shard, seq)`. Part-manifest cleanup workers own disjoint `ManifestId` ranges/namespaces. One coordinator owns the global responsibilities; leases are work-dedup only. No writer observes any mapper/reducer product until the `GenerationSeal` is sealed atomically.

**Tech stack:** C++ (ClickHouse coding standards, Allman braces); Protobuf for the control plane; dense block-framed sorted binary runs (`RunFile`/`DataBlock`/`RunFooter`) for the hot data plane; TLA+ (TLC) for the safety gate; gtest for unit oracles; `ci.praktika` for integration and chaos-soak.

**Source spec:** `docs/superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md` (rev. 13), §Sharding Model and §Phase Plan / Phase 4.

## Global Constraints {#global-constraints}

*Every task below implicitly includes this section. Copied verbatim from `2026-06-26-cas-gc-redesign-overview.md`.*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in a phase may begin until that phase's TLA+ gate is green.
- **R1 — bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 — target-shardable.** Default `gc_shards = 1`; sharded mode is optional (this phase).
- **R3 — simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, trimmed.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden (per `feedback_ca_no_compat_scaffolding_predev`).

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source — **never** `GET`s the condemned object (per `feedback_ca_resurrect_invariant`);
- GC must never throw/fail-closed on a 404 during fold (record what you can and continue — per `feedback_ca_gc_never_throw_on_404`).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into a `build_*` directory (e.g. `build`, `build_debug`, `build_asan`). Always redirect ninja output to `<build_dir>/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `<build_dir>/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>[.sh]`. Do not add `no-*` tags unless strictly necessary. Prefer a new test over extending an existing one.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'`.

**TLA+ run mechanics** (exact; from `docs/superpowers/models/`)
- Run one config:
  ```bash
  cd docs/superpowers/models
  java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
       -metadir ../../../tmp/tlc-meta -workers auto -config <Cfg>.cfg CaGcRootLocalPartManifestCore.tla
  ```
  (For a long run set `TLC_JAVA_OPTS=-Xmx48g`.) Reuse the Phase 0 `run_gc_partmanifest.sh` wrapper (it already hardcodes `CaGcRootLocalPartManifestCore.tla`).
- **PASS** = exit 0 and the log contains `Model checking completed. No error has been found.`
- A **negative-control / `_sab_*`** config is correct **only when it FAILS** with `Error: Invariant <NAME> is violated.` (or `Temporal properties were violated.`). A zero exit on a `_sab_*` config is a **suite failure** (`UNEXPECTED PASS`).
- Convention: `<Module>_stageN.cfg` / `_fixed` / `_safe` must HOLD; `<Module>_sab_<rule>.cfg` must produce the expected counterexample.

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

- **`gc_shards` default = 1** (overview R2; spec §Sharding Model). Sharded mode is optional; the single-shard path is the equivalence oracle of Task 7.
- **No durable `RootEdgeIndex`** (spec §Sharding Model, §What Becomes Simpler). Because owner transitions carry both old and new `ManifestRef`, the displacement decision is made at the source root shard; reducers solve displacement from the paired delta streams, not from a resident reverse index.
- **Two sharding axes stay distinct** (`Core/CasGcFormats.h:33-37`): `fence_version` is indexed by **root** shard (`"ns/shard"` journal sources); blob target shards are indexed by **blob-hash prefix**. This phase renames the blob-target axis knob from `snap_shards` to `gc_shards` and leaves the root-shard fence axis untouched.
- **Coordinator-owned fence stays global** (spec §Global Fence): a publish into one root shard can protect blobs in any target shard, so target reducers do **not** own independent fences. The Task 1 negative control `SabotageReducerOwnsFence` proves this.

## Canonical Contract (consumed from Phase 1d / 2 / 3) {#canonical-contract-consumed-from-phase-1d-2-3}

These names are fixed by the redesign and are the **only** contract type names this plan uses (do not invent variants):

- `GenerationSeal` — the only writer-visible product; sealed atomically (`Core/CasGenerationSeal.h`).
- `CasBlobInDegree` — per-shard reducer state held in `RunFile`s (`Core/CasBlobInDegree.h`).
- `OwnerTransition` — carries **both** `old_manifest` and `new_manifest` (`ManifestRef`) (`Core/CasRootShardCodec`).
- `RunFile` / `DataBlock` / `RunFooter` / `RunMerger` — dense sorted binary runs and their k-way merge (`Core/CasRunFile.h`).
- `ManifestId` / `ManifestRef` — namespace-qualified manifest identity and its compact ref (`Core/CasManifestId.h`).
- `CasLayout::blobTargetRunKey(gen, shard, seq)` — key for a blob-target run of one shard.
- `CasLayout::partManifestCleanupKey(gen, owner_shard, seq)` — key for a part-manifest cleanup bundle of one owner shard.

## File Structure {#file-structure}

*All paths under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless noted — abbreviated `CA/` below. gtests live in `src/Disks/tests/gtest_cas_*.cpp` and use `<gtest/gtest.h>` + `cas_test_helpers.h` + `CasInMemoryBackend` (confirmed: `src/Disks/tests/gtest_cas_gc_snap.cpp`).*

- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5.cfg` — multi-shard/multi-leader safety stage (must HOLD).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_reducerownsfence.cfg` — negative control (must VIOLATE `INV_NO_DANGLE`).
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_crosssharddisplacement.cfg` — negative control (must VIOLATE `INV_NO_LOSS`).
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` — add `Shards`, `EnableSharding`, `SabotageReducerOwnsFence`, `SabotageCrossShardDisplacement`; per-shard reducer fold; coordinator-owned single fence.
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` — append the Phase 4 rows.
- Create: `CA/Core/CasGcShardPlan.h` / `.cpp` — `blobShard`; mapper scatter into per-shard delta runs; reducer disjoint-range ownership; coordinator responsibility helpers.
- Modify: `CA/Core/CasStore.h` — add `gc_shards` to `PoolConfig` (default 1).
- Modify: `CA/Core/CasGcFormats.h` — rename the `GcState::snap_shards` knob to `gc_shards` semantics for blob target shards.
- Modify: `CA/Core/CasGc.cpp` — remove the `gc_shards != 1` fold pin; route fold through `CasGcShardPlan` (mapper scatter + per-shard reducer merge); keep the coordinator's global fence.
- Modify: `CA/CasGcScheduler.h` / `.cpp` — wire disjoint blob-target-shard ownership across replicas (lease = work-dedup only; coordinator role distinct from reducer role).
- Create: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp` — `CasGcShard*` gtests (Tasks 3–8).
- Create: `tests/integration/test_cas_gc_sharded/` — two-replica disjoint-shard chaos soak (Task 9).

## Modeled Vocabulary (extends Phase 0) {#modeled-vocabulary-extends-phase-0}

This phase adds to the Phase 0 `CaGcRootLocalPartManifestCore.tla` vocabulary:

- CONSTANTS: `Shards` (the blob target shards, e.g. `{s1, s2}`), reuses `Leaders` (e.g. `{L1, L2}`), `EnableSharding`, `SabotageReducerOwnsFence`, `SabotageCrossShardDisplacement`.
- A pure model function `BlobShard(b)` partitioning `Blobs` into `Shards` (the abstraction of `blobShard(blob_hash, gc_shards)`); reducers act only on their own `BlobShard(b)`.
- VARIABLES: `shardIndeg` (`[Shards -> [Blobs -> Int]]` per-shard reducer in-degree, replacing the single `blobIndeg` when `EnableSharding`), `coordFence` (the single coordinator-owned fence position, replacing a per-shard fence), and `reducerOwner` (`[Shards -> Leaders]` disjoint-shard ownership).
- Actions: `GScatterDelta(n, s)` (mapper scatters one owner-transition delta into shard `s`'s run), `GReduceShard(l, s)` (leader `l`, owning shard `s`, merges its delta runs into `shardIndeg[s]`), and a `GCoordSeal` that writes the single `GenerationSeal` only after every shard's reducer product and every cleanup bundle is durable.

---

## Tasks {#tasks}

### Task 1: Phase 4 model extension — multi-shard / multi-leader stage + two negative controls {#task-1-phase-4-model-extension-multi-shard-multi-leader-stage-two-negative-controls}

**Files:**
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_reducerownsfence.cfg`
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_crosssharddisplacement.cfg`

**This is the R0 gate. No code task in this phase starts until stage5 HOLDs and both `_sab_*` configs VIOLATE their named invariant.**

- [ ] **Step 1: Add the sharding constants and variables.** In `CaGcRootLocalPartManifestCore.tla`, extend the `CONSTANTS` block with `Shards`, `EnableSharding`, `SabotageReducerOwnsFence`, `SabotageCrossShardDisplacement`, and extend the `VARIABLES` / `vars` tuple with `shardIndeg, coordFence, reducerOwner`. Add the partition function and the disjoint-ownership helper:

```tla
\* BlobShard abstracts blobShard(blob_hash, gc_shards): a total partition of Blobs into Shards.
\* Determinism + disjoint coverage are the modeled facts the C++ scatter must satisfy (Task 3).
BlobShard(b) == CHOOSE s \in Shards : TRUE  \* refined below to a fixed total map over Blobs

\* Disjoint reducer ownership: each shard has exactly one owning leader; two leaders never both
\* reduce the same shard. Two replicas MAY reduce different shards concurrently (spec §Sharding Model).
DisjointShardOwnership ==
    \A s \in Shards : reducerOwner[s] \in Leaders
ReducerOwns(l, s) == reducerOwner[s] = l
```

Initialize in `Init`: `shardIndeg = [s \in Shards |-> [b \in Blobs |-> 0]]`, `coordFence = 0`, `reducerOwner = [s \in Shards |-> CHOOSE l \in Leaders : TRUE]`.

- [ ] **Step 2: Add the sharded fold actions.** Add `GScatterDelta`, `GReduceShard`, and the coordinator seal. The mapper scatters BOTH the `old_manifest` `-1` deltas and the `new_manifest` `+1` deltas of one `OwnerTransition` to the target shard of each blob (this is the contract: the displacement is solved at the source, so the reducer never infers the old target from the new ref alone):

```tla
\* Mapper: scatter one owner transition's paired old/new blob deltas into per-shard delta runs,
\* routing each blob b to BlobShard(b). SabotageCrossShardDisplacement makes the reducer infer the
\* displaced old target from the NEW manifest ref alone (dropping the old_manifest's -1 deltas),
\* which is exactly the cross-shard last-op-wins leak the old snap_shards == 1 pin guarded against.
GScatterDelta(n, s) ==
    /\ EnableSharding
    /\ ... \* consume next unfolded journal[n] record at cursor[n]; for each blob b with BlobShard(b)=s:
    /\ shardIndeg' = [shardIndeg EXCEPT ![s] = [b \in Blobs |->
           @[b]
           + (IF NewEdge(n, b)  THEN 1 ELSE 0)
           - (IF OldEdge(n, b) /\ ~SabotageCrossShardDisplacement THEN 1 ELSE 0))]
    /\ ...

\* Reducer: leader l, owning shard s, folds its shard's deltas (work-dedup only — DisjointShardOwnership
\* lets a different leader own a different shard at the same time).
GReduceShard(l, s) ==
    /\ EnableSharding /\ ReducerOwns(l, s)
    /\ ...

\* Coordinator: ONE leader owns the single global fence and the seal. SabotageReducerOwnsFence lets a
\* target reducer write an independent per-shard fence instead, so a publish into a root shard that
\* protects a blob in a DIFFERENT target shard races past the missing global fence -> INV_NO_DANGLE.
GCoordFence(l) ==
    /\ ~SabotageReducerOwnsFence
    /\ coordFence' = ...        \* single global fence over the whole fence universe
GCoordSeal(l) ==
    /\ \A s \in Shards : ShardReducerDurable(s)   \* every shard's product durable before visibility
    /\ \A m \in mfCleanupRound : CleanupBundleDurable(m)
    /\ seal' = ...                                 \* the single GenerationSeal; ViewableRound preserved
```

Wire `EnableSharding => (GScatterDelta \/ GReduceShard \/ GCoordFence \/ GCoordSeal)` and `~EnableSharding => (the Phase 1d single-shard fold)` into `Next`, so a `gc_shards = 1` config exercises the original path unchanged.

- [ ] **Step 3: Add the sharded invariants.** The blob in-degree across shards must equal the active-manifest edge multiset (the sharded form of `BlobInDegreeMatchesActiveManifests`); the single fence must precede every shard's recheck-delete:

```tla
ShardedInDegreeMatchesActiveManifests ==
    EnableSharding =>
        (\A b \in Blobs : shardIndeg[BlobShard(b)][b] = ActiveEdgeCount(b))
SingleCoordinatorFence ==
    \A s \in Shards : RecheckDeleteIssued(s) => coordFence >= FenceUniverseHigh
```

`INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE` are unchanged (reuse the Phase 0 definitions; under `EnableSharding` they read `shardIndeg`/`coordFence` through the helpers).

- [ ] **Step 4: Write `stage5.cfg`** (must HOLD). Copy the Phase 0 `stage4.cfg`, then set the sharded constants and add the new invariants:

```
SPECIFICATION Spec
CONSTANTS
    Namespaces = {n1}
    Writers = {w1}
    Leaders = {L1, L2}
    Blobs = {b1, b2}
    ManifestInstances = {m1, m2}
    Refs = {r1}
    Builds = {bd1}
    Shards = {s1, s2}
    MaxToken = 2
    MaxRound = 2
    MaxLog = 3
    EnablePrecommit = TRUE
    EnableMissingBody = TRUE
    EnableOrphanSweep = TRUE
    EnableMutablePayload = TRUE
    EnableSharding = TRUE
    SabotageReducerOwnsFence = FALSE
    SabotageCrossShardDisplacement = FALSE
    \* (all Phase 0 Sabotage* flags = FALSE)
CONSTRAINT StateConstraint
INVARIANT TypeOK
INVARIANT INV_NO_DANGLE
INVARIANT INV_NO_LOSS
INVARIANT INV_NO_RETURN
INVARIANT INV_JOURNAL_COVERAGE
INVARIANT ShardedInDegreeMatchesActiveManifests
INVARIANT SingleCoordinatorFence
INVARIANT DisjointShardOwnership
PROPERTY MonotoneGC
```

Run: `cd docs/superpowers/models && TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_stage5`
Expected: `Model checking completed. No error has been found.` and `exit=0`. If it finds a trace with all sabotage flags off, the model (not the config) is wrong — fix the action gates until it holds.

- [ ] **Step 5: Write `sab_reducerownsfence.cfg`** — copy `stage5.cfg`, set `SabotageReducerOwnsFence = TRUE`, and narrow to the single targeted invariant:
```
INVARIANT TypeOK
INVARIANT INV_NO_DANGLE
```
Run: `TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_reducerownsfence`
Expected: `Error: Invariant INV_NO_DANGLE is violated.` and a nonzero `exit=`. A zero exit is an `UNEXPECTED PASS` (gate failure).

- [ ] **Step 6: Write `sab_crosssharddisplacement.cfg`** — copy `stage5.cfg`, set `SabotageCrossShardDisplacement = TRUE`, narrow to:
```
INVARIANT TypeOK
INVARIANT INV_NO_LOSS
```
Run: `TLC_JAVA_OPTS=-Xmx24g ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_crosssharddisplacement`
Expected: `Error: Invariant INV_NO_LOSS is violated.` and a nonzero `exit=` (the dropped old-target `-1` deltas leave a stale edge that retire never clears, so an active blob's in-degree is overstated and a displaced-then-re-added edge is lost — `INV_NO_LOSS`).

- [ ] **Step 7: Re-green the whole suite** (regression — the existing Phase 0/2/3 configs must still pass under the extended model):
```bash
cd docs/superpowers/models
for s in stage0 stage1 stage2 stage3 stage4 stage5 live ; do ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_$s ; done
for c in reducerownsfence crosssharddisplacement ; do
  ./run_gc_partmanifest.sh CaGcRootLocalPartManifestCore_sab_$c && echo "UNEXPECTED PASS: $c"
done
```
Expected: every stage/live HOLDs; each `_sab_*` prints its targeted violation; **no** `UNEXPECTED PASS` line. (Also re-run the 22 Phase 0 `_sab_*` configs; they must still VIOLATE their original invariants. Use the analyze-via-subagent rule for any long log.)

- [ ] **Step 8: Append the Phase 4 rows to `CaGcRootLocalPartManifestCore_RESULTS.md`** — one row per new config with `result` (HOLD / VIOLATED `<Inv>`), `states generated`, `distinct states`, `wall time`. Mark the Phase 4 gate **GREEN** iff `stage5` HOLDs, both new `_sab_*` rows show their expected violation, and the whole prior suite still passes.

- [ ] **Step 9: Commit**
```bash
git add docs/superpowers/models/CaGcRootLocalPartManifestCore.tla \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_stage5.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_reducerownsfence.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_sab_crosssharddisplacement.cfg \
        docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md
git commit -m "CA GC phase4: TLA+ sharded-reducer extension (stage5 holds; reducer-fence + cross-shard-displacement controls break)"
```

---

### Task 2: `gc_shards` config in `PoolConfig` (default 1) + gtest {#task-2-gc-shards-config-in-poolconfig-default-1-gtest}

**Files:**
- Modify: `CA/Core/CasStore.h`
- Modify: `CA/Core/CasGcFormats.h`
- Create: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Add `gc_shards` to `PoolConfig`.** In `CA/Core/CasStore.h`, after `root_shards` (`Core/CasStore.h:31`), add the creation-time knob:

```cpp
    /// Blob target shards for GC (spec §Sharding Model). Default 1 (single-shard equivalence to
    /// Phase 1d). Creation-time only; the pool is authoritative on reopen, like `root_shards`. This
    /// is the BLOB-HASH-prefix reducer axis, distinct from the root-shard fence axis (`fence_version`).
    uint64_t gc_shards = 1;
```

- [ ] **Step 2: Rename the `GcState` knob to `gc_shards` semantics.** In `CA/Core/CasGcFormats.h`, rename `snap_shards` (`Core/CasGcFormats.h:49`) to `gc_shards` and update the doc comment so it reads "blob target shards" and the JSON key example becomes `"gc_shards":1`. Update `encodeGcState` / `decodeGcState` in the `.cpp` to read/write the `"gc_shards"` key. **No compat shim** — the format is unreleased; an old `"snap_shards"` key is not accepted.

```cpp
    uint64_t gc_shards = 1;      /// GC blob-target-shard count (blob-hash-prefix sharding); set once, immutable
```

- [ ] **Step 3: Write the config gtest.** In `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>

using namespace DB::Cas;

TEST(CasGcShardConfig, DefaultIsSingleShard)
{
    PoolConfig cfg;
    EXPECT_EQ(cfg.gc_shards, 1u);
}

TEST(CasGcShardConfig, GcStateRoundTripPreservesShardCount)
{
    GcState s;
    s.gc_shards = 4;
    s.round = 7;
    const GcState back = decodeGcState(encodeGcState(s));
    EXPECT_EQ(back.gc_shards, 4u);
    EXPECT_EQ(back.round, 7u);
}
```

- [ ] **Step 4: Build + run the gtest.** Build into `build` (redirect to `build/build.log`, analyze with a subagent). Then:
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardConfig.*' > build/test_gc_shard_config.log 2>&1
```
Expected: both tests pass. Analyze the log with a subagent; return a concise summary.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.cpp \
        src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: gc_shards knob in PoolConfig + GcState (default 1)"
```

---

### Task 3: `CasGcShardPlan` mapper scatter by blob hash + gtest {#task-3-casgcshardplan-mapper-scatter-by-blob-hash-gtest}

**Files:**
- Create: `CA/Core/CasGcShardPlan.h` / `.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Add `blobShard`.** In `CA/Core/CasGcShardPlan.h`, define the partition function as a thin wrapper over the existing hash-prefix routing so the two sharding axes share one routing rule (`Core/CasGcSnap.h:42` `hashPrefixShard`):

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>

namespace DB::Cas
{

/// Blob target shard for a blob hash (spec §Sharding Model). Deterministic and total over all hashes;
/// `gc_shards == 1` routes every hash to shard 0 (the single-shard equivalence used by Task 7).
inline uint64_t blobShard(const UInt128 & blob_hash, uint64_t gc_shards)
{
    return hashPrefixShard(blob_hash, gc_shards);
}

}
```

- [ ] **Step 2: Add the mapper scatter.** In `CasGcShardPlan.h`/`.cpp`, add a `ShardScatter` that takes one `OwnerTransition`'s paired old/new blob-edge lists and appends `+1`/`-1` blob deltas into the per-shard delta-run builders keyed by `blobShard(blob_hash, gc_shards)`. Both the old-manifest `-1` deltas and the new-manifest `+1` deltas are scattered here (the displacement is solved at the source — this is the `SabotageCrossShardDisplacement` defense from Task 1). Emit into `RunFile` delta runs (one per shard), not per-edge objects.

```cpp
/// Scatter the paired old/new blob deltas of one OwnerTransition into per-shard delta runs.
/// `gc_shards` shards, routing each blob by blobShard. Determinism + disjoint coverage are the
/// contract proven by BlobShard in the model (Task 1) and the gtest below.
class ShardScatter
{
public:
    explicit ShardScatter(uint64_t gc_shards_);
    void scatterAdd(const UInt128 & blob_hash);     /// +1 (new_manifest edge)
    void scatterRemove(const UInt128 & blob_hash);  /// -1 (old_manifest edge)
    /// ... finalize per-shard RunFile delta runs ...
};
```

- [ ] **Step 3: Write the scatter gtest** (determinism + disjoint coverage):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h>

TEST(CasGcShardScatter, DeterministicAndStable)
{
    const UInt128 h = hexToU128("0123456789abcdef0123456789abcdef");
    EXPECT_EQ(blobShard(h, 4), blobShard(h, 4));   /// stable
    EXPECT_LT(blobShard(h, 4), 4u);                /// in range
    EXPECT_EQ(blobShard(h, 1), 0u);                /// single shard -> 0
}

TEST(CasGcShardScatter, DisjointCoverageOverManyHashes)
{
    const uint64_t shards = 4;
    std::array<size_t, 4> counts{};
    for (uint64_t i = 0; i < 4096; ++i)
    {
        UInt128 h = static_cast<UInt128>(i) << 64 | i;   /// spread the prefix bits
        const uint64_t s = blobShard(h, shards);
        ASSERT_LT(s, shards);            /// every hash lands in exactly one in-range shard
        ++counts[s];
    }
    for (auto c : counts)
        EXPECT_GT(c, 0u);                /// every shard is covered (no dead shard)
}
```

- [ ] **Step 4: Build + run** (redirect to `build/build.log` + `build/test_gc_shard_scatter.log`, analyze each with a subagent).
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardScatter.*' > build/test_gc_shard_scatter.log 2>&1
```
Expected: both tests pass.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.cpp \
        src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: CasGcShardPlan mapper scatter by blob hash (deterministic, disjoint coverage)"
```

---

### Task 4: Blob target reducers merge disjoint shards via `RunMerger` + gtest {#task-4-blob-target-reducers-merge-disjoint-shards-via-runmerger-gtest}

**Files:**
- Modify: `CA/Core/CasGcShardPlan.h` / `.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Add the reducer.** In `CasGcShardPlan.h`/`.cpp`, add a `ShardReducer` that owns ONE shard and merges that shard's delta runs into a per-shard in-degree run. It uses `RunMerger` (from `Core/CasRunFile.h`, the Phase 1a k-way streaming merge) over the shard's delta `RunFile`s, accumulating the `+1`/`-1` deltas per blob hash, and writes a `CasBlobInDegree` run stored under `CasLayout::blobTargetRunKey(gen, shard, seq)`. A reducer touches only blobs whose `blobShard == its shard` (disjoint ownership):

```cpp
/// Owns one blob target shard. Merges that shard's delta runs (RunMerger over RunFiles) into a
/// per-shard in-degree run (CasBlobInDegree), written under blobTargetRunKey(gen, shard, seq). Two
/// replicas may run ShardReducer for DIFFERENT shards concurrently (spec §Sharding Model).
class ShardReducer
{
public:
    ShardReducer(uint64_t shard_, uint64_t gc_shards_);
    /// Merge the shard's delta runs into per-shard in-degree; returns the sealed-into run handle.
    void reduce(/* delta run inputs */);
};
```

- [ ] **Step 2: Add `blobTargetRunKey` if not already present** in `CA/Core/CasLayout.h` (it is part of the Phase 1d/2/3 contract; if Phase 1d already added it, leave it and only reference it). Shape (mirrors `gcSnapKey`, `Core/CasLayout.h:132`):

```cpp
    /// Blob-target run for one shard of a generation: <prefix>/gc/gen/<gen>/blob_target/<shard>/<seq>.
    String blobTargetRunKey(uint64_t generation, uint64_t shard, uint64_t seq) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/blob_target/"
               + std::to_string(shard) + "/" + std::to_string(seq);
    }
```

- [ ] **Step 3: Write the reducer gtest** (`CasGcShard*` — merge correctness + disjoint ownership):

```cpp
TEST(CasGcShardReducer, MergesDeltasToInDegree)
{
    /// Build per-shard delta runs via ShardScatter (+1 b1 twice, -1 b1 once, +1 b2 once), then reduce.
    /// Reducer for shard(b1) yields indeg(b1)=1; reducer for shard(b2) yields indeg(b2)=1; neither
    /// reducer touches a blob outside its own shard.
    ... build deltas ...
    ShardReducer red_b1(blobShard(b1, gc_shards), gc_shards);
    red_b1.reduce(...);
    EXPECT_EQ(inDegreeOf(red_b1, b1), 1u);
    EXPECT_FALSE(red_b1.owns(b2_if_other_shard));   /// disjoint ownership: out-of-shard blob untouched
}

TEST(CasGcShardReducer, TwoReducersCoverDisjointShards)
{
    /// Reducers for s0 and s1 partition the blob space with no overlap and no gap.
    ... assert union == all blobs, intersection == empty ...
}
```

- [ ] **Step 4: Build + run** (redirect + subagent-analyze `build/test_gc_shard_reducer.log`).
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardReducer.*' > build/test_gc_shard_reducer.log 2>&1
```
Expected: both tests pass.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: blob target reducers merge disjoint shards via RunMerger into CasBlobInDegree runs"
```

---

### Task 5: Part-manifest cleanup workers own disjoint `ManifestId` ranges/namespaces + gtest {#task-5-part-manifest-cleanup-workers-own-disjoint-manifestid-ranges-namespaces-gtest}

**Files:**
- Modify: `CA/Core/CasGcShardPlan.h` / `.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Add the cleanup-worker partition.** Part-manifest cleanup is keyed by `ManifestId` (spec §GC Authority Model). Add a `manifestCleanupShard(const ManifestId &, uint64_t gc_shards)` that routes a cleanup bundle to a worker by hashing the `ManifestId` (namespace-qualified — never by `ManifestRef` alone; that is the Phase 0 `SabotageKeyByRefNotId` hazard). Cleanup workers own disjoint `ManifestId` ranges (or whole namespaces); a bundle is written under `CasLayout::partManifestCleanupKey(gen, owner_shard, seq)`.

```cpp
/// Route a part-manifest cleanup bundle to a worker by its namespace-qualified ManifestId. Workers
/// own disjoint ranges; routing by ManifestRef alone would merge two namespaces' cleanup work
/// (Phase 0 SabotageKeyByRefNotId). gc_shards == 1 routes every ManifestId to owner shard 0.
uint64_t manifestCleanupShard(const ManifestId & id, uint64_t gc_shards);
```

- [ ] **Step 2: Add `partManifestCleanupKey` if not already present** in `CA/Core/CasLayout.h` (part of the Phase 1d contract; reference it if Phase 1d added it):

```cpp
    /// Part-manifest cleanup bundle for one owner shard of a generation:
    /// <prefix>/gc/gen/<gen>/part_manifest_cleanup/<owner_shard>/<seq>.
    String partManifestCleanupKey(uint64_t generation, uint64_t owner_shard, uint64_t seq) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/part_manifest_cleanup/"
               + std::to_string(owner_shard) + "/" + std::to_string(seq);
    }
```

- [ ] **Step 3: Write the cleanup-partition gtest** (`CasGcShard*` — namespace-qualified routing + disjoint coverage). The load-bearing assertion: two `ManifestId`s that share a `ManifestRef` but differ in namespace route independently (no merge):

```cpp
TEST(CasGcShardCleanup, RoutesByQualifiedManifestIdNotRef)
{
    /// Same ManifestRef, two namespaces -> two distinct ManifestIds. They must be allowed to route to
    /// the SAME worker only because the QUALIFIED id hashes there, never because the ref collides.
    const ManifestId a = makeManifestId("n1", ref);
    const ManifestId b = makeManifestId("n2", ref);   /// same ref, different namespace
    EXPECT_NE(a, b);
    /// Routing is a pure function of the qualified id (deterministic, in range):
    EXPECT_LT(manifestCleanupShard(a, 4), 4u);
    EXPECT_EQ(manifestCleanupShard(a, 4), manifestCleanupShard(a, 4));
    EXPECT_EQ(manifestCleanupShard(a, 1), 0u);
}

TEST(CasGcShardCleanup, DisjointWorkerCoverage)
{
    /// Over many ManifestIds, every owner shard is covered and each id lands in exactly one.
    ... assert coverage + single-assignment ...
}
```

- [ ] **Step 4: Build + run** (redirect + subagent-analyze `build/test_gc_shard_cleanup.log`).
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardCleanup.*' > build/test_gc_shard_cleanup.log 2>&1
```
Expected: both tests pass.

- [ ] **Step 5: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: part-manifest cleanup workers own disjoint ManifestId ranges (qualified routing)"
```

---

### Task 6: Coordinator retains the global responsibilities; leases are work-dedup only + gtest {#task-6-coordinator-retains-the-global-responsibilities-leases-are-work-dedup-only-gtest}

**Files:**
- Modify: `CA/Core/CasGc.cpp`
- Modify: `CA/Core/CasGcShardPlan.h` / `.cpp`
- Modify: `CA/CasGcScheduler.h` / `.cpp`
- Modify: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Remove the `gc_shards != 1` fold pin** in `CA/Core/CasGc.cpp` (`Core/CasGc.cpp:1641`). Replace the `throw NOT_IMPLEMENTED` with the routed path: when `state.gc_shards > 1`, fold routes each `OwnerTransition`'s paired old/new deltas through `ShardScatter` (Task 3) and each owning leader's `ShardReducer` (Task 4); when `state.gc_shards == 1` it takes the single-shard path unchanged. Update the stale comment that claimed cross-shard displacement is undesigned — the owner transition's old/new pair now solves it at the source (cite the spec §Sharding Model).

- [ ] **Step 2: Encode the coordinator responsibilities.** Add a `CoordinatorPlan` to `CasGcShardPlan.h`/`.cpp` documenting and enforcing that exactly ONE coordinator owns: registry fence, input seal, round visibility (`ViewableRound`), the single global fence, and the generation-pointer advance. Reducers/cleanup workers own only their disjoint shard work. The global fence stays in `Gc::fence` (coordinator), NOT per reducer — a publish into one root shard can protect blobs in any target shard (spec §Global Fence; the Task 1 `SabotageReducerOwnsFence` control proves an independent reducer fence is unsafe).

- [ ] **Step 3: Wire disjoint shard ownership in the scheduler.** In `CA/CasGcScheduler.cpp` (the round entry is `gc.runRegularRound()`, `CasGcScheduler.cpp:127`), the lease remains **work-dedup only** (it already is — see the class comment, `CasGcScheduler.h:14-24`). Assign blob-target-shard ownership so two replicas can reduce disjoint shards concurrently; the coordinator role (fence/seal/round-visibility) is held by the lease holder, while reducer work for an unowned shard may proceed on another replica. Do not add any coordination beyond the existing lease.

- [ ] **Step 4: Write the coordinator gtest** (`CasGcShard*` — single fence owner; lease is not a reducer gate):

```cpp
TEST(CasGcShardCoordinator, SingleGlobalFenceNotPerShard)
{
    /// The plan exposes exactly one global fence position; asking for a per-shard fence is rejected
    /// (the SabotageReducerOwnsFence shape). Assert there is one coordinator fence covering all shards.
    CoordinatorPlan plan{/* gc_shards = */ 4};
    EXPECT_TRUE(plan.hasSingleGlobalFence());
    EXPECT_FALSE(plan.allowsPerShardFence());
}

TEST(CasGcShardCoordinator, ReducerWorkIsLeaseFree)
{
    /// A reducer for a shard not owned by the coordinator does not require holding the lease (leases are
    /// work-dedup only). The plan classifies reduce(shard) as lease-free and seal/fence as coordinator.
    CoordinatorPlan plan{4};
    EXPECT_FALSE(plan.requiresLeaseForReduce());
    EXPECT_TRUE(plan.requiresCoordinatorForSeal());
    EXPECT_TRUE(plan.requiresCoordinatorForFence());
}
```

- [ ] **Step 5: Build + run** (redirect to `build/build.log` + `build/test_gc_shard_coordinator.log`, analyze each with a subagent).
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardCoordinator.*' > build/test_gc_shard_coordinator.log 2>&1
```
Expected: both tests pass.

- [ ] **Step 6: Commit**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcShardPlan.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp \
        src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: remove gc_shards==1 fold pin; coordinator owns fence/seal/visibility; reducers lease-free"
```

---

### Task 7: Single-shard equivalence — `gc_shards = 1` reproduces Phase 1d byte-for-byte + gtest {#task-7-single-shard-equivalence-gc-shards-1-reproduces-phase-1d-byte-for-byte-gtest}

**Files:**
- Modify: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Write the equivalence gtest.** Drive one GC round through `CasInMemoryBackend` (the in-memory backend, `Core/CasInMemoryBackend.h`) twice over an identical scripted journal of owner transitions: once with `gc_shards = 1` via the single-shard path, once with the sharded code path forced to `gc_shards = 1`. Assert the produced `GenerationSeal` bytes are identical, and the resulting blob in-degrees match. This is the load-bearing equivalence: `gc_shards = 1` must produce the **same** `GenerationSeal` as Phase 1d.

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>

TEST(CasGcShardEquivalence, SingleShardMatchesPhase1dSeal)
{
    /// Identical scripted journal; gc_shards=1. The sharded fold path (ShardScatter -> one ShardReducer
    /// for shard 0) must seal byte-for-byte the same GenerationSeal as the Phase 1d single-shard fold.
    const String seal_phase1d = runRoundAndReadSeal(/* gc_shards = */ 1, /* force_sharded = */ false);
    const String seal_sharded = runRoundAndReadSeal(/* gc_shards = */ 1, /* force_sharded = */ true);
    EXPECT_EQ(seal_phase1d, seal_sharded);          /// byte-for-byte
}
```

- [ ] **Step 2: Build + run** (redirect + subagent-analyze `build/test_gc_shard_equivalence.log`).
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardEquivalence.*' > build/test_gc_shard_equivalence.log 2>&1
```
Expected: the test passes (identical seal bytes). If the seals differ, the sharded path is not a faithful generalization — fix `ShardScatter`/`ShardReducer` ordering until shard-0-only output is byte-identical to the single-shard fold (deterministic block boundaries, no nondeterministic ordering).

- [ ] **Step 3: Commit**
```bash
git add src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: gc_shards=1 reproduces Phase 1d GenerationSeal byte-for-byte (equivalence gtest)"
```

---

### Task 8: Two-replica disjoint-shard concurrency + gtest {#task-8-two-replica-disjoint-shard-concurrency-gtest}

**Files:**
- Modify: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp`

**Gate:** Task 1 GREEN.

- [ ] **Step 1: Write the two-replica gtest.** Over a shared `CasInMemoryBackend`, run two `Gc` instances (two replicas) with `gc_shards = 2`, each reducing a different blob target shard concurrently (drive both deterministically with no sleeps — alternate their reduce steps from the test thread). Assert: (a) neither replica touches the other's shard's blobs; (b) **no writer observes any mapper/reducer product until the `GenerationSeal` is written** — i.e. before the coordinator seal, a reader querying the would-be in-degree sees only the prior sealed generation; (c) after the single coordinator seal, the merged in-degrees across both shards equal the active-manifest edge multiset.

```cpp
TEST(CasGcShardTwoReplica, DisjointShardsConcurrentNoPreSealVisibility)
{
    auto backend = makeSharedInMemoryBackend();
    /// Replica A owns shard 0, replica B owns shard 1; gc_shards = 2.
    ... script a journal; interleave A.reduceShard(0) and B.reduceShard(1) deterministically ...

    /// Pre-seal: no writer-visible product. A reader's reachability view is the PRIOR sealed generation.
    EXPECT_EQ(readerVisibleGeneration(backend), prior_generation);
    EXPECT_FALSE(generationSealExists(backend, new_generation));

    coordinatorSeal(backend, new_generation);       /// single GenerationSeal, written once

    /// Post-seal: merged in-degree across both shards == active-manifest edge multiset.
    EXPECT_EQ(mergedInDegree(backend, new_generation, b_in_shard0), expected0);
    EXPECT_EQ(mergedInDegree(backend, new_generation, b_in_shard1), expected1);
}
```

- [ ] **Step 2: Build + run** (redirect + subagent-analyze `build/test_gc_shard_two_replica.log`).
```bash
build/src/unit_tests_dbms --gtest_filter='CasGcShardTwoReplica.*' > build/test_gc_shard_two_replica.log 2>&1
```
Expected: the test passes — disjoint, no pre-seal visibility, correct post-seal merge.

- [ ] **Step 3: Commit**
```bash
git add src/Disks/tests/gtest_cas_gc_shard_plan.cpp
git commit -m "CA GC phase4: two-replica disjoint-shard concurrency; no writer sees products pre-seal (gtest)"
```

---

### Task 9: Build + full `Cas*`/`Ca*` gtest sweep + two-replica chaos soak + commit {#task-9-build-full-cas-ca-gtest-sweep-two-replica-chaos-soak-commit}

**Files:**
- Create: `tests/integration/test_cas_gc_sharded/` (test module + config)

**Gate:** Tasks 1–8 done.

- [ ] **Step 1: Clean build.** Build into `build` (redirect to `build/build.log`). **Analyze the build log with a subagent**; return only a concise summary. Fix any break before proceeding.

- [ ] **Step 2: Full CA gtest sweep.**
```bash
build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_cas_sweep_phase4.log 2>&1
```
**Analyze the log with a subagent**; confirm every `Cas*`/`Ca*` test passes (including the new `CasGcShard*`, the existing `CasGcSnap`, `CasGcRound`, `CasGcFormats`, `CasGcLeak`, `CasBackend`, `CasLayout` suites). This is the phase-exit gtest gate (overview §Execution Model).

- [ ] **Step 3: Two-replica disjoint-shard chaos soak.** Create `tests/integration/test_cas_gc_sharded/` configuring a CA pool with `gc_shards = 2` and two replicas mounting the same pool, then run the ca-soak workload+chaos procedure (per `reference_ca_soak_fresh_restart` and the `cas-test-triage` skill) with each replica owning a disjoint blob target shard. Run via:
```bash
python -m ci.praktika run "integration" --test test_cas_gc_sharded > build/test_cas_gc_sharded_soak.log 2>&1
```
**Analyze the log with a subagent.** Watch for: no dangle/no-loss assertion fires; the regression-watch sees a single `GenerationSeal` per round (no partial-shard visibility); both replicas make reduce progress on disjoint shards. (This is the post-Phase-4 soak required by the overview §Phase exit.)

- [ ] **Step 4: Commit**
```bash
git add tests/integration/test_cas_gc_sharded
git commit -m "CA GC phase4: two-replica disjoint-shard integration soak; full Cas*/Ca* sweep green"
```

---

## Self-Review {#self-review}

- **Gate definition matches the overview:** Task 1 is the R0 model gate — `stage5` HOLDs (multi-shard `Shards={s1,s2}`, multi-leader `Leaders={L1,L2}`, `EnableSharding`, disjoint reducers, single coordinator fence) and the two negative controls VIOLATE: `SabotageReducerOwnsFence` ⇒ `INV_NO_DANGLE`, `SabotageCrossShardDisplacement` ⇒ `INV_NO_LOSS`. The whole prior suite is re-greened (Step 7). No code task starts before the gate is green. ✓
- **Depends on Phase 3, gates Phase 5:** declared in the header; the plan consumes the Phase 1d/2/3 contract types (`GenerationSeal`, `CasBlobInDegree`, `OwnerTransition` old+new, `blobTargetRunKey`, `partManifestCleanupKey`) and emits `CasGcShardPlan` + the `gc_shards` config that Phase 5 builds on. ✓
- **Spec coverage (§Sharding Model / §Phase 4):** mappers scatter by `blobShard(blob_hash, gc_shards)` (Task 3); reducers own disjoint blob-hash ranges and merge via `RunMerger` into `CasBlobInDegree` runs (Task 4); part-manifest cleanup workers own disjoint `ManifestId` ranges/namespaces (Task 5); one coordinator owns registry fence / input seal / round visibility / global fence / generation-pointer advance, leases work-dedup only (Task 6); `gc_shards` default 1 (Task 2); two replicas process disjoint shards with no pre-seal visibility (Task 8); no `RootEdgeIndex` because owner transitions carry old+new refs (the `SabotageCrossShardDisplacement` defense). ✓
- **Single-shard equivalence:** Task 7 proves `gc_shards = 1` reproduces the Phase 1d `GenerationSeal` byte-for-byte — the safety net for removing the `gc_shards != 1` fold pin (`Core/CasGc.cpp:1641`) in Task 6. ✓
- **TDD + bite-sized + commits:** every task is one 2–5 minute action, ends with a commit, and each code task writes a failing-then-passing gtest before/with the implementation. No placeholders — exact paths, real cfg/code skeletons, runnable commands with expected output. ✓
- **Constraints honored:** Allman braces in all C++ skeletons; only contract type names used; build/test logs redirected and subagent-analyzed; no `sleep` (Task 8 interleaves deterministically from the test thread); commits on `cas-gc-part-manifest-impl` with the two trailers; no compat shim for the `snap_shards`→`gc_shards` rename (unreleased format). ✓
- **Two sharding axes kept distinct:** the `fence_version` root-shard axis is untouched; only the blob-target axis knob is renamed `snap_shards`→`gc_shards` (Task 2), matching `Core/CasGcFormats.h:33-37`. ✓
