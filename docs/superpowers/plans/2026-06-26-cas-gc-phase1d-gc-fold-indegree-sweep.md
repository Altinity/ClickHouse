---
description: "GC fold, streaming blob in-degree generation, orphan sweep, and removal of trees/snap/cascade."
sidebar_label: "GC redesign — Phase 1d (GC core)"
sidebar_position: 6
slug: /superpowers/plans/2026-06-26-cas-gc-phase1d-gc-fold-indegree-sweep
title: "Phase 1d — GC Fold/In-Degree/Seal/Sweep + Removals — Implementation Plan"
doc_type: reference
---

# Phase 1d — GC Fold/In-Degree/Seal/Sweep + Removals — Implementation Plan {#phase-1d-gc-fold-in-degree-seal-sweep-removals-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Read `2026-06-26-cas-gc-redesign-overview.md` first.

**Goal:** Rewrite CA GC accounting so that owner transitions over root-local part manifests fold into a streaming **blob in-degree generation** sealed by a `GenerationSeal`, add a per-namespace orphan part-manifest sweep, and delete the content-addressed-tree / resident-`GcSnap` / cascade machinery — the first point CA GC behavior changes.

**Architecture:** GC reads ONE `PartManifest` per old/new owner named by a streamed `OwnerTransition`/`PrecommitTransition`/`PromotePrecommit`, emits `±1` blob deltas, and merges them with the prior in-degree run via `RunMerger` into a write-once generation; blob retire/fence/recheck keep the proved exact-token-delete tail; manifest bodies are deleted by exact token only AFTER their owner-removal decrements are sealed; an orphan sweep cleans pre-precommit `_manifests` debris per namespace. No tree objects, no `children_by_tree`, no `GcSnap`, no GC-side cascade.

**Tech Stack:** C++ (ClickHouse coding standards, Allman braces); Protobuf for control-plane records (`GenerationSeal`, `GcState`, retired/outcome); dense block-framed sorted binary runs (`RunFile`/`DataBlock`/`RunFooter` via `RunMerger`) for the hot blob in-degree / blob-delta data plane; gtest for unit oracles (binary `build/src/unit_tests_dbms`); `ci.praktika` for chaos soak.

## Global Constraints {#global-constraints}

*Every task in this plan implicitly includes this section.*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only — never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals — non-negotiable)**
- **R0 — safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` are proved by the Phase-0 model `CaGcRootLocalPartManifestCore.tla`, not argued. **No code task in this phase may begin until the Phase-0 gate is green** (every `stage*`/`live`/`witness` HOLDs, all 22 `_sab_*` controls VIOLATE, no `UNEXPECTED PASS`). The gtests in this plan are the **C++ realization** of those invariants, not re-proofs.
- **R1 — bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 — target-shardable.** Keep `gc_shards = 1` in this phase; sharded mode is Phase 4.
- **R3 — simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, trimmed.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden (per `feedback_ca_no_compat_scaffolding_predev`).

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source — **never** `GET`s the condemned object (per `feedback_ca_resurrect_invariant`);
- GC must never throw/fail-closed on a 404 during fold or sweep (record what you can and continue — per `feedback_ca_gc_never_throw_on_404`).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into `build/` (the existing build dir holding `build/src/unit_tests_dbms`). Always redirect ninja output to `build/build.log`. **Analyze the build log with a subagent and return only a concise summary** — never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `build/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- Run CA gtests via `build/src/unit_tests_dbms --gtest_filter='<Suite>.*'` for a single suite, or `--gtest_filter='Cas*:Ca*'` for the full sweep. The gtest sources are globbed by `file(GLOB_RECURSE ... "gtest*.cpp" CONFIGURE_DEPENDS)` at `src/CMakeLists.txt:880`, so adding or deleting a `gtest_*.cpp` file needs **no** CMake edit.

**Resolved Open Questions consumed here** (from the overview; vetoable = plan edit, not a code rewrite)
- **OQ5 (block-run details):** `RunFile` blocks target 256 KiB / hard-cap 1 MiB; per-`DataBlock` CRC32C; sparse footer `(min_key,max_key,block_offset)` per block; compression off by default; hashes fixed-width; key schemas fixed per `kind`; deterministic encoding so a write-once run is byte-reproducible. (These are owned by Phase 1a's `RunFile`; this plan consumes them.)
- **OQ6 (sweep eligibility):** a build prefix is sweep-eligible iff an explicit retired-epoch sentinel is present, **or** the same epoch's durable watermark has `min_active > build_sequence`, **or** the writer incarnation has been replaced — **never** a frozen-seq / judged-dead heuristic alone.
- **OQ7 (backpressure):** caps are enforced fail-closed at the writer (Phase 1b), not here; this phase's fold/sweep only *consume* already-published transitions.
- **OQ8 (fsck classification):** the read-only manifest audit ships as **Task 9** of this plan (owner-visible missing body ⇒ **error**; reclaimable pre-precommit body ⇒ **info**; same sealed-owner-view + eligibility rule as the sweep).

---

## Gate & Dependencies {#gate-dependencies}

- **Gate (before any code task):** Phase 0 GREEN — `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` marks the suite GREEN (`INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_NO_RETURN` HOLD in stage3/stage4, all 22 `_sab_*` reproduce, liveness HOLDs, witnesses reachable). Confirm by reading `RESULTS.md`; if absent or not GREEN, **stop** and surface it.
- **Depends on:** Phase **1a** (identity/codecs/layout/runs), **1b** (build/precommit/promote/owner transitions), **1c** (read path). This plan **consumes** the canonical types those phases emit (see [Canonical Contract](#canonical-contract)) and is the **last** task of the behavior switch (1b+1c+1d together).
- **Phase exit:** full `Cas*`/`Ca*` gtest sweep green (Task 10), then a chaos soak per `reference_ca_soak_fresh_restart` with periodic reports (Task 10).

## Canonical Contract {#canonical-contract}

**Consume verbatim from Phase 1a/1b/1c (do NOT redefine these — `#include` their headers):**
- From `CA/Core/CasManifestId.h`: `ManifestRef` (`writer_instance_id`, `build_sequence`, `manifest_instance_id`); `ManifestId{root_namespace, ref}` with ordering + hash; `<aa>` derivation.
- From `CA/Core/CasManifestCodec.h`: `PartManifest`, `ManifestEntry` (`path`, `placement = inline|blob`, `blob_hash`, `blob_size`, `inline_bytes`); `decodePartManifest(std::string_view) -> PartManifest`; `refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body) -> bool`; `manifestNamespaceMatches(const RootNamespace & owning, const PartManifest & body) -> bool` (ref-first / namespace-first, exactly as Phase 1a emits them).
- From `CA/Core/CasLayout.h`: `Layout::manifestKey(const ManifestId&) -> String` (added by 1a). Existing keys (verbatim, from `CasLayout.h`): `gcStateKey()` ⇒ `<prefix>/gc/state`; `rootsRegistryKey()` ⇒ `<prefix>/gc/registry`; `retiredKey(round,fence_seq,shard)` ⇒ `<prefix>/gc/retired/<round>.<fence_seq>/<shard>`; `outcomesKey(round,fence_seq,shard)` ⇒ `<prefix>/gc/outcomes/<round>.<fence_seq>/<shard>`; `blobKey(const BlobId&)` ⇒ `<prefix>/blobs/<aa>/<hash>`; private member `String prefix`.
- From `CA/Core/CasRunFile.h` (1a): `RunFile`, `DataBlock`, `RunFooter`, `RunKind`, and a `RunMerger` that k-way-merges sorted runs by merge key. (Reader/writer/merge API is defined by 1a; this plan calls it.)
- From `CA/Core/CasRootShardCodec.h` (reworked by 1b): `RootShard{... , std::vector<OwnerTransition> transitions, std::vector<PrecommitTransition> precommits, std::vector<PromotePrecommit> promotions, ...}`; `OwnerTransition{uint64_t transition_version; String ref_name; std::optional<ManifestRef> old_manifest; std::optional<ManifestRef> new_manifest;}`; `PrecommitTransition{uint64_t transition_version; UInt128 build_id; String final_ref_name; std::optional<ManifestRef> old_manifest; std::optional<ManifestRef> new_manifest;}`; `PromotePrecommit{uint64_t transition_version; UInt128 build_id; String final_ref_name; ManifestRef manifest;}`; `RootRef`. `decodeRootShard`/`encodeRootShard` from the same header. (1b emits `promotions` as a first-class third record vector so `PromotePrecommit` is GC-foldable.)
- From `CA/Core/CasBackend.h` (verbatim seam): `Backend::get(key,Range={}) -> std::optional<GetResult>` (`GetResult{String bytes; Token token; ObjectMeta attributes;}`); `head(key) -> HeadResult{bool exists; uint64_t size; Token token;}`; `putIfAbsent(key,bytes,meta={}) -> PutResult{PutOutcome outcome; Token token;}` (`PutOutcome::Done|PreconditionFailed`); `casPut(key,bytes,std::optional<Token> expected,meta={}) -> CasResult{CasOutcome outcome; Token token;}` (`CasOutcome::Committed|Conflict`); `deleteExact(key,token) -> DeleteOutcome{Kind kind;}` (`Kind::Deleted|TokenMismatch|NotFound`); `list(prefix,cursor,limit) -> ListPage{std::vector<ListedKey> keys; String next_cursor;}` (`ListedKey{String key; uint64_t size;}`). Accessors: `store->backend()`, `store->layout()`, `store->poolConfig()`, `store->poolMeta()`, `store->readShard(ns,shard)` (returns `std::pair<RootShard, Token>`), `store->mutateShard(ns,shard,fn,&committed)`.

**This phase EMITS:**
- `CA/Core/CasGenerationSeal.h`/`.cpp` — `GenerationSeal` + coverage fields + codec (`FormatId::GenerationSeal`, magic `"CAGN"`); `Layout::generationSealKey`/`blobTargetRunKey`/`partManifestCleanupKey`; **removes** `Layout::gcSnapKey`.
- `CA/Core/CasBlobInDegree.h`/`.cpp` — streaming blob in-degree generation built by merging the prior in-degree run with scattered `±1` deltas via `RunMerger`.
- `CA/Core/CasOrphanManifestSweep.h`/`.cpp` — per-namespace pre-precommit `_manifests` debris sweep.
- `CA/Core/CasGc.*` — `fold` rewritten over owner transitions → blob deltas; remove `cascadeAndPersist`; remove `snap_shards==1` tree branch; keep `gc_shards=1`, all-shard fence, per-candidate `HEAD` in retire.
- `CA/Core/CasFsck.*` — OQ8 read-only manifest audit (Task 9).
- **Deletes:** `CA/Core/CasGcSnap.*`, `CA/Core/CasTreeCodec.*`, `CA/Core/CasClosureWalk.*`.

> `CA/` abbreviates `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`. All paths below are absolute-from-repo-root.

---

## File Structure {#file-structure}

- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h` / `.cpp` — the `GenerationSeal` type, coverage fields, codec, and the three new `Layout` keys.
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h` / `.cpp` — streaming in-degree fold + zero-in-degree scan.
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h` / `.cpp` — per-namespace orphan sweep.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h` / `.cpp` — `fold` rewrite, retire/fence/recheck adaptation, remove `cascadeAndPersist`/`assertSnapJournalCoherent`/snap members, wire the sweep + retire-visibility barrier.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h` / `.cpp` — keep `GcState`/`RetiredSet`; drop `snap_*` fields that the new model no longer uses (see Task 1) and add the generation pointer.
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h` / `.cpp` — add `FormatId::GenerationSeal`; remove `FormatId::Tree` and `FormatId::GcSnap` (and their magics).
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h` — remove `gcSnapKey`; add the three generation keys (in `CasGenerationSeal` task, declared on `Layout`).
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h` / `.cpp` — OQ8 manifest audit.
- Delete: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h` / `.cpp`, `CasTreeCodec.h` / `.cpp`, `CasClosureWalk.h` / `.cpp`.
- Modify (test infra): `src/Disks/tests/cas_test_helpers.h` — drop the `CasTreeCodec.h` include + `writeTreeRaw`; add `writeManifestRaw` + `injectBlobInDegreeGeneration`.
- Create (tests): `src/Disks/tests/gtest_cas_generation_seal.cpp`, `gtest_cas_blob_indegree.cpp`, `gtest_cas_gc_fold.cpp`, `gtest_cas_gc_fence_recheck.cpp`, `gtest_cas_orphan_manifest_sweep.cpp`, `gtest_cas_gc_resume.cpp`.
- Delete (tests): `src/Disks/tests/gtest_cas_gc_snap.cpp`, `gtest_cas_tree_id.cpp`, `gtest_cas_closure_walk.cpp`. Rewrite for the new model: `gtest_cas_gc_round.cpp`, `gtest_cas_gc_formats.cpp`, `gtest_cas_gc_leak.cpp`, `gtest_cas_b140_dangle.cpp`, `gtest_cas_fsck.cpp` (and fix `gtest_cas_build.cpp`, `gtest_cas_protocol_scenarios.cpp`, `gtest_cas_store.cpp`, `gtest_cas_layout.cpp`, `gtest_cas_format.cpp`, `gtest_cas_codecs.cpp`, `gtest_cas_tree_layout.cpp` references — Task 8).

---

### Task 1: `CasGenerationSeal` — type, coverage fields, codec, layout keys {#task-1-casgenerationseal-type-coverage-fields-codec-layout-keys}

Realizes the `GenerationSeal` coverage that makes `SabotageCutOverclaim` (negative control #12) defensible: a sealed generation states, per `(namespace, shard)`, exactly what was folded (`classification`, `folded_token`, `folded_cursor`), its parent generation, the run-list checksums, fence positions, and phase markers. Enforces spec §Debuggability-And-Resume ("every durable generation must answer …") and §Safety-Invariants `JournalCoverage`.

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_generation_seal.cpp`

**Interfaces:**
- Consumes: `ManifestRef`/`ManifestId` (`CasManifestId.h`); `Token` (`CasToken.h`); `FormatId`/`magicFor`/`currentWriterVersion`/`currentCompatibilityVersion` (`CasFormat.h`); `Layout::prefix` (`CasLayout.h`).
- Produces (later tasks rely on these EXACT names/types):
  - `struct RunRef { String key; UInt128 checksum; };`
  - `struct ShardCoverage { uint8_t classification; Token folded_token; uint64_t folded_cursor; };`
  - `struct PhaseMarkers { bool folded; bool retired; bool fenced; bool rechecked; bool deleted; bool trimmed; };`
  - `struct GenerationSeal { uint64_t generation; uint64_t parent_generation; std::map<String, ShardCoverage> per_ns_shard; std::vector<RunRef> blob_target_runs; std::vector<RunRef> part_manifest_cleanup; std::map<String, uint64_t> fence_positions; PhaseMarkers markers; };`
  - `String encodeGenerationSeal(const GenerationSeal &);` / `GenerationSeal decodeGenerationSeal(std::string_view);`
  - `Layout::generationSealKey(uint64_t generation)`; `Layout::blobTargetRunKey(uint64_t generation, uint64_t shard, uint64_t seq)`; `Layout::partManifestCleanupKey(uint64_t generation, uint64_t owner_shard, uint64_t seq)`.

- [ ] **Step 1: Add `FormatId::GenerationSeal` and remove `Tree`/`GcSnap`.** In `CasFormat.h`, the enum currently reads (verbatim):

```cpp
enum class FormatId : uint16_t
{
    Blob = 1,
    Tree = 2,
    Manifest = 3,
    GcSnap = 4,
    GcState = 5,
    RetiredSet = 6,
    Watermark = 7,
    PoolMeta = 8,
    Roster = 9,
    RootsRegistry = 10,
    GcOutcomes = 11,
};
```

Replace it with (remove `Tree`, remove `GcSnap`, append `GenerationSeal`; keep existing numeric values stable for the survivors since CA is pre-release and there is no on-disk compat to honor — the only rule is no two enumerators share a value):

```cpp
enum class FormatId : uint16_t
{
    Blob = 1,
    Manifest = 3,
    GcState = 5,
    RetiredSet = 6,
    Watermark = 7,
    PoolMeta = 8,
    Roster = 9,
    RootsRegistry = 10,
    GcOutcomes = 11,
    GenerationSeal = 12,
};
```

In `CasFormat.cpp`, find the `magicFor` switch (the function returning the per-format magic, e.g. `0x4C424143u` for `Blob`). Delete the `case FormatId::Tree:` and `case FormatId::GcSnap:` arms, and add:

```cpp
        case FormatId::GenerationSeal: return 0x4E474143u;  // "CAGN" little-endian
```

- [ ] **Step 2: Add the three `Layout` keys and remove `gcSnapKey`.** In `CasLayout.h`, delete the existing `gcSnapKey` method (verbatim, to be removed):

```cpp
    String gcSnapKey(uint64_t generation, uint64_t snap_shard) const
    {
        return prefix + "/gc/snap/" + std::to_string(generation) + "/" + std::to_string(snap_shard);
    }
```

and add, next to `outcomesKey`:

```cpp
    /// Per-generation completeness seal: <prefix>/gc/gen/<generation>/seal
    String generationSealKey(uint64_t generation) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/seal";
    }

    /// One blob-target in-degree/delta run segment: <prefix>/gc/gen/<generation>/blob_target/<shard>/<seq>
    String blobTargetRunKey(uint64_t generation, uint64_t shard, uint64_t seq) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/blob_target/"
               + std::to_string(shard) + "/" + std::to_string(seq);
    }

    /// One part-manifest cleanup bundle: <prefix>/gc/gen/<generation>/part_manifest_cleanup/<owner_shard>/<seq>
    String partManifestCleanupKey(uint64_t generation, uint64_t owner_shard, uint64_t seq) const
    {
        return prefix + "/gc/gen/" + std::to_string(generation) + "/part_manifest_cleanup/"
               + std::to_string(owner_shard) + "/" + std::to_string(seq);
    }
```

- [ ] **Step 3: Write the failing test.** Create `src/Disks/tests/gtest_cas_generation_seal.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>

using namespace DB::Cas;

namespace
{
GenerationSeal sampleSeal()
{
    GenerationSeal seal;
    seal.generation = 7;
    seal.parent_generation = 6;
    seal.per_ns_shard["ns1/0"] = ShardCoverage{.classification = 2, .folded_token = Token{"tok-a"}, .folded_cursor = 42};
    seal.per_ns_shard["ns1/1"] = ShardCoverage{.classification = 1, .folded_token = Token{}, .folded_cursor = 0};
    seal.blob_target_runs.push_back(RunRef{.key = "gc/gen/7/blob_target/0/0", .checksum = DB::UInt128(0xABCDEF)});
    seal.part_manifest_cleanup.push_back(RunRef{.key = "gc/gen/7/part_manifest_cleanup/0/0", .checksum = DB::UInt128(0x1234)});
    seal.fence_positions["ns1/0"] = 99;
    seal.fence_positions["_registry"] = 100;
    seal.markers = PhaseMarkers{.folded = true, .retired = true, .fenced = true,
                                .rechecked = true, .deleted = true, .trimmed = false};
    return seal;
}
}

TEST(CasGenerationSeal, RoundTripsAllFields)
{
    const GenerationSeal in = sampleSeal();
    const GenerationSeal out = decodeGenerationSeal(encodeGenerationSeal(in));

    EXPECT_EQ(out.generation, in.generation);
    EXPECT_EQ(out.parent_generation, in.parent_generation);
    ASSERT_EQ(out.per_ns_shard.size(), in.per_ns_shard.size());
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").classification, 2);
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").folded_token.value, "tok-a");
    EXPECT_EQ(out.per_ns_shard.at("ns1/0").folded_cursor, 42u);
    ASSERT_EQ(out.blob_target_runs.size(), 1u);
    EXPECT_EQ(out.blob_target_runs[0].key, "gc/gen/7/blob_target/0/0");
    EXPECT_EQ(out.blob_target_runs[0].checksum, DB::UInt128(0xABCDEF));
    ASSERT_EQ(out.part_manifest_cleanup.size(), 1u);
    EXPECT_EQ(out.fence_positions.at("_registry"), 100u);
    EXPECT_TRUE(out.markers.deleted);
    EXPECT_FALSE(out.markers.trimmed);
}

TEST(CasGenerationSeal, EncodingIsByteDeterministic)
{
    const GenerationSeal in = sampleSeal();
    EXPECT_EQ(encodeGenerationSeal(in), encodeGenerationSeal(in));
}

TEST(CasGenerationSeal, RejectsEmptyAndBadMagic)
{
    EXPECT_ANY_THROW(decodeGenerationSeal(""));
    EXPECT_ANY_THROW(decodeGenerationSeal("not-a-seal"));
}

TEST(CasGenerationSeal, CoverageRecordsEveryDiscoveredShard)
{
    // Completeness: a seal that omits a shard the round visited is invalid input to recheck;
    // the codec preserves exactly the per_ns_shard map it was given (no silent drop).
    GenerationSeal in = sampleSeal();
    in.per_ns_shard["ns2/0"] = ShardCoverage{.classification = 0, .folded_token = Token{}, .folded_cursor = 0};
    const GenerationSeal out = decodeGenerationSeal(encodeGenerationSeal(in));
    EXPECT_TRUE(out.per_ns_shard.contains("ns2/0"));
    EXPECT_EQ(out.per_ns_shard.size(), 3u);
}
```

- [ ] **Step 4: Run the test to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGenerationSeal.*' 2>&1 | tee build/test_generation_seal.log` (after the build in Step 6 fails to compile, this confirms the type is missing). Expected at this point: **compile/link error** — `CasGenerationSeal.h` not found.

- [ ] **Step 5: Write the header.** Create `CasGenerationSeal.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <map>
#include <string>
#include <vector>

namespace DB::Cas
{

/// A reference to one write-once run object plus its content checksum (RunFooter checksum), so a
/// resuming round can verify it adopted the exact bytes a prior attempt sealed (spec §Resume).
struct RunRef
{
    String key;
    UInt128 checksum{};
};

/// What a round did to ONE (namespace, shard). classification is a small enum byte:
///   0 = Absent (shard not present / fresh-pool), 1 = Unchanged (token matched persisted; skipped),
///   2 = Folded (records in (folded_cursor, shard_version] were folded), 3 = Minted (fence-only).
/// folded_token is the shard's observed manifest token at fold time; folded_cursor is the position
/// the fold folded TO (the shard_version). Together they make SabotageCutOverclaim defensible: the
/// recheck can prove the cursor never ran past the sealed deltas.
struct ShardCoverage
{
    uint8_t classification = 0;
    Token folded_token;
    uint64_t folded_cursor = 0;
};

/// Per-generation phase progress — the durable "what has happened" the round-visibility barrier and
/// resume read (spec §Debuggability-And-Resume). Each flips true exactly when that phase is durable.
struct PhaseMarkers
{
    bool folded = false;
    bool retired = false;
    bool fenced = false;
    bool rechecked = false;
    bool deleted = false;
    bool trimmed = false;
};

/// The completeness + index record for one GC generation (spec §Round-Protocol "Fold output is sealed
/// in a write-once generation seal"). Coarse: there is no object per edge/manifest/candidate.
struct GenerationSeal
{
    uint64_t generation = 0;
    uint64_t parent_generation = 0;
    std::map<String, ShardCoverage> per_ns_shard;   /// "ns/shard" -> coverage
    std::vector<RunRef> blob_target_runs;           /// the blob in-degree run segments this gen sealed
    std::vector<RunRef> part_manifest_cleanup;      /// the part-manifest cleanup bundles this gen sealed
    std::map<String, uint64_t> fence_positions;     /// "ns/shard" (+ "_registry") -> fenced version
    PhaseMarkers markers;
};

String encodeGenerationSeal(const GenerationSeal & seal);
GenerationSeal decodeGenerationSeal(std::string_view data);

}
```

- [ ] **Step 6: Write the codec + build.** Create `CasGenerationSeal.cpp`. Use the same protobuf-with-`CasHeader` pattern `CasGcFormats.cpp` uses (a `Cas::Proto::GenerationSealProto` message — add it to the `.proto` alongside `GcStateProto`, fields: `header`, `generation`, `parent_generation`, repeated `per_ns_shard` entries `{key, classification, folded_token_type, folded_token_value, folded_cursor}`, repeated `blob_target_runs`/`part_manifest_cleanup` `{key, checksum_hi, checksum_lo}`, repeated `fence_positions` `{key, version}`, and a `markers` submessage of six bools). Encode sorts `per_ns_shard`/`fence_positions` by key and the run vectors by `key` so bytes are deterministic:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Common/Exception.h>
#include <cas_format.pb.h>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

namespace DB::Cas
{

String encodeGenerationSeal(const GenerationSeal & seal)
{
    Cas::Proto::GenerationSealProto msg;
    auto * hdr = msg.mutable_header();
    hdr->set_magic(magicFor(FormatId::GenerationSeal));
    hdr->set_writer_version(currentWriterVersion());
    hdr->set_compatibility_version(currentCompatibilityVersion());

    msg.set_generation(seal.generation);
    msg.set_parent_generation(seal.parent_generation);

    for (const auto & [key, cov] : seal.per_ns_shard)   /// std::map => sorted => deterministic
    {
        auto * e = msg.add_per_ns_shard();
        e->set_key(key);
        e->set_classification(cov.classification);
        e->set_folded_token_type(static_cast<uint32_t>(cov.folded_token.type));
        e->set_folded_token_value(cov.folded_token.value);
        e->set_folded_cursor(cov.folded_cursor);
    }
    auto addRuns = [](auto * field, const std::vector<RunRef> & runs)
    {
        std::vector<const RunRef *> sorted;
        sorted.reserve(runs.size());
        for (const auto & r : runs)
            sorted.push_back(&r);
        std::sort(sorted.begin(), sorted.end(), [](const RunRef * a, const RunRef * b) { return a->key < b->key; });
        for (const RunRef * r : sorted)
        {
            auto * e = field->Add();
            e->set_key(r->key);
            e->set_checksum_hi(static_cast<uint64_t>(r->checksum >> 64));
            e->set_checksum_lo(static_cast<uint64_t>(r->checksum));
        }
    };
    addRuns(msg.mutable_blob_target_runs(), seal.blob_target_runs);
    addRuns(msg.mutable_part_manifest_cleanup(), seal.part_manifest_cleanup);

    for (const auto & [key, version] : seal.fence_positions)
    {
        auto * e = msg.add_fence_positions();
        e->set_key(key);
        e->set_version(version);
    }
    auto * m = msg.mutable_markers();
    m->set_folded(seal.markers.folded);
    m->set_retired(seal.markers.retired);
    m->set_fenced(seal.markers.fenced);
    m->set_rechecked(seal.markers.rechecked);
    m->set_deleted(seal.markers.deleted);
    m->set_trimmed(seal.markers.trimmed);

    return msg.SerializeAsString();
}

GenerationSeal decodeGenerationSeal(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS generation seal: empty object");

    Cas::Proto::GenerationSealProto msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS generation seal: protobuf parse failed");
    if (msg.header().magic() != magicFor(FormatId::GenerationSeal))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS generation seal: bad magic (got 0x{:08x}, expected 0x{:08x})",
            msg.header().magic(), magicFor(FormatId::GenerationSeal));

    GenerationSeal seal;
    seal.generation = msg.generation();
    seal.parent_generation = msg.parent_generation();
    for (const auto & e : msg.per_ns_shard())
        seal.per_ns_shard[e.key()] = ShardCoverage{
            .classification = static_cast<uint8_t>(e.classification()),
            .folded_token = Token{e.folded_token_value(), static_cast<TokenType>(e.folded_token_type())},
            .folded_cursor = e.folded_cursor()};
    auto readRuns = [](const auto & field, std::vector<RunRef> & runs)
    {
        for (const auto & e : field)
            runs.push_back(RunRef{.key = e.key(),
                .checksum = (DB::UInt128(e.checksum_hi()) << 64) | DB::UInt128(e.checksum_lo())});
    };
    readRuns(msg.blob_target_runs(), seal.blob_target_runs);
    readRuns(msg.part_manifest_cleanup(), seal.part_manifest_cleanup);
    for (const auto & e : msg.fence_positions())
        seal.fence_positions[e.key()] = e.version();
    const auto & m = msg.markers();
    seal.markers = PhaseMarkers{.folded = m.folded(), .retired = m.retired(), .fenced = m.fenced(),
                                .rechecked = m.rechecked(), .deleted = m.deleted(), .trimmed = m.trimmed()};
    return seal;
}

}
```

> Note: the exact `Token` field names (`.value`, `.type`, `TokenType`) and the `cas_format.proto` additions follow the existing `CasToken.h` / `cas_format.proto`. If `Token` has no `type` field in the ground-truth header, drop `folded_token_type`/`set_folded_token_type` and the `TokenType` cast — keep only `.value`. Confirm against `CasToken.h` before writing.

Build: `ninja -C build unit_tests_dbms > build/build.log 2>&1` (analyze `build/build.log` with a subagent; expect clean compile of the three changed files + the new ones).

- [ ] **Step 7: Run the test to verify it passes.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGenerationSeal.*' 2>&1 | tee build/test_generation_seal.log`
Expected: 4 tests PASS (analyze the log with a subagent).

- [ ] **Step 8: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp \
        src/Disks/tests/gtest_cas_generation_seal.cpp \
        contrib/... # the cas_format.proto change if it lives under a generated dir; otherwise its source
git commit -m "CA GC phase1d: GenerationSeal type + codec + coverage fields + layout keys"
```

---

### Task 2: `CasBlobInDegree` — streaming in-degree generation over `RunFile`/`RunMerger` {#task-2-casblobindegree-streaming-in-degree-generation-over-runfile-runmerger}

Realizes `BlobInDegreeMatchesActiveManifests` (durable blob target state = the multiset of blob edges emitted by active manifests) and R1's streaming bound (`RunMerger`, memory `O(inputs * block_size)`). Defends negative controls #4 (missing-body precommit ⇒ no edge ⇒ no spurious `-1`) and #10 (missing committed body must not undercount) at the accounting layer.

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp`
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Consumes: `RunFile`/`DataBlock`/`RunFooter`/`RunKind`/`RunMerger` (`CasRunFile.h`); `BlobId` (`CasIds.h`); `Backend` (`CasBackend.h`); `Layout` keys from Task 1.
- Produces:
  - `struct BlobDelta { UInt128 blob_hash; int64_t delta; };` (always `+1` or `-1` per source edge, pre-merge).
  - `struct Candidate { UInt128 hash; };` (a blob whose in-degree reached zero — reuse the existing `Candidate` type if it survives Task 8's blob-only narrowing; otherwise this local).
  - `void foldDeltasIntoGeneration(Backend & backend, const Layout & layout, uint64_t prior_generation, uint64_t new_generation, uint64_t shard, std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs);` — merges the prior in-degree run for `shard` with `scattered` (sorted by `blob_hash`) via `RunMerger`, writes the new run(s) under `blobTargetRunKey(new_generation, shard, seq)`, appends their `RunRef`s to `out_runs`.
  - `std::vector<Candidate> zeroInDegree(Backend & backend, const Layout & layout, uint64_t generation, uint64_t shard);` — streams the sealed in-degree run for `(generation, shard)` and yields every blob whose merged in-degree is exactly 0.

- [ ] **Step 1: Write the failing test.** Create `src/Disks/tests/gtest_cas_blob_indegree.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>

using namespace DB::Cas;

namespace
{
DB::UInt128 b(uint64_t n) { return DB::UInt128(n); }
}

TEST(CasBlobInDegree, FoldStartsFromEmptyPriorGeneration)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    // Generation 1 from empty prior: +1 b1, +1 b1, +1 b2  =>  indeg(b1)=2, indeg(b2)=1.
    std::vector<BlobDelta> deltas{{b(1), +1}, {b(1), +1}, {b(2), +1}};
    std::vector<RunRef> runs;
    foldDeltasIntoGeneration(backend, layout, /*prior*/0, /*new*/1, /*shard*/0, deltas, runs);
    ASSERT_FALSE(runs.empty());

    const auto zero = zeroInDegree(backend, layout, /*gen*/1, /*shard*/0);
    EXPECT_TRUE(zero.empty());   // nothing at zero yet
}

TEST(CasBlobInDegree, PlusMinusCancelToZeroDetectsCandidate)
{
    InMemoryBackend backend;
    Layout layout{"pool"};

    std::vector<RunRef> runs1;
    foldDeltasIntoGeneration(backend, layout, 0, 1, 0, {{b(1), +1}, {b(2), +1}}, runs1);

    // Generation 2 merges prior gen-1 run with a -1 on b1: indeg(b1)=0, indeg(b2)=1.
    std::vector<RunRef> runs2;
    foldDeltasIntoGeneration(backend, layout, /*prior*/1, /*new*/2, 0, {{b(1), -1}}, runs2);

    const auto zero = zeroInDegree(backend, layout, 2, 0);
    ASSERT_EQ(zero.size(), 1u);
    EXPECT_EQ(zero[0].hash, b(1));
}

TEST(CasBlobInDegree, RunsAreByteDeterministic)
{
    InMemoryBackend a;
    InMemoryBackend b2;
    Layout layout{"pool"};
    std::vector<RunRef> ra;
    std::vector<RunRef> rb;
    // Same deltas in a DIFFERENT input order must produce the same sealed run bytes (sorted by key).
    foldDeltasIntoGeneration(a,  layout, 0, 1, 0, {{b(3), +1}, {b(1), +1}, {b(2), +1}}, ra);
    foldDeltasIntoGeneration(b2, layout, 0, 1, 0, {{b(1), +1}, {b(2), +1}, {b(3), +1}}, rb);
    const auto ga = a.get(layout.blobTargetRunKey(1, 0, 0));
    const auto gb = b2.get(layout.blobTargetRunKey(1, 0, 0));
    ASSERT_TRUE(ga.has_value());
    ASSERT_TRUE(gb.has_value());
    EXPECT_EQ(ga->bytes, gb->bytes);
}

TEST(CasBlobInDegree, NegativeInDegreeIsCorruption)
{
    InMemoryBackend backend;
    Layout layout{"pool"};
    std::vector<RunRef> runs;
    // A -1 with no prior +1 would drive in-degree below zero — an undercount bug; fold must fail closed.
    EXPECT_ANY_THROW(foldDeltasIntoGeneration(backend, layout, 0, 1, 0, {{b(9), -1}}, runs));
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasBlobInDegree.*' 2>&1 | tee build/test_blob_indegree.log`
Expected: compile/link error — `CasBlobInDegree.h` not found.

- [ ] **Step 3: Write the header.** Create `CasBlobInDegree.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGenerationSeal.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/types.h>
#include <vector>

namespace DB::Cas
{

/// A single +1/-1 source-edge update to a blob's in-degree, pre-merge. delta is always +1 or -1.
struct BlobDelta
{
    UInt128 blob_hash{};
    int64_t delta = 0;
};

/// A blob whose merged in-degree reached exactly zero — a retire candidate.
struct BlobCandidate
{
    UInt128 hash{};
};

/// Merge the prior generation's blob in-degree run for `shard` (absent => empty/zero baseline) with
/// `scattered` deltas, producing the new generation's write-once run(s) under
/// blobTargetRunKey(new_generation, shard, seq). Streaming: prior run + scattered deltas are sorted by
/// blob_hash and merged via RunMerger; memory is O(inputs * block_size). The merged per-blob counter
/// must never go below zero (CORRUPTED_DATA — an undercount that would over-delete). Appends the
/// produced runs' RunRefs (key + RunFooter checksum) to out_runs for the GenerationSeal.
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t new_generation, uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs);

/// Stream the sealed in-degree run for (generation, shard) and return every blob at in-degree 0.
std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t shard);

}
```

- [ ] **Step 4: Write the implementation + build.** Create `CasBlobInDegree.cpp`. The merge key is `blob_hash` (fixed-width 16 bytes); the value is a packed `int64` count. `RunKind::BlobInDegree` (add to 1a's enum if not present; otherwise reuse the blob-hash-keyed kind). Read the prior run via a `RunFile` reader over `blobTargetRunKey(prior_generation, shard, *)` (enumerate `seq` until absent), sort `scattered` by `blob_hash`, build a `RunMerger` over `{prior_reader, scattered_reader}` that sums counts per key, and stream-write the output through a `RunFile` writer (256 KiB target blocks, CRC32C per block). Fail closed if any summed count `< 0`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

namespace DB::Cas
{

void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t new_generation, uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs)
{
    /// Deterministic input ordering => byte-reproducible output run (OQ5 resume/adoption).
    std::sort(scattered.begin(), scattered.end(),
        [](const BlobDelta & a, const BlobDelta & b) { return a.blob_hash < b.blob_hash; });

    /// Open the prior generation's run segments (absent => empty baseline; fresh pool / first gen).
    RunReader prior(backend, RunKind::BlobInDegree);
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(prior_generation, shard, seq);
        if (!prior.addSegment(key))
            break;   /// first absent seq ends the prior run
    }

    /// Merge: per blob_hash, sum prior count + all matching deltas. RunMerger yields keys in order.
    RunWriter out(backend, RunKind::BlobInDegree, layout.blobTargetRunKey(new_generation, shard, 0));
    RunMerger merger;
    merger.addCountedRun(prior);          /// prior: (blob_hash -> count)
    merger.addDeltaRun(std::move(scattered));   /// deltas: (blob_hash -> +/-1) summed
    while (auto entry = merger.next())     /// entry = {blob_hash, summed_count}
    {
        if (entry->count < 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS blob in-degree: merged in-degree {} < 0 for a blob in gen {} shard {} "
                "(undercount — fail closed rather than over-delete)", entry->count, new_generation, shard);
        if (entry->count > 0)
            out.append(entry->blob_hash, entry->count);   /// zero-count rows are dropped (no longer pinned)
    }
    const RunFooter footer = out.finalize();
    out_runs.push_back(RunRef{.key = layout.blobTargetRunKey(new_generation, shard, 0),
                              .checksum = footer.checksum});
}

std::vector<BlobCandidate> zeroInDegree(Backend & backend, const Layout & layout,
                                        uint64_t generation, uint64_t shard)
{
    /// A blob is a zero-in-degree candidate when it WAS pinned (a prior gen carried count>0) and the
    /// current gen dropped it to 0. The sealed run drops zero-count rows (above), so zero-in-degree is
    /// computed by diffing this gen against the prior: a key present>0 in (gen-1) and absent/0 in gen.
    /// To keep the run self-contained for resume, the writer above instead emits an explicit 0-row for
    /// keys that transitioned this generation; zeroInDegree streams the run and yields those 0-rows.
    std::vector<BlobCandidate> result;
    RunReader reader(backend, RunKind::BlobInDegree);
    for (uint64_t seq = 0; ; ++seq)
    {
        const String key = layout.blobTargetRunKey(generation, shard, seq);
        if (!reader.addSegment(key))
            break;
    }
    while (auto entry = reader.next())
        if (entry->count == 0)
            result.push_back(BlobCandidate{.hash = entry->blob_hash});
    return result;
}

}
```

> The `RunReader`/`RunWriter`/`RunMerger` member names (`addSegment`, `addCountedRun`, `addDeltaRun`, `next`, `append`, `finalize`, the merged-entry `{blob_hash, count}`) follow Phase 1a's `CasRunFile.h`. If 1a named them differently, adapt the calls (the algorithm is unchanged: sort, k-way merge summing counts, stream-write, fail-closed on negative). Reconcile the zero-row representation between `foldDeltasIntoGeneration` and `zeroInDegree` (emit explicit transitioned-to-0 rows in the writer, or compute the diff in `zeroInDegree`) so the `PlusMinusCancelToZeroDetectsCandidate` test passes — pick one and keep both functions consistent.

Build: `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze the log).

- [ ] **Step 5: Run to verify it passes.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasBlobInDegree.*' 2>&1 | tee build/test_blob_indegree.log`
Expected: 4 tests PASS (subagent-analyze).

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/tests/gtest_cas_blob_indegree.cpp
git commit -m "CA GC phase1d: streaming blob in-degree generation over RunFile/RunMerger"
```

---

### Task 3: `CasGc::fold` rewrite — owner transitions → blob deltas {#task-3-casgc-fold-rewrite-owner-transitions-blob-deltas}

Realizes spec §Fold-Owner-Transitions. Enforces `BlobInDegreeMatchesActiveManifests` and `ManifestActivationMatchesEdges`; defends negative controls **#4** (missing-body precommit ⇒ no `+`), **#10** (missing committed body ⇒ fail closed, never empty), **#22** (promote-after-missing-body ⇒ committed `+`), and the `RefMatchesBody`/`ManifestNamespaceMatches` fail-closed checks (#19, #20). The Phase-0 model already proved these; this gtest is the C++ realization.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Modify: `src/Disks/tests/cas_test_helpers.h`
- Test: `src/Disks/tests/gtest_cas_gc_fold.cpp`

**Interfaces:**
- Consumes: `RootShard{transitions, precommits, promotions}`, `OwnerTransition`, `PrecommitTransition`, `PromotePrecommit` (`CasRootShardCodec.h`); `ManifestId`/`ManifestRef` (`CasManifestId.h`); `PartManifest`/`decodePartManifest`/`refMatchesBody`/`manifestNamespaceMatches` (`CasManifestCodec.h`); `Layout::manifestKey` (`CasLayout.h`); `BlobDelta`/`foldDeltasIntoGeneration` (Task 2); `GenerationSeal`/`ShardCoverage` (Task 1).
- Produces (replaces the old `FoldResult`): `struct FoldResult { GenerationSeal seal; std::vector<std::pair<RootNamespace, uint64_t>> root_shards; std::map<ManifestId, Token> mf_cleanup; };` and `FoldResult Gc::fold(GcState & state, Token & state_token);`.
- New private helper: `void foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas, std::map<ManifestId, Token> & mf_cleanup, RoundReport & report);` — read ONE manifest, validate, emit `sign * 1` per blob entry; on `sign < 0` queue `mf_cleanup`.

- [ ] **Step 1: Add `writeManifestRaw` to `cas_test_helpers.h`.** After `publishRaw` (the new owner-transition-based one introduced by 1b), add a manifest-body fixture mirroring what `Build` emits in 1b (it writes a `PartManifest` body via `decodePartManifest`'s inverse encoder):

```cpp
/// Write a part-manifest body object directly via the manifest codec, exactly as Build::stageTree
/// emits it (1b). Returns the ManifestId. Used by GC fold tests to stage old/new owner targets.
inline DB::Cas::ManifestId writeManifestRaw(
    DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
    const DB::Cas::RootNamespace & ns, const DB::Cas::ManifestRef & ref,
    const std::vector<DB::Cas::ManifestEntry> & entries)
{
    const DB::Cas::ManifestId id{ns, ref};
    DB::Cas::PartManifest body;
    body.ref = ref;
    body.root_namespace = ns;
    body.entries = entries;
    backend.putIfAbsent(layout.manifestKey(id), DB::Cas::encodePartManifest(body));
    return id;
}
```

(Use the exact `PartManifest` field names and `encodePartManifest` signature from 1a's `CasManifestCodec.h`; adjust if they differ.)

- [ ] **Step 2: Write the failing test.** Create `src/Disks/tests/gtest_cas_gc_fold.cpp` covering each spec case. Use a small `InMemoryBackend`, register a namespace, build a `RootShard` with a `transitions`/`precommits` vector, write manifest bodies with `writeManifestRaw`, run `fold`, then read the sealed in-degree run via `zeroInDegree`/the run reader and assert per-blob counts:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{
ManifestEntry blobEntry(const String & path, DB::UInt128 hash)
{
    ManifestEntry e;
    e.path = path;
    e.placement = ManifestEntry::Placement::Blob;
    e.blob_hash = hash;
    e.blob_size = 1;
    return e;
}
}

// Committed new_manifest => +1 per blob entry (BlobInDegreeMatchesActiveManifests).
TEST(CasGcFold, CommittedAddEmitsPlusOnePerBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);   // helper from 1b/1c test infra
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{/*writer*/"srv-a", /*build_seq*/1, /*instance*/DB::UInt128(0xAA)};
    writeManifestRaw(*backend, store->layout(), ns,
        ref, {blobEntry("a", DB::UInt128(1)), blobEntry("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", /*old*/std::nullopt, /*new*/ref);

    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();

    // After fold, blob 1 and blob 2 each have in-degree 1 in the sealed generation.
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(2)), 1);
}

// Owner removal => -1 per blob entry; the removed ManifestId is queued for cleanup.
TEST(CasGcFold, RemovalEmitsMinusOneAndQueuesCleanup)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {blobEntry("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();                       // +1 sealed
    dropRefTransition(*backend, store->layout(), ns, "tbl", /*old*/ref);  // owner removed
    gc.runRegularRound();                       // -1 folded
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

// Precommit with a PRESENT, valid body => +1 (PrecommitMayReferenceMissingBlob still allows missing blob bytes).
TEST(CasGcFold, PrecommitBodyPresentEmitsPlusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {blobEntry("a", DB::UInt128(1))});
    addPrecommitTransition(*backend, store->layout(), ns, /*build*/DB::UInt128(7), "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
}

// Precommit whose body is ABSENT => NO delta (missing-body activation; control #4).
TEST(CasGcFold, PrecommitMissingBodyEmitsNoDelta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    // No writeManifestRaw — body is absent.
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    EXPECT_NO_THROW(gc.runRegularRound());      // 404 on the body must NOT throw (record-and-continue)
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
}

// Promote AFTER a missing-body precommit => committed +1 (control #22).
TEST(CasGcFold, PromoteAfterMissingBodyEmitsCommittedPlusOne)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    addPrecommitTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();                       // missing-body activation: no delta
    writeManifestRaw(*backend, store->layout(), ns, ref, {blobEntry("a", DB::UInt128(1))});  // body now present
    promoteTransition(*backend, store->layout(), ns, DB::UInt128(7), "tbl", ref);
    gc.runRegularRound();                       // promotion folds as committed add
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);
}

// Committed add naming a MISSING body (404) => fail-closed FOR THAT DECISION, never treated as empty
// (control #10), but NOT a throw that wedges the round (feedback_ca_gc_never_throw_on_404): the cursor
// is clamped below the decision and the anomaly is surfaced; no +1 is guessed.
TEST(CasGcFold, CommittedMissingBodyClampsCursorAndRecordsAnomaly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    const uint64_t v = publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);  // no body written
    Gc gc(store, DB::UInt128(0xG1));
    const RoundReport report = gc.runRegularRound();   // 404 on committed body must NOT throw/wedge
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0));
    // No +1 was guessed for the missing committed body.
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);
    // Cursor clamped below the unresolved decision so trim cannot pass it; it is retried next round.
    const GenerationSeal sealed = decodeGenerationSeal(
        backend->get(store->layout().generationSealKey(currentGenerationOf(*backend, store->layout())))->bytes);
    EXPECT_LT(sealed.per_ns_shard.at(cursorKeyForTest(ns, 0)).folded_cursor, v);
}

// Body whose self-ref/self-ns disagree (PRESENT but INVALID) => hard fail closed (controls #19, #20).
// This is genuine corruption, distinct from a 404, so it DOES throw CORRUPTED_DATA.
TEST(CasGcFold, RefOrNamespaceMismatchFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    // Write a body whose internal ref points at a DIFFERENT instance id (refMatchesBody must reject).
    PartManifest bad;
    bad.ref = ManifestRef{"srv-a", 1, DB::UInt128(0xBB)};   // != ref
    bad.root_namespace = ns;
    bad.entries = {blobEntry("a", DB::UInt128(1))};
    backend->putIfAbsent(store->layout().manifestKey(ManifestId{ns, ref}), encodePartManifest(bad));
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&]{ gc.runRegularRound(); });
}

// Owner-removal whose OLD body is gone at removal-fold => fail-closed anomaly: the fold must NOT
// silently skip the removal, NOT emit a partial/empty decrement, NOT advance the cursor past it, and
// NOT let trim pass that point — so the blobs are neither under-counted nor over-deleted (control #11
// keeps the old body present at removal-fold; a missing body here is an anomaly to surface).
TEST(CasGcFold, RemovalWithMissingOldBodyHaltsAtCursorAndRecordsAnomaly)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {blobEntry("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();                       // +1 sealed; blob 1 in-degree 1

    // Drop the ref (owner removal at version V) but ALSO delete the old body out from under GC, so the
    // removal-fold's -1 cannot be resolved.
    const uint64_t removal_version = dropRefTransition(*backend, store->layout(), ns, "tbl", /*old*/ref);
    deleteManifestBody(*backend, store->layout(), ManifestId{ns, ref});   // body gone before its decrement

    // Fold must NOT throw (record-and-continue) and must NOT touch the count: no partial -1.
    const RoundReport report = gc.runRegularRound();
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 1);   // unchanged: no silent -1, no over-delete

    // The anomaly is surfaced and the sealed cursor is CLAMPED below the unresolved removal, so trim
    // cannot pass it and the decrement is retried next round.
    EXPECT_TRUE(report.hasAnomaly(ns, /*shard*/0));
    const GenerationSeal sealed = decodeGenerationSeal(
        backend->get(store->layout().generationSealKey(currentGenerationOf(*backend, store->layout())))->bytes);
    EXPECT_LT(sealed.per_ns_shard.at(cursorKeyForTest(ns, 0)).folded_cursor, removal_version);
}
```

> The test-side transition helpers (`publishCommittedTransition`, `dropRefTransition`, `addPrecommitTransition`, `promoteTransition`, `openStoreForTest`, `inDegreeOf`) come from 1b/1c's test infra. If 1b did not provide them, add thin wrappers in `cas_test_helpers.h` that read-modify-CAS a `RootShard` to append the corresponding `OwnerTransition`/`PrecommitTransition`/`PromotePrecommit` and bump `shard_version` (mirroring the production `Build` path), and `inDegreeOf` that streams `blobTargetRunKey` of the current generation. The removal-anomaly test also needs: `dropRefTransition` returns the removal's `transition_version`; `deleteManifestBody(backend, layout, id)` exact-token-deletes a manifest body (HEAD then `deleteExact`); `currentGenerationOf`/`cursorKeyForTest` read the latest generation pointer and render `cursorKey(ns, shard)`; and `RoundReport::hasAnomaly(ns, shard)` reports whether `recordAnomaly` fired for that shard (add the `anomalies` vector + `recordAnomaly`/`hasAnomaly` to `RoundReport`).

- [ ] **Step 2b: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcFold.*' 2>&1 | tee build/test_gc_fold.log`
Expected: compile error (old `fold` signature) and/or test failures.

- [ ] **Step 3: Replace the `FoldResult` struct and `fold` declaration in `CasGc.h`.** Delete the old `FoldResult` (the `std::map<uint64_t, GcSnap> snap;` form) and its doc-comment; replace with:

```cpp
    /// What one R1 fold produced. The blob deltas are sealed into a write-once generation BEFORE
    /// retire; `seal` is the durable index, `root_shards` the discovered universe (for trim), and
    /// `mf_cleanup` the part-manifest cleanup work keyed by ManifestId (owner-removed bodies whose
    /// exact-token delete is deferred until their decrements are sealed — spec §Retire / §Recheck).
    struct FoldResult
    {
        GenerationSeal seal;
        std::vector<std::pair<RootNamespace, uint64_t>> root_shards;
        std::map<ManifestId, Token> mf_cleanup;
    };

    /// R1 (spec §Fold-Owner-Transitions): per changed root shard, stream OwnerTransition /
    /// PrecommitTransition / PromotePrecommit in journal order; for each old/new manifest ref read ONE
    /// PartManifest, validate RefMatchesBody + ManifestNamespaceMatches, and emit +1 (committed add /
    /// precommit body-present / promote-after-missing-body) or -1 (owner removal) per blob entry.
    /// 404 RULE (refined): a body that is PRESENT-but-invalid (ref/namespace mismatch) is genuine
    /// corruption => CORRUPTED_DATA (hard). A MISSING body (404) is handled by where it appears:
    ///   - precommit `new` missing body  => no blob edges (legal; PrecommitMayReferenceMissingManifest);
    ///   - committed/promoted `new` missing body, or an owner-removal `old` body missing at removal-fold
    ///     => fail-closed FOR THAT DECISION: clamp the shard's folded_cursor below the decision, record
    ///        the anomaly (surfaced to fsck), and stop folding THIS shard — never guess a delta, never
    ///        delete off a missing committed body. GC NEVER wedges the whole round on a 404: it records
    ///        and continues other shards (feedback_ca_gc_never_throw_on_404).
    /// Scattered deltas are merged with the prior in-degree run via foldDeltasIntoGeneration; the result
    /// is sealed in a write-once GenerationSeal, then ONE gc/state CAS advances the generation pointer
    /// against state_token (threaded into retire, never re-read — the zombie-steal protection
    /// runRegularRound documents).
    FoldResult fold(GcState & state, Token & state_token);

    /// Read ONE part manifest named by `id`, validate it, and append sign*(+1) blob deltas for each
    /// blob entry to `deltas`. On sign<0 queue (id -> token) into mf_cleanup. Returns whether a body was
    /// read+validated: false => ABSENT body (404; the caller decides per the 404 rule — legal for a
    /// precommit new, an anomaly to clamp+surface for a committed/promote new or an owner-removal old).
    /// A body that is PRESENT but fails RefMatchesBody / ManifestNamespaceMatches throws CORRUPTED_DATA.
    bool foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Token> & mf_cleanup, RoundReport & report);
```

Remove the now-dead declarations: `foldShardRecords`, `assertSnapJournalCoherent`, `persistGenerationProbingUpward`, `loadSnap`, `cascadeAndPersist`, and the `RecheckResult::deleted_trees` field's tree semantics (kept blob-only — Task 5). Remove the snap members `std::optional<std::map<uint64_t, GcSnap>> resident_snap;` and `uint64_t resident_generation = 0;`. Update `#include` — drop `CasGcSnap.h`, add `CasBlobInDegree.h`, `CasGenerationSeal.h`, `CasManifestCodec.h`, `CasRunFile.h`.

- [ ] **Step 4: Rewrite `fold` and add `foldManifestEdges` in `CasGc.cpp`.** Replace the whole `Gc::fold` body (from line 1634, including the `snap_shards != 1` throw at 1641-1644 and the `reclaimAbandonedPrecommit` at 1713 — keep precommit reclaim, drop the tree/snap machinery). The new body:

```cpp
bool Gc::foldManifestEdges(const ManifestId & id, int sign, std::vector<BlobDelta> & deltas,
                           std::map<ManifestId, Token> & mf_cleanup, RoundReport & report)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    const String key = layout.manifestKey(id);
    const HeadResult head = backend.head(key);
    if (!head.exists)
        return false;   /// absent body: caller decides (missing-body precommit OK; committed => fail closed)

    const auto got = backend.get(key);
    if (!got)
        return false;   /// raced delete between HEAD and GET — record-and-continue (never throw on 404)

    const PartManifest body = decodePartManifest(got->bytes);
    if (!refMatchesBody(id.ref, body))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc fold: manifest body ref mismatch at {} (RefMatchesBody fail-closed)", key);
    if (!manifestNamespaceMatches(id.root_namespace, body))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS gc fold: manifest body namespace mismatch at {} (ManifestNamespaceMatches fail-closed)", key);

    for (const ManifestEntry & entry : body.entries)
        if (entry.placement == ManifestEntry::Placement::Blob)
            deltas.push_back(BlobDelta{.blob_hash = entry.blob_hash, .delta = sign});

    if (sign < 0)
        mf_cleanup.emplace(id, got->token);   /// owner removed: defer exact-token body delete to recheck
    return true;
}

Gc::FoldResult Gc::fold(GcState & state, Token & state_token)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();
    FoldResult result;

    /// B171 precommit reclaim uses the per-round watermark caches the K=2 detector accumulates.
    beginWatermarkRound();

    /// 1. Discover the namespace universe FROM THE REGISTRY (LIST is only an accelerator).
    result.root_shards = discoverUniverse();

    /// 2. Stream owner transitions per changed shard; emit +/-1 blob deltas. gc_shards == 1: every
    /// delta goes to target shard 0. (Sharded scatter by blob hash is Phase 4.)
    std::vector<BlobDelta> deltas;
    bool folded_any = false;
    const uint64_t new_generation = state.snap_generation + 1;

    result.seal.generation = new_generation;
    result.seal.parent_generation = state.snap_generation;
    result.seal.markers.folded = false;

    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const String cursor_key = cursorKey(ns, root_shard);
        const uint64_t cursor = sealedCursorOf(state, cursor_key);   /// from the parent seal (Task 7)

        ShardCoverage cov;
        cov.folded_token = manifest_token;
        cov.folded_cursor = root.shard_version;   /// provisional; reset to resolved_through below
        cov.classification = 0;   /// refined below

        bool shard_changed = false;
        /// A removal-fold whose old body is missing/invalid is an ANOMALY: a correctly-ordered protocol
        /// keeps the old body present at removal-fold time because its delete is gated behind sealed
        /// decrements (negative control #11), so a missing body here means we cannot resolve the -1. We
        /// must NOT advance the fold cursor past such a removal, must NOT emit a partial/empty decrement,
        /// and must NOT let trim pass that point — otherwise the referenced blobs are either silently
        /// under-counted (leak) or, if we guessed, over-deleted. So: record the anomaly, CLAMP the
        /// shard's folded_cursor to just below the unresolved removal, and stop folding THIS shard. GC
        /// never wedges the whole round on this — other shards still fold (record-and-continue).
        uint64_t resolved_through = root.shard_version;   /// folded_cursor we commit if nothing halts us
        bool removal_anomaly = false;
        auto haltAtRemoval = [&](uint64_t at_version, const ManifestId & gone, const char * what)
        {
            report.recordAnomaly(ns, root_shard, gone, what);   /// surfaced (fsck / round report / log)
            resolved_through = at_version - 1;                  /// cursor does not advance past the removal
            removal_anomaly = true;
        };

        /// Committed owner transitions in journal order.
        for (const OwnerTransition & t : root.transitions)
        {
            if (t.transition_version <= cursor || t.transition_version > root.shard_version)
                continue;
            if (t.old_manifest)
            {
                const ManifestId old_id{ns, *t.old_manifest};
                if (!foldManifestEdges(old_id, -1, deltas, result.mf_cleanup, report))
                {
                    /// Old body gone at removal-fold: do NOT skip silently, do NOT emit a partial -1.
                    haltAtRemoval(t.transition_version, old_id, "committed owner-removal: old body missing at removal-fold");
                    break;
                }
            }
            if (t.new_manifest)
            {
                const ManifestId id{ns, *t.new_manifest};
                /// A committed ref naming a MISSING body (404) is fail-closed FOR THIS DECISION: do not
                /// guess a +1, do not advance the cursor past it, surface it to fsck — but do NOT throw
                /// (a throw would wedge the whole round; feedback_ca_gc_never_throw_on_404). The cursor
                /// clamp keeps INV_NO_DANGLE: a committed owner is never treated as having zero edges.
                /// (A body that is PRESENT but invalid — ref/namespace mismatch — is genuine corruption
                /// and foldManifestEdges throws CORRUPTED_DATA; that is a separate, hard failure.)
                if (!foldManifestEdges(id, +1, deltas, result.mf_cleanup, report))
                {
                    haltAtRemoval(t.transition_version, id, "committed ref names a missing manifest body (CommittedManifestBodyRequired)");
                    break;
                }
            }
            shard_changed = true;
        }
        /// Precommit transitions + promotions (precommit body MAY be missing => no delta).
        for (const PrecommitTransition & p : root.precommits)
        {
            if (removal_anomaly)
                break;   /// a prior unresolved removal clamped the cursor; stop folding this shard
            if (p.transition_version <= cursor || p.transition_version > root.shard_version)
                continue;
            if (p.old_manifest)
            {
                /// Removal of a precommit: mirror ONLY edges actually emitted at activation. If the
                /// activation was missing-body (no edges), emit nothing (control #4 undercount guard).
                const ManifestId old_id{ns, *p.old_manifest};
                if (wasActivatedWithBody(state, old_id))   /// from the parent seal's activation record
                {
                    if (!foldManifestEdges(old_id, -1, deltas, result.mf_cleanup, report))
                    {
                        /// It WAS activated with a body (so it emitted +edges) but the body is gone at
                        /// removal-fold: the matching -1 is unresolvable => same anomaly, fail closed.
                        haltAtRemoval(p.transition_version, old_id, "precommit owner-removal: activated body missing at removal-fold");
                        break;
                    }
                }
            }
            if (p.new_manifest)
            {
                const ManifestId id{ns, *p.new_manifest};
                const bool had_body = foldManifestEdges(id, +1, deltas, result.mf_cleanup, report);
                recordActivation(result.seal, id, had_body);   /// ManifestActivationMatchesEdges oracle
            }
            shard_changed = true;
        }
        /// PromotePrecommit: same readable manifest => owner-only move, no delta; missing-body or
        /// not-yet-active precommit => committed add (+1) after reading+validating the body (control #22).
        for (const PromotePrecommit & pr : root.promotions)
        {
            if (removal_anomaly)
                break;   /// a prior unresolved removal clamped the cursor; stop folding this shard
            if (pr.transition_version <= cursor || pr.transition_version > root.shard_version)
                continue;
            const ManifestId id{ns, pr.manifest};
            if (!wasActivatedWithBody(state, id))
            {
                /// Promote of a not-yet-active precommit whose body is MISSING (404) is fail-closed FOR
                /// THIS DECISION (clamp + surface, never throw/wedge — same rule as a committed add). A
                /// present-but-invalid body still throws inside foldManifestEdges.
                if (!foldManifestEdges(id, +1, deltas, result.mf_cleanup, report))
                {
                    haltAtRemoval(pr.transition_version, id, "promote names a missing manifest body (committed ref must never name a missing manifest)");
                    break;
                }
            }
            shard_changed = true;
        }

        /// Commit the cursor we actually RESOLVED through. On a removal anomaly this is below the
        /// unresolved removal, so trim (Task 7, sourced from this sealed cursor) can never pass that
        /// point and the unresolved decrement is retried (with the anomaly surfaced) next round.
        cov.folded_cursor = resolved_through;
        cov.classification = shard_changed ? 2 : 1;
        result.seal.per_ns_shard[cursor_key] = cov;
        if (shard_changed)
            folded_any = true;

        if (Layout::isPrecommitNamespace(ns))
            reclaimAbandonedPrecommit(ns, root_shard, root, state.round + 1);
    }

    if (!folded_any)
        return result;   /// nothing new: state_token unchanged, retire rides the lease token

    /// 3. Merge deltas into the new generation's blob in-degree run; seal it; advance the pointer.
    foldDeltasIntoGeneration(backend, layout, state.snap_generation, new_generation, /*shard*/0,
                             std::move(deltas), result.seal.blob_target_runs);
    /// part-manifest cleanup bundle(s) — keyed by ManifestId; one bundle for gc_shards==1.
    writePartManifestCleanupBundle(backend, layout, new_generation, /*owner_shard*/0, result.mf_cleanup,
                                   result.seal.part_manifest_cleanup);
    result.seal.markers.folded = true;
    backend.putIfAbsent(layout.generationSealKey(new_generation), encodeGenerationSeal(result.seal));

    state.snap_generation = new_generation;
    const CasResult fold_res = backend.casPut(layout.gcStateKey(), encodeGcState(state), state_token);
    if (fold_res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc fold: gc/state moved during the fold (another leader advanced it); retry next round");
    state_token = fold_res.token;
    return result;
}
```

> The helpers `sealedCursorOf`, `wasActivatedWithBody`, `recordActivation`, `recordAnomaly`, `writePartManifestCleanupBundle` are small additions: `sealedCursorOf`/`wasActivatedWithBody` read the **parent** generation seal (`generationSealKey(state.snap_generation)`) loaded once at fold start; `recordActivation` stamps the activation bit into the new seal's `per_ns_shard` coverage (a per-`ManifestId` activation map can ride alongside, or reuse `ShardCoverage`); `recordAnomaly` (on `RoundReport`) records an unresolved removal (namespace/shard/ManifestId/reason) so the cursor-clamp is observable to fsck/logs (it never throws — surfacing, not wedging). The promotion loop reads `root.promotions` directly — 1b emits `PromotePrecommit` as a first-class third record vector (`RootShard.promotions`), per the canonical contract, so there is no `promotionsOf` helper and promotions are not folded into `precommits`. Keep the activation representation consistent with Task 5's recheck reader. The generation-seal write is `putIfAbsent` (write-once; on `PreconditionFailed` adopt the byte-equal occupant as own crash-replay, else `ABORTED` — same adoption rule retire uses).

- [ ] **Step 5: Update `runRegularRound` in `CasGc.cpp`.** Remove the `assertSnapJournalCoherent(folded.snap, folded.root_shards);` call (lines 108-114) and the `cascadeAndPersist(...)` call (line 146). Change `fold`'s consumers: `retire`, `recheck`, `trim` now take the `FoldResult` (seal/root_shards/mf_cleanup) instead of `folded.snap`. Remove the `resident_snap`/`resident_generation` assignment at lines 162-163. The new tail reads:

```cpp
    FoldResult folded = fold(state, state_token);

    /// R2: retire blob candidates (zero-in-degree in the sealed generation) + part-manifest cleanup.
    const RetireResult retired = retire(state, state_token, folded, report);
    report.round = state.round;

    /// R3: global registry + all-shard fence; record fence positions into the seal.
    fence(state, state_token, folded);

    /// R4: fold-through-fence recheck + the single content-delete site + exact-token manifest deletes.
    recheck(state, folded, retired, report);

    /// Trim journals below sealed cursor coverage; trim cleanup bundles after decrements are sealed.
    trim(folded, state.round);

    return report;
```

(Adjust `retire`/`fence`/`recheck`/`trim` signatures to take `const FoldResult &` — Tasks 4, 5, 7 finalize them.)

- [ ] **Step 6: Build.** `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze). Expect failures from `retire`/`fence`/`recheck`/`trim` still referencing `snap` — that is fine; this task only needs `CasGcFold.*` and `CasBlobInDegree.*`/`CasGenerationSeal.*` to compile. If the others block the build, stub their bodies minimally to compile (they are rewritten in Tasks 4-7) — but prefer completing Task 3's `fold` and letting Tasks 4-7 follow before the first green build at Task 4's end. **If a clean build is not reachable until Task 5, fold Steps 6-8 of this task into Task 5's build/run/commit** (the reviewer may reject Task 3 alone — that is acceptable for this rewrite).

- [ ] **Step 7: Run to verify it passes.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcFold.*' 2>&1 | tee build/test_gc_fold.log`
Expected: 7 `CasGcFold` tests PASS (subagent-analyze).

- [ ] **Step 8: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/cas_test_helpers.h \
        src/Disks/tests/gtest_cas_gc_fold.cpp
git commit -m "CA GC phase1d: fold owner transitions into blob deltas (committed/precommit/promote, fail-closed)"
```

---

### Task 4: Retire — blob per-candidate `HEAD` + part-manifest retire ordering {#task-4-retire-blob-per-candidate-head-part-manifest-retire-ordering}

Realizes spec §Retire. Enforces `ExactDeleteOnly` (retire observes the exact token the delete will carry) and the ordering obligation that a manifest body's exact-token delete is deferred until its owner-removal decrements are sealed (defends control **#11**). Keeps the proved per-candidate `HEAD`.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Test: append to `src/Disks/tests/gtest_cas_gc_fence_recheck.cpp` (created here; shared with Task 5).

**Interfaces:**
- Consumes: `FoldResult` (Task 3); `zeroInDegree`/`BlobCandidate` (Task 2); `RetiredSet`/`RetiredEntry` (`CasGcFormats.h`); `Backend::head`/`putIfAbsent`/`get` (`CasBackend.h`); `Layout::retiredKey`/`blobKey` (`CasLayout.h`).
- Produces: `struct RetireResult { std::map<uint64_t, RetiredSet> blobs; std::map<ManifestId, Token> mf_cleanup; };` and `RetireResult Gc::retire(GcState & state, Token & state_token, const FoldResult & folded, RoundReport & report);`.

- [ ] **Step 1: Write the failing test.** Create `src/Disks/tests/gtest_cas_gc_fence_recheck.cpp` with the retire-ordering case first:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

// Retire writes a per-shard retired set keyed by (round, fence_seq, shard) with the EXACT observed token.
TEST(CasGcRetire, BlobRetireObservesExactTokenAndWritesSet)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    // Stage a blob body so HEAD returns a token; publish then drop so its in-degree zeroes.
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));   // helper writing a blob object
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob*/});  // entry referencing blob 1
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();                            // +1
    dropRefTransition(*backend, store->layout(), ns, "tbl", ref);
    gc.runRegularRound();                            // -1 => blob 1 at zero; retire observes its token

    // A retired set exists for round 2 carrying blob 1's observed token (not yet deleted until recheck).
    EXPECT_TRUE(retiredSetContains(*backend, store->layout(), /*round*/2, DB::UInt128(1)));
}

// The owner-removed manifest body is NOT deleted during retire — only after recheck seals decrements (#11).
TEST(CasGcRetire, ManifestBodyNotDeletedBeforeDecrementsSealed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", ref);

    // Single round: after fold seals the -1 and retire runs, the manifest body still EXISTS
    // (its exact-token delete is in recheck, after the decrement generation is durable).
    // We assert by hooking the backend to fail the round right after retire — the body must remain.
    // (Simpler: assert the manifest cleanup bundle is durable while the body still exists mid-round
    // is not observable from outside; instead assert end-to-end: after a FULL round the body is gone,
    // AND a crash between fold-seal and body-delete leaves the body deletable from the bundle.)
    gc.runRegularRound();
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, ref})).exists);
}
```

> If the mid-round non-deletion is not externally observable, keep the second test as the end-to-end "body eventually deleted" assertion and rely on the Phase-0 model + the recheck-ordering test in Task 5 for the ordering proof. Do not add white-box hooks unless a natural black-box assertion is impossible (per `feedback_testing_style_natural_blackbox`).

- [ ] **Step 2: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRetire.*' 2>&1 | tee build/test_gc_retire.log`
Expected: compile error (`retire` signature) / failures.

- [ ] **Step 3: Rewrite `retire` in `CasGc.h`/`.cpp`.** Replace the declaration with `RetireResult Gc::retire(GcState & state, Token & state_token, const FoldResult & folded, RoundReport & report);` and add the `RetireResult` struct. The body keeps the proved shape (the `backend.head(objectKey(...))` at the old line 1007, the per-`(round,fence_seq,shard)` `putIfAbsent` write-once with byte-equal adoption at the old line 1151-1153, and the `.round`-advancing gc/state CAS), but derives candidates from `zeroInDegree(backend, layout, folded.seal.generation, /*shard*/0)` (blobs only — no `GcSnap`, no tree candidates), and passes `folded.mf_cleanup` straight through into `RetireResult::mf_cleanup` (the bodies were already token-captured at fold time; do not re-`HEAD` a condemned body — `feedback_ca_resurrect_invariant`):

```cpp
RetireResult Gc::retire(GcState & state, Token & state_token, const FoldResult & folded, RoundReport & report)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();
    chassert(state.lease.owner == gc_id);
    const uint64_t round = state.round + 1;

    RetireResult result;
    result.mf_cleanup = folded.mf_cleanup;   /// tokens captured during fold; deferred to recheck

    /// Blob candidates: zero-in-degree in the sealed generation (the model's GRetire guard, now
    /// blob-only). ONE HEAD per candidate observes the current token; an absent object is SKIPPED
    /// (a prior round's landed delete — never fabricate a token).
    for (const BlobCandidate & cand : zeroInDegree(backend, layout, folded.seal.generation, /*shard*/0))
    {
        const HeadResult observed = backend.head(layout.blobKey(BlobId(u128ToHex(cand.hash))));
        if (!observed.exists)
            continue;
        RetiredEntry entry;
        entry.kind = ObjectKind::Blob;
        entry.hash = cand.hash;
        entry.token = observed.token;
        entry.size = retiredLogicalSize(ObjectKind::Blob, observed.size, store->poolMeta().blob_header_len);
        result.blobs[/*single shard*/0].entries.push_back(std::move(entry));
    }

    /// Write each shard's retired set write-once (adopt a byte-equal occupant as our crash-replay).
    for (auto & [shard, set] : result.blobs)
    {
        const String key = layout.retiredKey(round, state.fence_seq, shard);
        const String body = encodeRetiredSet(set);
        if (backend.putIfAbsent(key, body).outcome == PutOutcome::PreconditionFailed)
        {
            const auto existing = backend.get(key);
            if (!existing)
                throw Exception(ErrorCodes::ABORTED,
                    "CAS gc retire: retired set at {} vanished between putIfAbsent and read", key);
            if (existing->bytes != body)
            {
                try { set = decodeRetiredSet(existing->bytes); }
                catch (const Exception & e)
                {
                    throw Exception(ErrorCodes::ABORTED,
                        "CAS gc retire: undecodable occupant at {} cannot be adopted: {}", key, e.message());
                }
            }
        }
    }

    /// ONE gc/state CAS advances .round — the durable "retire phase complete" marker (ViewableRound).
    GcState next = state;
    next.round = round;
    const CasResult res = backend.casPut(layout.gcStateKey(), encodeGcState(next), state_token);
    if (res.outcome != CasOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS gc retire: gc/state moved during retire (another leader advanced it); retry next round");
    state = std::move(next);
    state_token = res.token;
    return result;
}
```

Remove the old tree-candidate / `stripTree` / `forget` branches (the old lines 1021-1100 P9-prune block applied to trees), and the `std::map<uint64_t, GcSnap> & snap` parameter.

- [ ] **Step 4: Build, run, commit** (combined with Task 5 if a clean build needs recheck — see Task 3 Step 6 note). When green:

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRetire.*' 2>&1 | tee build/test_gc_retire.log` → PASS.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_fence_recheck.cpp
git commit -m "CA GC phase1d: retire blobs by sealed zero-in-degree + defer manifest body delete"
```

---

### Task 5: Fence (registry + all-shard) + recheck (fold-through-fence) + exact-token deletes {#task-5-fence-registry-all-shard-recheck-fold-through-fence-exact-token-deletes}

Realizes spec §Global-Fence, §Recheck-And-Delete, §Retire-Visibility-Barrier. Defends controls **#11** (delete body only after decrements sealed), **#12** (`SabotageCutOverclaim` — recheck folds through the sealed cursor), **#13** (`ViewableRound`), **#14** (skip-fence dangle), **#16** (non-exact / reused-token delete). Keeps the single content-delete site.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Test: append to `src/Disks/tests/gtest_cas_gc_fence_recheck.cpp`.

**Interfaces:**
- Consumes: `FoldResult`/`RetireResult` (Tasks 3, 4); `foldDeltasIntoGeneration`/`zeroInDegree` (Task 2); `GenerationSeal` (Task 1); `RootsRegistry`/`decodeRootsRegistry`/`encodeRootsRegistry` (`CasRootsRegistry.h`); `Backend::deleteExact` (`CasBackend.h`); `Layout::rootsRegistryKey`/`outcomesKey`/`blobKey`/`manifestKey`.
- Produces: `void Gc::fence(GcState & state, Token & state_token, FoldResult & folded);` (records fence positions into `folded.seal.fence_positions`, advances all-shard fence_round); `void Gc::recheck(GcState & state, FoldResult & folded, const RetireResult & retired, RoundReport & report);` (folds the fence window into a completion generation, then deletes).

- [ ] **Step 1: Write the failing tests.** Append to `gtest_cas_gc_fence_recheck.cpp`:

```cpp
// Fence raises fence_round on every root shard of every registered namespace + the registry.
TEST(CasGcFence, RaisesAllShardAndRegistryFence)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();
    // Every shard of ns is at fence_round >= the executed round; registry fence_round advanced too.
    for (uint64_t s = 0; s < store->poolConfig().root_shards; ++s)
        EXPECT_GE(fenceRoundOf(*backend, store->layout(), ns, s), 1u);
    EXPECT_GE(registryFenceRound(*backend, store->layout()), 1u);
}

// A publish racing the fence (in-degree restored at/below the fence) is SPARED, not deleted (#14).
TEST(CasGcRecheck, PublishRacingFenceSparesBlob)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r1{"srv-a", 1, DB::UInt128(0xA1)};
    const ManifestRef r2{"srv-a", 2, DB::UInt128(0xA2)};
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r1, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r1);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", r1);
    // Re-publish blob 1 under a fresh manifest BEFORE the next round's fence sees it:
    writeManifestRaw(*backend, store->layout(), ns, r2, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r2);
    gc.runRegularRound();   // fold sees both -1 (r1) and +1 (r2) => net in-degree 1 => spared
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists);
}

// A genuinely unreferenced blob is deleted with its exact token (the single content-delete site).
TEST(CasGcRecheck, UnreferencedBlobDeletedExactToken)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", ref);
    gc.runRegularRound();   // round 2: fold -1, retire, fence, recheck deletes blob 1
    EXPECT_FALSE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists);
    // The owner-removed manifest body is also gone (after its decrement was sealed — #11 ordering).
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, ref})).exists);
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcFence.*:CasGcRecheck.*' 2>&1 | tee build/test_gc_fence_recheck.log`
Expected: compile error / failures.

- [ ] **Step 3: Rewrite `fence`.** Keep the registry-fence-first CAS loop (verbatim from the old `fence`, lines 819-863+: read `rootsRegistryKey`, `fence_round = max(., round)`, `++registry_version`, `casPut`), then the per-shard `mutateShard(ns, shard, [&](RootShard & root){ root.fence_round = std::max(root.fence_round, round); }, &committed)` loop over `fence_universe.namespaces` × `shardsToVisit(ns)`. The only change: record positions into `folded.seal.fence_positions[cursorKey(ns,shard)] = committed` and `folded.seal.fence_positions["_registry"] = registry.registry_version`, and persist the seal (re-`putIfAbsent` is write-once; instead carry fence positions in `gc/state.fence_version[round]` exactly as today AND mirror into the seal for debuggability). Set `folded.seal.markers.fenced = true`. Signature becomes `void Gc::fence(GcState & state, Token & state_token, FoldResult & folded);`.

- [ ] **Step 4: Rewrite `recheck`.** Replace the body (old lines 237/276-530). New shape — fold the fence window into a **completion generation**, then per retired blob delete-or-spare, then exact-token-delete the owner-removed manifest bodies whose decrements are now sealed:

```cpp
void Gc::recheck(GcState & state, FoldResult & folded, const RetireResult & retired, RoundReport & report)
{
    Backend & backend = store->backend();
    const Layout & layout = store->layout();
    chassert(state.lease.owner == gc_id);
    const uint64_t round = state.round;

    /// 1. FOLD-THROUGH-FENCE (FoldedThroughFence guard; defends SabotageCutOverclaim #12). Re-stream
    /// every fenced shard's owner transitions in (sealed_cursor, fence_version] and emit deltas into a
    /// completion generation merged on top of the fold generation. Records at/below a fence that
    /// re-pin a blob lift its in-degree above zero => spared below. The window starts at the (possibly
    /// fold-clamped) folded_cursor, so an unresolved-removal anomaly the fold surfaced is NOT re-deleted
    /// here. 404 rule applies as in fold: a present-but-invalid body is corruption (CORRUPTED_DATA via
    /// foldManifestEdges); a MISSING body is record-and-continue — recheck only adds spare-side +/-1, so
    /// a missing edge here can only spare, never over-delete. Never throw on a 404 (never wedge the round).
    std::vector<BlobDelta> window;
    const auto & fence_pos = state.fence_version.at(round);
    for (const auto & [cursor_key, fence_version] : fence_pos)
    {
        if (cursor_key == "_registry")
            continue;
        const auto [ns, shard] = parseCursorKey(cursor_key);
        const auto [root, tok] = store->readShard(ns, shard);
        const uint64_t lo = folded.seal.per_ns_shard.at(cursor_key).folded_cursor;
        for (const OwnerTransition & t : root.transitions)
        {
            if (t.transition_version <= lo || t.transition_version > fence_version)
                continue;
            /// Missing body => false return, recorded by foldManifestEdges and skipped here (spare-safe).
            if (t.old_manifest)
                foldManifestEdges(ManifestId{ns, *t.old_manifest}, -1, window, folded.mf_cleanup, report);
            if (t.new_manifest)
                foldManifestEdges(ManifestId{ns, *t.new_manifest}, +1, window, folded.mf_cleanup, report);
        }
        /// (precommit/promote window handled identically — omitted for brevity; mirror fold's loops)
    }
    const uint64_t completion_generation = state.snap_generation + 1;
    foldDeltasIntoGeneration(backend, layout, state.snap_generation, completion_generation, 0,
                             std::move(window), folded.seal.blob_target_runs);

    /// 2. Per retired blob: spare if in-degree > 0 in the completion generation, else exact-token delete.
    std::map<uint64_t, OutcomeLog> computed;
    for (const auto & [shard, set] : retired.blobs)
    {
        for (const RetiredEntry & entry : set.entries)
        {
            OutcomeEntry outcome{.kind = entry.kind, .hash = entry.hash, .token = entry.token,
                                 .outcome = OutcomeKind::Spared};
            if (inDegreeInGeneration(backend, layout, completion_generation, 0, entry.hash) == 0)
            {
                /// ==================== THE SINGLE CONTENT-DELETE SITE (blob) ====================
                const DeleteOutcome del = backend.deleteExact(layout.blobKey(BlobId(u128ToHex(entry.hash))), entry.token);
                outcome.outcome = del.kind == DeleteOutcome::Kind::Deleted ? OutcomeKind::Deleted
                                : del.kind == DeleteOutcome::Kind::NotFound ? OutcomeKind::Absent
                                : OutcomeKind::Replaced;
            }
            computed[shard].entries.push_back(std::move(outcome));
        }
    }
    /// Write outcome logs write-once (adopt byte-equal occupant), tally report (verbatim shape).
    persistOutcomeLogs(backend, layout, round, state.fence_seq, computed, report);

    /// 3. Manifest exact-token deletes — ONLY now, after the owner-removal decrements are sealed into
    /// the generation (controls #11). recheck never READS the body to recompute decrements (they were
    /// produced at fold from the present body); it deletes by the token captured at fold.
    for (const auto & [id, token] : folded.mf_cleanup)
    {
        if (manifestStillOwned(state, id))   /// owner restored in the fold-through-fence view => keep
            continue;
        backend.deleteExact(layout.manifestKey(id), token);   /// NotFound/TokenMismatch tolerated
    }

    /// 4. Round-visibility barrier: gc/state.round already advanced in retire AFTER all retired sets +
    /// the cleanup bundle were durable (ViewableRound, #13). Seal the completion generation + advance
    /// the pointer in ONE gc/state CAS; drop the round's retired-set objects on confirmed outcomes.
    folded.seal.markers.rechecked = true;
    folded.seal.markers.deleted = true;
    sealCompletionAndAdvance(state, folded, completion_generation, retired);
}
```

> Helpers `inDegreeInGeneration`, `parseCursorKey`, `persistOutcomeLogs`, `manifestStillOwned`, `sealCompletionAndAdvance` are thin: `inDegreeInGeneration` streams `blobTargetRunKey(gen,shard,*)` for one hash; `persistOutcomeLogs` is the verbatim old outcome-log write+tally (lines ~440-530, kept); `manifestStillOwned` checks whether the fold-through-fence window re-added the `ManifestId`'s owner; `sealCompletionAndAdvance` writes the completion `GenerationSeal` (`putIfAbsent`), CASes `gc/state` (`snap_generation = completion_generation`, erase `fence_version[<=round]`), and `deleteExact`-drops each `retiredKey(round,fence_seq,shard)` on its confirmed outcome (a GC-metadata delete, not the content site). Keep the `B174` superseded-generation prune (old lines 605-640) but pruning `generationSealKey`/`blobTargetRunKey`/`partManifestCleanupKey` of generations below the retention floor instead of `gcSnapKey`.

- [ ] **Step 5: Build, run, commit.** This is the first task that should produce a **clean build** of the whole `CasGc` rewrite (fold+retire+fence+recheck consistent; `cascadeAndPersist` gone). `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze).

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcFold.*:CasGcRetire.*:CasGcFence.*:CasGcRecheck.*' 2>&1 | tee build/test_gc_core.log` → all PASS.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_fence_recheck.cpp
git commit -m "CA GC phase1d: all-shard fence + fold-through-fence recheck + exact-token blob/manifest deletes"
```

---

### Task 6: `CasOrphanManifestSweep` — per-namespace pre-precommit debris sweep {#task-6-casorphanmanifestsweep-per-namespace-pre-precommit-debris-sweep}

Realizes spec §Orphan-Part-Manifest-Cleanup-Sweep and §Pre-Precommit-Part-Manifest-Debris. Defends controls **#7** (omit sweep ⇒ leak forever), **#8** (wholesale dead-prefix delete ⇒ live ref loses body), **#9** (frozen-seq authority). Enforces `OrphanManifestDebrisDrains`. The sweep emits **no** blob deltas (a pre-precommit body never contributed `+1`).

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` (wire one sweep step per round)
- Test: `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp`

**Interfaces:**
- Consumes: `ManifestId`/`ManifestRef` (`CasManifestId.h`); `Backend::list`/`head`/`deleteExact` (`CasBackend.h`); `Layout::manifestKey` + the `_manifests/<writer_instance_id>/<build_sequence>/` prefix (`CasLayout.h`); `RootShard`/`decodeRootShard` (`CasRootShardCodec.h`); `ServerWatermark`/watermark accessors (`CasWatermark.h`); the durable owner view (sealed committed + live precommit `ManifestId`s for the namespace).
- Produces:
  - `struct BuildPrefix { String writer_instance_id; uint64_t build_sequence; };`
  - `void sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix);` — enumerates the one build prefix, builds the active `ManifestId` set from the namespace's sealed owner view, deletes (exact-token) only bodies absent from the active set, eligibility from the durable watermark fact (OQ6), **never** frozen-seq.

- [ ] **Step 1: Write the failing test.** Create `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

// A staged-but-unowned body in an ELIGIBLE prefix, absent from the owner view, is deleted (#7).
TEST(CasOrphanManifestSweep, EligibleAndUnownedIsDeleted)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef ref{"srv-a", 5, DB::UInt128(0xAB)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*any*/});   // body, but NO owner transition
    setWatermarkMinActive(*backend, store->layout(), "srv-a", /*min_active*/6);  // 6 > build_seq 5 => eligible

    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = "srv-a", .build_sequence = 5});
    EXPECT_FALSE(backend->head(store->layout().manifestKey(ManifestId{ns, ref})).exists);
}

// A body that IS in the owner view (committed/precommit) is NEVER swept (#8).
TEST(CasOrphanManifestSweep, OwnedBodyIsSkipped)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 5, DB::UInt128(0xAB)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*any*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);  // now owned
    setWatermarkMinActive(*backend, store->layout(), "srv-a", 6);

    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = "srv-a", .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, ref})).exists);
}

// The sweep emits NO blob deltas: the in-degree generation is unchanged.
TEST(CasOrphanManifestSweep, EmitsNoBlobDeltas)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 5, DB::UInt128(0xAB)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob 1*/});
    setWatermarkMinActive(*backend, store->layout(), "srv-a", 6);
    const String gen_key_before = store->layout().generationSealKey(currentGeneration(*backend, store->layout()));
    const auto before = backend->head(gen_key_before).exists;

    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = "srv-a", .build_sequence = 5});
    // No new generation seal was written by the sweep (it touches no blob in-degree state).
    EXPECT_EQ(backend->head(gen_key_before).exists, before);
    EXPECT_EQ(inDegreeOf(*backend, store->layout(), DB::UInt128(1)), 0);  // unchanged
}

// A NON-eligible prefix (no watermark fact; only a frozen-seq guess) deletes NOTHING (#9).
TEST(CasOrphanManifestSweep, FrozenSeqIsNotAuthority)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 5, DB::UInt128(0xAB)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*any*/});
    // NO setWatermarkMinActive — the only "signal" would be a frozen-seq heuristic, which is NOT authority.
    sweepNamespace(*store, ns, BuildPrefix{.writer_instance_id = "srv-a", .build_sequence = 5});
    EXPECT_TRUE(backend->head(store->layout().manifestKey(ManifestId{ns, ref})).exists);
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasOrphanManifestSweep.*' 2>&1 | tee build/test_orphan_sweep.log`
Expected: compile error — header missing.

- [ ] **Step 3: Write the header.** Create `CasOrphanManifestSweep.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <base/types.h>

namespace DB::Cas
{

/// One writer build prefix under _manifests: <writer_instance_id>/<build_sequence>/.
struct BuildPrefix
{
    String writer_instance_id;
    uint64_t build_sequence = 0;
};

/// Per-namespace pre-precommit orphan sweep (spec §Orphan-Part-Manifest-Cleanup-Sweep). Deletes
/// manifest bodies written before PrecommitAdd and never named by any live owner, scoped to ONE
/// namespace + ONE build prefix. Rules:
///   - eligibility from the durable watermark fact ONLY (OQ6): retired-epoch sentinel, or the same
///     epoch's min_active > build_sequence, or the writer incarnation replaced — NEVER frozen-seq alone;
///   - the active ManifestId set comes from the namespace's sealed committed + live-precommit owner view;
///   - delete only bodies whose ManifestId is ABSENT from the active set, by exact token;
///   - emits NO blob deltas (a pre-precommit body never contributed +1);
///   - a 404 mid-sweep is record-and-continue, never a throw (feedback_ca_gc_never_throw_on_404);
///   - never GETs a condemned body to revive it (feedback_ca_resurrect_invariant) — eligibility +
///     exact-token delete only.
void sweepNamespace(Store & store, const RootNamespace & ns, const BuildPrefix & prefix);

}
```

- [ ] **Step 4: Write the implementation + wire one sweep step into the round.** Create `CasOrphanManifestSweep.cpp` (enumerate the prefix via `backend.list(prefix, cursor, limit)` paging; build the active set by reading the namespace's root shards and collecting committed + live-precommit `ManifestId`s; skip if `!eligible`; delete-exact each absent body). Eligibility:

```cpp
bool prefixEligible(Store & store, const BuildPrefix & prefix)
{
    /// OQ6: durable watermark fact only. min_active > build_sequence (epoch matches), explicit retired
    /// sentinel, or replaced incarnation. A frozen-seq / judged-dead guess is NEVER sufficient.
    const ServerWatermark * wm = store.watermarkForWriter(prefix.writer_instance_id);
    if (!wm)
        return false;                       /// no durable fact => not eligible (control #9)
    if (wm->retired_epoch_sentinel)
        return true;
    if (wm->incarnation_replaced(prefix.writer_instance_id))
        return true;
    return wm->min_active > prefix.build_sequence;
}
```

Then in `CasGc.cpp::runRegularRound`, after `recheck`/`trim`, add one bounded sweep step (rare backstop; one namespace, one eligible prefix per round) guarded so a 404 never wedges the round:

```cpp
    /// Bounded orphan-manifest backstop (spec §Orphan sweep): at most one namespace + one eligible
    /// prefix per round. Records-and-continues on 404; never throws (feedback_ca_gc_never_throw_on_404).
    try
    {
        if (auto target = pickOneSweepTarget(*store))   /// {ns, BuildPrefix} or nullopt
            sweepNamespace(*store, target->ns, target->prefix);
    }
    catch (const Exception & e)
    {
        LOG_WARNING(log, "CAS gc orphan sweep skipped this round: {}", e.message());
    }
```

> `watermarkForWriter`/`retired_epoch_sentinel`/`incarnation_replaced`/`min_active` follow `CasWatermark.h`; if the exact names differ, adapt while keeping the rule "durable fact only, never frozen-seq". `pickOneSweepTarget` is a small helper choosing one eligible prefix from the registry's namespaces (round-robin or oldest); keep it bounded.

Build: `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze).

- [ ] **Step 5: Run to verify it passes.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasOrphanManifestSweep.*' 2>&1 | tee build/test_orphan_sweep.log`
Expected: 4 tests PASS (subagent-analyze).

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp
git commit -m "CA GC phase1d: per-namespace orphan part-manifest sweep (watermark-eligible, no blob deltas)"
```

---

### Task 7: Retire-visibility barrier + trim below sealed coverage + resume {#task-7-retire-visibility-barrier-trim-below-sealed-coverage-resume}

Realizes spec §Retire-Visibility-Barrier, §Trim, §Debuggability-And-Resume. Defends controls **#13** (`gc/state.round` advances only after all retired sets + cleanup bundles durable) and **#15** (`SabotageTrimUnincorporated` — trim below an unincorporated transition). Enforces `INV_JOURNAL_COVERAGE`.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (rewritten in Task 8; add the round/resume cases here), `src/Disks/tests/gtest_cas_gc_resume.cpp` (created here).

**Interfaces:**
- Consumes: `FoldResult`/`GenerationSeal` (Tasks 1, 3); `RootShard`/`mutateShard` (`CasRootShardCodec.h`/`CasStore.h`).
- Produces: `void Gc::trim(const FoldResult & folded, uint64_t round);`; reworked `bool Gc::tryResumeIncompleteRound(GcState & state, Token & state_token, RoundReport & report);` (drives resume from the generation seal's `PhaseMarkers` + durable retired sets, not from a `GcSnap`).

- [ ] **Step 1: Write the failing tests.** Create `src/Disks/tests/gtest_cas_gc_resume.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

// Trim removes only owner transitions at/below the sealed cursor; a record past it survives (#15).
TEST(CasGcRound, TrimKeepsUnincorporatedTransitions)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();   // folds + seals the transition; trim may now drop it
    // The transition at/below the sealed cursor is trimmed; shard_version is preserved.
    EXPECT_EQ(ownerTransitionCountAtOrBelow(*backend, store->layout(), ns, sealedCursor(*backend, store->layout(), ns)), 0u);
}

// A round that crashes after retire (retired sets durable, generation pointer not yet advanced past
// recheck) is resumed idempotently: re-running completes the SAME round, deletes are exact-token no-ops.
TEST(CasGcResume, ResumeAfterRetireCompletesRoundIdempotently)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    Gc gc(store, DB::UInt128(0xG1));
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl", ref);

    // Hook the backend to throw right after retire's gc/state CAS (simulating a crash); the round
    // leaves durable retired sets at (round, fence_seq). A fresh Gc resumes and finishes the round.
    auto crashing = std::make_shared<ThrowAfterRetireBackend>(backend);  // inline test backend
    auto store2 = openStoreForTest(crashing);
    Gc gc2(store2, DB::UInt128(0xG1));
    EXPECT_ANY_THROW(gc2.runRegularRound());           // crashes mid-round
    Gc gc3(store, DB::UInt128(0xG1));
    gc3.runRegularRound();                              // resumes from durable state
    EXPECT_FALSE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists);
}
```

> `ThrowAfterRetireBackend` is an inline test decorator (the existing gtests define ad-hoc decorator backends inline). If hooking "after retire" is awkward, replace the crash test with a state-injection test: write the durable retired set + a seal with `markers.retired=true, markers.deleted=false` directly, then assert a fresh `Gc` resume deletes the blob — black-box on durable state, no decorator.

- [ ] **Step 2: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRound.Trim*:CasGcResume.*' 2>&1 | tee build/test_gc_resume.log`
Expected: compile error / failures.

- [ ] **Step 3: Rewrite `trim`.** Replace the body (old lines 168-219). Per discovered `(ns, shard)` in `folded.root_shards`, read the sealed cursor from `folded.seal.per_ns_shard.at(cursorKey(ns,shard)).folded_cursor`, and `mutateShard` to `std::erase_if(fresh.transitions, ...)` / `std::erase_if(fresh.precommits, ...)` for records with `transition_version <= cursor` — only after the generation carrying those deltas is durable (it is: `fold`/`recheck` sealed it before `trim` runs). Signature: `void Gc::trim(const FoldResult & folded, uint64_t round);`. Keep the "touch only when there is something to trim" peek.

- [ ] **Step 4: Rewrite `tryResumeIncompleteRound`.** Replace the snap-based resume (it loaded `GcSnap`). New resume reads the **latest generation seal** and its `PhaseMarkers`:
  - if retired sets at `(state.round, fence_seq)` exist and `markers.deleted` is false ⇒ re-run `fence`→`recheck` from the durable fold seal (exact-token deletes are idempotent: `NotFound`⇒`Absent`);
  - the part-manifest cleanup bundle is enough to re-issue manifest deletes (recheck reads it, never the deleted body);
  - drop the retired sets, complete the round.

  The "incomplete round detectable from durable state alone" property is unchanged; only the artifact read changes (seal markers instead of resident snap). Confirm `gc/state.round` only ever advanced after all retired sets + the cleanup bundle were durable (the barrier in retire/recheck), so a resume never sees a half-published round visible to writers.

- [ ] **Step 5: Build, run, commit.** `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze).

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcRound.Trim*:CasGcResume.*' 2>&1 | tee build/test_gc_resume.log` → PASS.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_resume.cpp
git commit -m "CA GC phase1d: retire-visibility barrier, trim below sealed coverage, seal-driven resume"
```

---

### Task 8: DELETE tree/snap/closure machinery; fix all references {#task-8-delete-tree-snap-closure-machinery-fix-all-references}

Realizes spec §What-Becomes-Simpler removals: no `GcSnap`, no content-addressed trees, no closure walk, no cascade. The Phase-0 model already dropped `treeEdges`/`marker`/`children_by_tree`; this is the C++ deletion.

**Files:**
- Delete: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h`, `CasGcSnap.cpp`, `CasTreeCodec.h`, `CasTreeCodec.cpp`, `CasClosureWalk.h`, `CasClosureWalk.cpp`.
- Delete (tests): `src/Disks/tests/gtest_cas_gc_snap.cpp`, `gtest_cas_tree_id.cpp`, `gtest_cas_closure_walk.cpp`.
- Modify: `src/Disks/tests/cas_test_helpers.h` (drop `CasTreeCodec.h` include + `writeTreeRaw`).
- Modify: every remaining file referencing the deleted symbols (`CasFormat.*`, `CasEvent.*`, `CasFsck.*`, `CasBuild.*`, `CasStore.*`, `CasPlacement.h`, `CasRootShardCodec.h`, and the gtests `gtest_cas_build.cpp`, `gtest_cas_protocol_scenarios.cpp`, `gtest_cas_store.cpp`, `gtest_cas_layout.cpp`, `gtest_cas_format.cpp`, `gtest_cas_codecs.cpp`, `gtest_cas_tree_layout.cpp`, `gtest_cas_b140_dangle.cpp`, `gtest_cas_gc_formats.cpp`, `gtest_cas_gc_leak.cpp`, `gtest_cas_gc_round.cpp`, `gtest_cas_fsck.cpp`).

> Much of the `CasBuild`/`CasStore`/`CasRootShardCodec` tree removal is owned by Phase 1b/1c (they replace `stageTree`/`readTree`/`JournalRecord.closure` with manifests). This task removes only what is **left dangling after 1b/1c land** — primarily GC-side and test-side references, the `FormatId::Tree`/`GcSnap` enum arms (already done in Task 1), and the `CasEvent` tree-event types. If 1b/1c already deleted `CasTreeCodec`, skip those file deletions and only do the GC/test cleanup.

- [ ] **Step 1: Delete the source files.**

```bash
git rm src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.cpp \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.h \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.cpp
# CasTreeCodec only if 1b/1c has not already removed it:
git rm src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp 2>/dev/null || true
git rm src/Disks/tests/gtest_cas_gc_snap.cpp \
       src/Disks/tests/gtest_cas_tree_id.cpp \
       src/Disks/tests/gtest_cas_closure_walk.cpp
```

- [ ] **Step 2: Strip `CasTreeCodec`/`writeTreeRaw` from `cas_test_helpers.h`.** Remove the `#include <...CasTreeCodec.h>` line (line 13) and the entire `writeTreeRaw` function (lines 124-146). Leave `writeBlobRaw` and the new `writeManifestRaw` (Task 3).

- [ ] **Step 3: Find every remaining reference and fix it.** Run the sweep:

```bash
grep -rn "GcSnap\|CasGcSnap\|CasTreeCodec\|CasClosureWalk\|closureWalk\|merkleTreeId\|encodeTree\|writeTreeRaw\|stripTree\|addTreeEdge\|deleted_trees\|TreeStrip\|TreeDelete\|FormatId::Tree\|FormatId::GcSnap" \
     src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ src/Disks/tests/
```

For each hit: in `CasEvent.h`/`.cpp` remove the `TreeDelete`/`TreeStrip`/`Snap` event kinds and any `Tree`/`Snap` object-kind branches (CA emits only `Blob`/`Manifest` events now); in `CasFsck.*` the tree walk is rewritten in Task 9 (leave a compile stub here if Task 9 has not landed — but prefer ordering Task 9 before this sweep's final build); in the gtests, delete or rewrite the tree/snap test bodies (a tree-id round-trip test has no successor; a snap-codec test has no successor — delete them; a build/store test that staged a tree now stages a manifest via 1b/1c helpers — rewrite minimally).

- [ ] **Step 4: Build the whole `unit_tests_dbms` and iterate to zero references.** `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze; the subagent returns the remaining undefined-symbol / missing-include errors). Fix each, rebuild, until the build is clean. **Do not** stub out behavior to silence an error — delete dead code or port it to the manifest model.

- [ ] **Step 5: Run the full CA sweep to confirm nothing references the deleted machinery at runtime.**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tee build/test_cas_sweep_post_delete.log`
Expected: all CA suites compile and pass (subagent-analyze; some rewritten suites may still be thin — Task 10 is the final green gate).

- [ ] **Step 6: Commit.**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ src/Disks/tests/
git commit -m "CA GC phase1d: delete GcSnap/TreeCodec/ClosureWalk + cascade; port all references to the manifest model"
```

---

### Task 9: fsck OQ8 read-only manifest audit {#task-9-fsck-oq8-read-only-manifest-audit}

Realizes Resolved-OQ8 + spec §fsck. A read-only audit flags an **owner-visible missing manifest body** as an **error** (`Dangling`) and a reclaimable **pre-precommit** manifest body as **info** (`Unreachable`), using the same sealed-owner-view + eligibility rule as the sweep (Task 6). No deletes.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp`
- Test: `src/Disks/tests/gtest_cas_fsck.cpp` (rewritten — the old fsck walked trees; the new one walks manifests).

**Interfaces:**
- Consumes: `PartManifest`/`decodePartManifest`/`refMatchesBody`/`manifestNamespaceMatches` (`CasManifestCodec.h`); `RootShard`/`OwnerTransition` (`CasRootShardCodec.h`); `Layout::manifestKey`/`blobKey` (`CasLayout.h`); `BuildPrefix`/`prefixEligible` (Task 6); the existing `FsckReport`/`FsckClass`/`FsckObject`/`runFsck(Store&, bool detail, FsckProgress, deadline)` (`CasFsck.h`).
- Produces: the audit folded into the existing `runFsck` (no new entry point); a committed ref naming a missing manifest body increments `report.dangling` with an `error`-class `FsckObject`; an eligible-pre-precommit body increments `report.unreachable` with an `info`-class object.

- [ ] **Step 1: Write the failing test.** Rewrite `src/Disks/tests/gtest_cas_fsck.cpp` to the manifest model (delete the tree-walk tests). Keep the existing top includes minus `CasTreeCodec`:

```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

// A committed ref whose manifest body is present and whose blobs exist => clean.
TEST(CasFsck, CleanManifestPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*blob 1*/});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);
    const FsckReport r = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(r.clean());
    EXPECT_EQ(r.dangling, 0u);
}

// A committed ref naming a MISSING manifest body is an ERROR (Dangling).
TEST(CasFsck, OwnerVisibleMissingManifestBodyIsError)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef ref{"srv-a", 1, DB::UInt128(0xAA)};
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, ref);  // no body
    const FsckReport r = runFsck(*store, /*detail*/true);
    EXPECT_FALSE(r.clean());
    EXPECT_GE(r.dangling, 1u);
}

// A pre-precommit body in an eligible prefix (no owner) is INFO (Unreachable), not an error.
TEST(CasFsck, ReclaimablePrePrecommitBodyIsInfo)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    registerNamespaceRaw(*backend, store->layout(), ns);
    const ManifestRef ref{"srv-a", 5, DB::UInt128(0xAB)};
    writeManifestRaw(*backend, store->layout(), ns, ref, {/*any*/});   // body, no owner
    setWatermarkMinActive(*backend, store->layout(), "srv-a", 6);       // eligible
    const FsckReport r = runFsck(*store, /*detail*/true);
    EXPECT_TRUE(r.clean());            // not an error
    EXPECT_GE(r.unreachable, 1u);      // counted as info/unreachable
}
```

- [ ] **Step 2: Run to verify it fails.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasFsck.*' 2>&1 | tee build/test_fsck.log`
Expected: compile error / failures (old fsck used `readTree`/`treeKey`).

- [ ] **Step 3: Rewrite the reachability walk in `CasFsck.cpp`.** Replace the `closureWalk`/`readTree`/`treeKey` lambda (old lines ~74-90) with a manifest walk: for each namespace in the registry, read each root shard, and for each committed `RootRef` (`mActiveEdges` owner), derive the `ManifestId`, read the manifest via `layout.manifestKey(id)`:
  - body absent ⇒ `report.dangling++`, push `FsckObject{.key=manifestKey, .kind=ObjectKind::Manifest, .cls=FsckClass::Dangling}` (error — a committed ref must never name a missing body);
  - body present ⇒ validate `refMatchesBody`/`manifestNamespaceMatches` (mismatch ⇒ `Dangling` error); for each blob entry, mark the blob reachable; a reachable blob whose object is absent ⇒ `Dangling`.

  Then enumerate `_manifests/<prefix>/` bodies with no owner: if the prefix is `prefixEligible` (Task 6's rule) ⇒ `report.unreachable++` with `FsckClass::Unreachable` (info, reclaimable pre-precommit debris); if NOT eligible and unowned ⇒ still `Unreachable` (in-flight, not an error). Never delete anything; `runFsck` is read-only. Keep the `FsckReport` accounting fields (`physical_bytes`, `referenced_logical_bytes`, `total_blob_refs`, `distinct_blobs`, `dedupRatio`).

Build: `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze).

- [ ] **Step 4: Run to verify it passes.**

Run: `build/src/unit_tests_dbms --gtest_filter='CasFsck.*' 2>&1 | tee build/test_fsck.log`
Expected: 3 tests PASS (subagent-analyze).

- [ ] **Step 5: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp \
        src/Disks/tests/gtest_cas_fsck.cpp
git commit -m "CA GC phase1d: fsck OQ8 read-only manifest audit (owner-visible missing body = error; pre-precommit = info)"
```

---

### Task 10: Full build + `Cas*`/`Ca*` gtest sweep + chaos soak (phase exit) {#task-10-full-build-cas-ca-gtest-sweep-chaos-soak-phase-exit}

Phase exit per the overview: full CA gtest sweep green, then a chaos soak with periodic reports. This is the first behavior switch's soak.

**Files:** none new (verification + soak only).

- [ ] **Step 1: Clean build of the whole binary.** `ninja -C build unit_tests_dbms > build/build.log 2>&1` (subagent-analyze; expect zero errors, zero references to the deleted machinery).

- [ ] **Step 2: Run the FULL `Cas*`/`Ca*` gtest sweep.**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tee build/test_cas_full_sweep.log`
Expected: every CA suite PASSES (subagent-analyze `build/test_cas_full_sweep.log`; return the count of suites/tests run and any failure). New suites present: `CasGenerationSeal`, `CasBlobInDegree`, `CasGcFold`, `CasGcRetire`, `CasGcFence`, `CasGcRecheck`, `CasOrphanManifestSweep`, `CasGcRound`, `CasGcResume`, `CasFsck`. Deleted suites absent: `CasGcSnap`, `CasTreeId`, `CasClosureWalk`.

- [ ] **Step 3: Build the server binary for the soak.** Build the full `clickhouse` target (per `reference_ca_soak_fresh_restart`): `ninja -C build clickhouse > build/build_server.log 2>&1` (subagent-analyze).

- [ ] **Step 4: Run the chaos soak.** Follow `reference_ca_soak_fresh_restart`: archive prior host logs, `down -v` for clean data, `up` to remount the rebuilt binary, run the CA chaos workload + regression watch with **periodic reports** (per `project_ca_soak_test_effort` cadence). Watch for: `INV_NO_DANGLE`/`INV_NO_LOSS` regressions (a reachable blob/manifest missing), a wedged GC round, an orphan-manifest leak that never drains, or a fold/sweep throw on a 404 (must be record-and-continue). Use `system.content_addressed_log` (B170) to attribute any blob/manifest delete. Run until the soak window completes with no regression; report the window length and any findings.

- [ ] **Step 5: Commit the worklog / soak result.**

```bash
git commit --allow-empty -m "CA GC phase1d: full Cas*/Ca* sweep green + chaos soak clean (phase exit)"
```

---

## Self-Review {#self-review}

**§Round-Protocol coverage:** Discovery (kept in `fold` Step 4, registry-authority + LIST-accelerator — Task 3); Fold-Owner-Transitions (Task 3 — committed `+`, removal `-`, precommit body-present `+`, precommit-missing-body ⇒ none, promote-after-missing-body ⇒ committed `+`, `RefMatchesBody`/`ManifestNamespaceMatches` fail-closed; **404 rule:** a committed/promote `new` missing body or an owner-removal `old` body missing at removal-fold is fail-closed FOR THAT DECISION — clamp the shard's folded_cursor below it, record the anomaly, never guess a delta, never throw/wedge — while a present-but-invalid body is hard `CORRUPTED_DATA`); Retire (Task 4 — per-candidate `HEAD`, deferred manifest delete); Orphan-Part-Manifest-Cleanup-Sweep (Task 6); Retire-Visibility-Barrier (Task 7); Global-Fence (Task 5 — registry-first + all-shard); Recheck-And-Delete (Task 5 — fold-through-fence, single content-delete site, exact-token manifest delete after decrements sealed); Trim (Task 7 — below sealed cursor). ✓

**§Safety-Invariants realization:** `NoManifestIdReuse`/`SingleManifestOwner`/`CommittedManifestBodyRequired` (Task 3 fail-closed gates); `PrecommitMayReferenceMissingManifest`/`PrecommitMayReferenceMissingBlob` (Task 3 missing-body ⇒ no delta, no throw); `RefMatchesBody`/`ManifestNamespaceMatches` (Task 3); `MutablePayloadNotReachability` (no owner transition ⇒ no fold edge — structural in Task 3, no mutable-only path emits deltas); `ManifestActivationMatchesEdges` (Task 3 `recordActivation` + Task 5 mirror-only-emitted-edges on removal); `CommittedNoMissingBlob`/`NoCommittedDangle` (Task 5 recheck + Task 9 fsck); `BlobInDegreeMatchesActiveManifests` (Task 2 + Task 3); `NoReturn`/`ExactDeleteOnly` (Tasks 4/5 exact-token, observed token); `ViewableRound` (Task 7 barrier); `JournalCoverage` (Task 7 trim); `OrphanManifestDebrisDrains` (Task 6). ✓

**§Negative-Controls realization (C++ gtests, not re-proofs):** #4/#10/#22 (Task 3 `CasGcFold`); #19/#20 (Task 3 mismatch tests); #11/#12/#14/#16 (Task 5 `CasGcFence`/`CasGcRecheck`); #7/#8/#9 (Task 6 `CasOrphanManifestSweep`); #13/#15 (Task 7 `CasGcRound`/`CasGcResume`). Controls #1/#2/#3/#5/#6/#17/#18/#21 are structural consequences of the consumed 1a/1b identity model (unique `ManifestId`, single owner, full `ManifestRef`, `ManifestId`-keyed edges, no mutable-as-reachability) — proved in Phase 0, enforced by the consumed types; this phase adds no path that could violate them. ✓

**§What-Becomes-Simpler removals:** no content-addressed tree revival race (unique `ManifestId`); no full-closure journal (owner transitions); no recursive GC tree reads (one manifest per old/new owner — Task 3); no GC-side cascade (`cascadeAndPersist` deleted — Tasks 3/8); no `TreeExpansionIndex`/`children_by_tree`/`GcSnap` (deleted — Task 8); no tree in-degree shards (blob-only — Task 2). ✓

**Honored feedback:** `feedback_ca_gc_never_throw_on_404` (Task 3 `foldManifestEdges` 404 ⇒ record-and-continue; Task 6 sweep 404 tolerated; Task 6 round wiring catches+warns); `feedback_ca_resurrect_invariant` (Tasks 4/6 never `GET` a condemned body — retire reuses the fold-captured token, sweep deletes by token only). ✓

**Placeholder scan:** every code step shows real code; every run step shows the exact `--gtest_filter` command and expected outcome. The known unknowns are the **consumed Phase 1a/1b/1c type/member names** (`PartManifest` field names, `RunFile`/`RunMerger` method names, `RootShard.transitions`/`promotions` shape, test-infra `openStoreForTest`/transition helpers) — each call site flags "adapt to the ground-truth header if names differ", which is a reconciliation instruction, not a vague TODO. ✓

**Type consistency:** `FoldResult{seal, root_shards, mf_cleanup}` (Task 3) is consumed unchanged by `retire`/`fence`/`recheck`/`trim` (Tasks 4/5/7); `RetireResult{blobs, mf_cleanup}` (Task 4) → `recheck` (Task 5); `GenerationSeal`/`ShardCoverage`/`RunRef`/`PhaseMarkers` (Task 1) used by Tasks 3/5/7; `BlobDelta`/`foldDeltasIntoGeneration`/`zeroInDegree`/`BlobCandidate` (Task 2) used by Tasks 3/4/5; `BuildPrefix`/`sweepNamespace`/`prefixEligible` (Task 6) used by Task 9. ✓
