# CA Precommit Inline-Closure (B199-S2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the never-expanded-tree GC leak (B199-S2) by construction: the precommit ref carries its staged closure inline, so GC expands a precommit from that inline data (never reading the tree object), and the existing `Remove`→cascade (+ B199-S1) releases the closure on abandon.

**Architecture:** Add an inline `closure` (the staged `[TreeEntry]` per tree node) to the precommit `RefPayload`. Extract the three near-duplicate closure traversals into ONE `walk(root, entriesOf, visit)` parameterized by a child *source* (`inline` | `backend=readTree`) and a *visitor* (`fold-addEdge` | `fsck-reachable`). On folding a precommit `Add`, expand via the walk with the inline source — identical snap state to a committed expansion, but no `readTree` and no 404 — then delete the precommit tree-read/pending-tolerance branch. Reclaim is unchanged (existing `Remove`→`removeRootEdge`→cascade; B199-S1 covers the absent-tree case). The traversal unification also fixes a latent fold-expand subtree-recursion gap.

**Tech Stack:** C++ (ClickHouse `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), protobuf (`cas_root_shard.proto`), gtest (`src/Disks/tests/`), TLA+/TLC (`docs/superpowers/models/`). Build dir `build/`; unit binary `build/src/unit_tests_dbms`. Branch `cas-vfs-path-mapping` (NOT master).

**Spec:** `docs/superpowers/specs/2026-06-23-ca-precommit-inline-closure-design.md`.

---

## File structure (what each touched file is responsible for)

- `Core/CasTreeCodec.h` — `TreeEntry` (already defined: `name`, `placement`, `file_hash`, `file_size`, `inline_bytes`, `pack_hash`, `pack_offset`, `pack_length`). No change; consumed.
- `cas_root_shard.proto` + `Core/CasRootShardCodec.{h,cpp}` — `RefPayload` gains a nested `closure` (a list of `{tree_hash, [entry]}` nodes). Codec encodes/decodes it (additive field, no `codec_version` bump).
- `Core/CasClosureWalk.{h,cpp}` — NEW: the single `walk(root, entriesOf, visit)` traversal + the two source adapters and the visitor typedef. One responsibility: recurse a manifest closure given a child source.
- `Core/CasGc.cpp` — `foldShardRecords` calls `walk(..., backendSource)` for table refs and `walk(..., inlineSource)` for precommit refs; the precommit `readTree`/pending-tolerance branch is deleted. `reclaimAbandonedPrecommit` unchanged.
- `Core/CasFsck.cpp` — `walk` lambda replaced by a call into `CasClosureWalk` with the backend source + a reachability visitor.
- `Core/CasBuild.cpp` — `Build::precommit` fills `RefPayload.closure` from the staged tree structure.
- `docs/superpowers/models/CaBuildRootPrecommit.tla` — model the inline-closure protect/reclaim; TLC gate.
- `src/Disks/tests/gtest_cas_*.cpp` — codec round-trip, walk unit tests, precommit-populates-closure, S2 green, nested-manifest recursion.

---

## Task 1: TLA+ gate — model inline-closure protect/reclaim

**Files:**
- Modify: `docs/superpowers/models/CaBuildRootPrecommit.tla`
- Modify/Create: `docs/superpowers/models/CaBuildRootPrecommit_RESULTS.md`

This is the operator-required gate: TLC must be clean before the C++ lands.

- [ ] **Step 1: Read the current model and its config.** Read `CaBuildRootPrecommit.tla` end to end and note: the actions (`Precommit`, `Commit`, `GcReclaimPrecommit`, `FailClosedCommit`, `BuildFreeze`), the `BuildRootProtected` predicate, and `INV_BUILDROOT_PROTECTS` / `INV_NO_DANGLE` / `INV_NO_LOSS`. Note the TLC config block (CONSTANTS, INVARIANTS) at the bottom or in the `.cfg`.

- [ ] **Step 2: Add the closure dimension.** Extend the model so a precommit records a `closure` set of object ids (blobs/subtrees) — i.e. on `Precommit`, the protection edges are seeded from the recorded `closure`, NOT derived by reading a tree object. Add a variable for "object uploaded?" so a closure member may be `recorded ∧ ¬uploaded` (the partial-build case). On `GcReclaimPrecommit`, drop the closure edges (mirror), and let an un-uploaded member's delete be a no-op (idempotent).

- [ ] **Step 3: Add the no-leak property.** Add a liveness/eventual property `INV_NO_LEAK`: for an abandoned build, every closure member with no other live reference is eventually NOT present (reclaimed). Keep `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`.

- [ ] **Step 4: Run TLC.** Use the same runbook as the other models (see `CaIncarnationCore_RESULTS.md` for the TLC invocation pattern — typically `tlc -config <model>.cfg <model>.tla` with bounded CONSTANTS). Run with small bounds (≤2 builds, ≤2 objects, ≤2 GC rounds).

Run: the model-check command from the existing RESULTS runbook.
Expected: **no invariant violations, no deadlock** within bounds.

- [ ] **Step 5: Record results.** Append a section to `CaBuildRootPrecommit_RESULTS.md`: the new variables/actions, the `INV_NO_LEAK` property, the bounds, and the clean-run state count. If TLC finds a counterexample, STOP and surface it — the design has a hole; do not proceed to code.

- [ ] **Step 6: Commit.**
```bash
git add docs/superpowers/models/CaBuildRootPrecommit.tla docs/superpowers/models/CaBuildRootPrecommit_RESULTS.md
git commit -m "CA B199-S2 TLA+: model precommit inline-closure protect/reclaim + INV_NO_LEAK; TLC clean

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `RefPayload.closure` field — proto + codec

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/cas_root_shard.proto`
- Modify: `Core/CasRootShardCodec.h` (the `RefPayload` struct), `Core/CasRootShardCodec.cpp` (encode/decode)
- Test: `src/Disks/tests/gtest_cas_root_shard_codec.cpp` (existing codec test file; if absent, add to `gtest_cas_build.cpp`)

- [ ] **Step 1: Write the failing codec round-trip test.**
```cpp
TEST(CasRootShardCodec, RefPayloadClosureRoundTrips)
{
    using namespace DB::Cas;
    RootShard in;
    in.shard_version = 7;
    RefPayload pl;
    pl.tree_id = u128Of("T");
    pl.tree_size = 55;
    // nested closure: tree T -> {Blob B1, Subtree S}; S -> {Blob B2}
    pl.closure = {
        { u128Of("T"), { TreeEntry{.placement=Placement::Blob,    .file_hash=u128Of("B1"), .file_size=52},
                         TreeEntry{.placement=Placement::Subtree, .file_hash=u128Of("S"),  .file_size=10} } },
        { u128Of("S"), { TreeEntry{.placement=Placement::Blob,    .file_hash=u128Of("B2"), .file_size=3} } },
    };
    in.refs["4815"] = pl;
    const RootShard out = decodeRootShard(encodeRootShard(in));
    ASSERT_TRUE(out.refs.contains("4815"));
    EXPECT_EQ(out.refs.at("4815").closure, pl.closure);   // requires operator== on the closure node type
}
```

- [ ] **Step 2: Run it — fails to compile** (`closure` member does not exist).
Run: `ninja -C build unit_tests_dbms > build/build_b199s2_t2.log 2>&1; echo $?` → nonzero.

- [ ] **Step 3: Add the C++ field.** In `Core/CasRootShardCodec.h`, add a closure node type and the field to `RefPayload`:
```cpp
/// One tree node of a precommit's inline closure: the node's hash and its staged entries.
struct ClosureNode
{
    UInt128 tree_hash{};
    std::vector<TreeEntry> entries;          // TreeEntry from CasTreeCodec.h
    bool operator==(const ClosureNode &) const = default;
};

struct RefPayload
{
    UInt128 tree_id{};
    uint64_t tree_size = 0;
    std::map<String, String> mutable_files;
    std::vector<ClosureNode> closure;        // NEW: populated only for precommit refs
};
```
Add `#include` for `CasTreeCodec.h` if not already present.

- [ ] **Step 4: Add the proto field.** In `cas_root_shard.proto`, add (ADDITIVE rule — new field numbers, NO `codec_version` bump):
```proto
message TreeEntryProto {
  uint32 placement = 1;        // mirrors Placement enum
  bytes  file_hash = 2;        // 16 bytes
  uint64 file_size = 3;
  bytes  pack_hash = 4;        // 16 bytes (PackSlice only)
}
message ClosureNode {
  bytes tree_hash = 1;
  repeated TreeEntryProto entries = 2;
}
message RefPayload {
  bytes tree_id = 1;
  uint64 tree_size = 2;
  map<string, string> mutable_files = 3;
  repeated ClosureNode closure = 4;          // NEW (field number 4 is unused today)
}
```
(`inline_bytes`/`name`/`pack_offset`/`pack_length` are intentionally NOT serialized — the closure only feeds the GC walk, which needs `placement` + `file_hash`/`pack_hash` + `file_size`. The inline source synthesizes `TreeEntry` with empty `name`/`inline_bytes`.)

- [ ] **Step 5: Encode/decode in `CasRootShardCodec.cpp`.** In the RefPayload encode path, for each `ClosureNode` write `tree_hash` + each entry's `placement`/`file_hash`/`file_size`/`pack_hash`. In decode, reconstruct `std::vector<ClosureNode>` (entries get empty `name`/`inline_bytes`). Mirror the existing `tree_id` raw-16-byte handling.

- [ ] **Step 6: Run the test — passes.**
Run: `ninja -C build unit_tests_dbms > build/build_b199s2_t2.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasRootShardCodec.*' 2>&1 | tail -5`
Expected: `RefPayloadClosureRoundTrips` PASS; existing codec tests still PASS.

- [ ] **Step 7: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/cas_root_shard.proto src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.* src/Disks/tests/gtest_cas_root_shard_codec.cpp
git commit -m "CA B199-S2: add inline closure (nested staged entries) to RefPayload + protobuf codec (additive)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Unified `walk(root, entriesOf, visit)` + refactor fold-expand & fsck onto it

**Files:**
- Create: `Core/CasClosureWalk.h`, `Core/CasClosureWalk.cpp`
- Modify: `Core/CasGc.cpp` (`foldShardRecords` expansion at ~1268-1365), `Core/CasFsck.cpp` (`walk` lambda at ~71-113)
- Test: `src/Disks/tests/gtest_cas_closure_walk.cpp` (new)

- [ ] **Step 1: Write the failing walk unit test** (recursion + dedup + placements, both sources):
```cpp
TEST(CasClosureWalk, RecursesSubtreesDedupsAndYieldsAllObjects)
{
    using namespace DB::Cas;
    // inline source: T -> {Blob B1, Subtree S}; S -> {Blob B2, Blob B1(shared)}
    std::map<UInt128, std::vector<TreeEntry>> nodes = {
        { u128Of("T"), { {.placement=Placement::Blob,.file_hash=u128Of("B1")},
                         {.placement=Placement::Subtree,.file_hash=u128Of("S")} } },
        { u128Of("S"), { {.placement=Placement::Blob,.file_hash=u128Of("B2")},
                         {.placement=Placement::Blob,.file_hash=u128Of("B1")} } },
    };
    auto inlineSource = [&](const UInt128 & node) -> std::vector<TreeEntry> {
        auto it = nodes.find(node); return it == nodes.end() ? std::vector<TreeEntry>{} : it->second;
    };
    size_t edge_calls = 0;
    std::set<UInt128> visited_trees;
    closureWalk(u128Of("T"), inlineSource,
        /*on_tree=*/[&](const UInt128 & t){ visited_trees.insert(t); },
        /*on_edge=*/[&](const UInt128 & /*parent*/, const TreeEntry & /*e*/){ ++edge_calls; });
    // both trees expanded once (dedup), every non-Inline child edge visited:
    EXPECT_EQ(visited_trees, (std::set<UInt128>{u128Of("T"), u128Of("S")}));
    // edges: T->B1, T->S(subtree), S->B2, S->B1  == 4
    EXPECT_EQ(edge_calls, 4u);
}
```

- [ ] **Step 2: Run — fails to compile** (`closureWalk` undefined).
Run: `ninja -C build unit_tests_dbms > build/build_b199s2_t3.log 2>&1; echo $?` → nonzero.

- [ ] **Step 3: Implement `CasClosureWalk`.**
`CasClosureWalk.h`:
```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <base/types.h>
#include <functional>
#include <set>
#include <vector>
namespace DB::Cas
{
/// Source: given a tree node, return its child entries. Inline (precommit) or backend (readTree).
using EntriesOf = std::function<std::vector<TreeEntry>(const UInt128 & node)>;
/// Visitors invoked during the walk.
using OnTree = std::function<void(const UInt128 & tree)>;
/// Called once per non-Inline child entry. The full TreeEntry is passed so visitors can read
/// placement (→ ObjectKind), file_hash/pack_hash, and file_size (fsck bookkeeping).
using OnEdge = std::function<void(const UInt128 & parent_tree, const TreeEntry & entry)>;

/// One closure traversal: DFS from `root`, dedup trees via a seen-set, recurse on Subtree, ignore Inline.
/// `entriesOf` decides where children come from; `on_tree`/`on_edge` decide what to do per node/edge.
void closureWalk(const UInt128 & root, const EntriesOf & entries_of, const OnTree & on_tree, const OnEdge & on_edge);
}
```
`CasClosureWalk.cpp`: recursive impl with a `std::set<UInt128> seen`; for each entry switch on `placement` (`Blob`→on_edge Blob; `PackSlice`→on_edge Pack with `pack_hash`; `Subtree`→on_edge Tree then recurse; `Inline`→skip). Call `on_tree(node)` before iterating, after the `seen.insert` guard. Match the placement handling in `CasFsck.cpp:81-112` exactly.

- [ ] **Step 4: Run the walk test — passes.**
Run: `ninja -C build unit_tests_dbms > build/build_b199s2_t3.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasClosureWalk.*' 2>&1 | tail -5` → PASS.

- [ ] **Step 5: Refactor `CasFsck.cpp` `walk` onto `closureWalk`.** Replace the recursive lambda (`CasFsck.cpp:71-113`) with a `closureWalk(tree_id, backendSource, on_tree, on_edge)` where `backendSource = [&](node){ return store.readTree(node); }`, `on_tree` records the tree key reachable, `on_edge(parent, entry)` records blob/pack reachable and the `total_blob_refs`/`referenced_logical_bytes += entry.file_size` bookkeeping (the `TreeEntry` carried by `OnEdge` gives `placement`/`file_hash`/`pack_hash`/`file_size`). The `seen` dedup now lives inside `closureWalk`.
Run: `build/src/unit_tests_dbms --gtest_filter='Cas*Fsck*:CasGcLeak.*:CaWiring*' 2>&1 | tail -8`
Expected: fsck-driven tests unchanged (same reachable/dangling); the known baseline reds only.

- [ ] **Step 6: Refactor `CasGc.cpp` fold-expand onto `closureWalk` (backend source), recursion now active.** Replace the inline expansion (`CasGc.cpp:1342-1365`) with `closureWalk(record.tree_id, backendSource, on_tree=markExpanded, on_edge=addTreeEdge/addPackEdge)`. KEEP the existing `readTree`-404 handling (`CasGc.cpp:1268-1340`) as the `backendSource`'s error contract for table refs (displaced-later / FailClosed). The recursion now expands subtrees (the latent-gap fix).
Run: `build/src/unit_tests_dbms --gtest_filter='CasGc*:CasReuse*:CaWiring*' 2>&1 | tail -8`
Expected: no new regressions vs the known baseline reds (S1 still GREEN; S2 still RED — not fixed yet).

- [ ] **Step 7: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.* src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp src/Disks/tests/gtest_cas_closure_walk.cpp
git commit -m "CA B199-S2: unify closure traversal into closureWalk(root,entriesOf,visit); refactor fold-expand + fsck onto it (subtree recursion now active)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: `Build::precommit` populates the inline closure from staging

**Files:**
- Modify: `Core/CasBuild.cpp` (`Build::precommit`), `Core/CasBuild.h` if a helper is added
- Test: `src/Disks/tests/gtest_cas_build.cpp`

- [ ] **Step 1: Write the failing test** (precommit ref carries the closure):
```cpp
TEST(CasBuild, PrecommitRefCarriesInlineClosure)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    auto build = s->startBuild({});
    build->putBlob(idOf("B1"), BlobSource::fromString("B1"));
    build->putBlob(idOf("B2"), BlobSource::fromString("B2"));
    TreeEntry e1{.name="data.bin", .placement=Placement::Blob, .file_hash=u128Of("B1"), .file_size=2};
    TreeEntry e2{.name="data.mrk", .placement=Placement::Blob, .file_hash=u128Of("B2"), .file_size=2};
    const TreeId t = build->stageTree({e1, e2});
    build->precommit(t);
    // read the precommit shard manifest and assert the ref's closure lists B1, B2 under tree t.
    const auto shard = b->get(s->layout().rootShardKey(build->precommitNs(), build->buildShard()));
    ASSERT_TRUE(shard.has_value());
    const RootShard rs = decodeRootShard(shard->bytes);
    const RefPayload & pl = rs.refs.at(std::to_string(build->buildSeq()));
    ASSERT_EQ(pl.closure.size(), 1u);                 // one tree node (flat manifest)
    EXPECT_EQ(pl.closure[0].tree_hash, hexToU128(t.string()));
    EXPECT_EQ(pl.closure[0].entries.size(), 2u);
}
```
(If `precommitNs()`/`buildShard()`/`buildSeq()` are private, expose a test accessor as the existing tests do, or read the shard via the known layout.)

- [ ] **Step 2: Run — fails** (`pl.closure` empty).
Run: `ninja -C build unit_tests_dbms > build/build_b199s2_t4.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasBuild.PrecommitRefCarriesInlineClosure' 2>&1 | tail -5` → FAIL.

- [ ] **Step 3: Implement closure population.** In `Build::precommit` (`CasBuild.cpp`), build the `closure` from the staged structure. The build has the manifest's entries (the `stageTree` argument is retained — see `retained_trees`/`source_tree_cache`, or thread the staged entries into precommit). For each tree node the build STAGED (built), add a `ClosureNode{tree_hash, entries}`; recurse into staged subtrees (entries with `Placement::Subtree` whose hash is a built/staged tree). For an ADOPTED subtree (not staged by this build), record it as a **leaf** — a `ClosureNode` is NOT added for it (its own children are not this build's to reclaim). Set `payload.closure` before `root.refs[ref] = payload`.
```cpp
// inside the mutateShard lambda, before root.refs[ref] = payload:
payload.closure = buildStagedClosure(manifest_hash);   // helper: walks staged trees only, in-memory
```
Implement `buildStagedClosure(root)` as a private helper using the staged-entries map (the same data `stageTree` stored). It MUST NOT read from the pool.

- [ ] **Step 4: Run — passes.**
Run: `build/src/unit_tests_dbms --gtest_filter='CasBuild.*' 2>&1 | tail -6`
Expected: new test PASS; existing `CasBuild.*` PASS.

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.* src/Disks/tests/gtest_cas_build.cpp
git commit -m "CA B199-S2: Build::precommit populates RefPayload.closure from the staged tree structure (in-memory, no pool read; adopted subtrees as leaves)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: GC folds a precommit `Add` via the inline source; delete the precommit tree-read (S2 → GREEN)

**Files:**
- Modify: `Core/CasGc.cpp` (`foldShardRecords`)
- Test: `src/Disks/tests/gtest_cas_gc_leak.cpp` (the existing S2 tests are the gate)

- [ ] **Step 1: Confirm the RED baseline.**
Run: `build/src/unit_tests_dbms --gtest_filter='CasGcLeak.*' 2>&1 | tail -8`
Expected: `..._S2_NoFoldBetween` and raw `DisplacedUnexpandedTreeBlobsLeak` FAIL (`unreachable=2`); `..._S1_FoldBetween` PASS.

- [ ] **Step 2: Implement the inline-source branch.** In `foldShardRecords`, where it processes an `Add` for a tree (`CasGc.cpp:~1268`), branch on `Layout::isPrecommitNamespace(ns)`:
  - **precommit ns:** build an `inlineSource` from the ref's `RefPayload.closure` (a `node→entries` lookup over the `ClosureNode` list), then `closureWalk(record.tree_id, inlineSource, markExpanded, addTreeEdge/addPackEdge)`. The `RefPayload` is in `root.refs[record.ref_name]`. NO `readTree`, NO 404 path.
  - **table ns:** the existing `closureWalk(..., backendSource)` from Task 3 (with its `readTree`-404/displaced/FailClosed contract).
  **Delete** the precommit pending-tolerance branch (`CasGc.cpp:1307-1316`, the `if (Layout::isPrecommitNamespace(ns)) displaced_later = true;`) — it is unreachable now that precommit refs never reach `readTree`.

- [ ] **Step 3: Run the S2 tests — GREEN.**
Run: `ninja -C build unit_tests_dbms > build/build_b199s2_t5.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasGcLeak.*' 2>&1 | tail -8`
Expected: `..._S2_NoFoldBetween` and raw `DisplacedUnexpandedTreeBlobsLeak` now PASS (`unreachable=0`, `dangling=0`); `..._S1_FoldBetween` still PASS.

- [ ] **Step 4: Update the now-stale RED comments** in `gtest_cas_gc_leak.cpp` for the two S2 tests (they are now GREEN — say so; mirror the wording the S1 test got after its fix).

- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_gc_leak.cpp
git commit -m "CA B199-S2: fold precommit Add via inline closure (no readTree); delete precommit tree-read/pending-tolerance — DisplacedUnexpandedTreeBlobsLeak S2 GREEN

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Nested-manifest recursion test + full-suite gate

**Files:**
- Test: `src/Disks/tests/gtest_cas_gc_leak.cpp` (or `gtest_cas_gc_round.cpp`)

- [ ] **Step 1: Write a nested-manifest test** (proves the unified walk's subtree recursion both expands and reclaims a built subtree's blobs — the latent-gap fix):
```cpp
TEST(CasGcLeak, NestedManifestSubtreeBlobsReclaimed)
{
    std::shared_ptr<InMemoryBackend> b;
    auto s = openTestStore(b);
    const RootNamespace ns{"srv1/tbl"};
    // build a part whose manifest tree T references a Subtree S -> {Blob B}
    auto build = s->startBuild({});
    build->putBlob(idOf("B"), BlobSource::fromString("B"));
    const TreeId sub = build->putTree({ TreeEntry{.name="x", .placement=Placement::Blob, .file_hash=u128Of("B"), .file_size=1} });
    const TreeId top = build->putTree({ TreeEntry{.name="sub", .placement=Placement::Subtree, .file_hash=hexToU128(sub.string()), .file_size=1} });
    build->publish(ns, "all_0_0_0", top, {});
    s->renewWatermarkOnce();
    s->dropRef(ns, "all_0_0_0");
    s->renewWatermarkOnce();
    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    runGcToFixpoint(gc);
    const FsckReport rep = runFsck(*s, /*detail=*/false);
    EXPECT_EQ(rep.dangling, 0u);
    EXPECT_EQ(rep.unreachable, 0u) << "nested subtree S and its blob B must be reclaimed after the ref is dropped";
}
```

- [ ] **Step 2: Run — expected GREEN** (the unified walk now recurses subtrees; pre-refactor fold would have leaked S/B).
Run: `build/src/unit_tests_dbms --gtest_filter='CasGcLeak.NestedManifestSubtreeBlobsReclaimed' 2>&1 | tail -5` → PASS. If RED, the recursion/visitor wiring is wrong — fix before continuing.

- [ ] **Step 3: Full CA-suite no-regress.**
Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -15`
Expected: only the known baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow` remains; ALL `CasGcLeak.*` (incl. both S2 + the nested test) GREEN; `CasGcDangle.*`/`CasReuseGcRace.*`/`CaBuildRootDangle.*` GREEN.

- [ ] **Step 4: Commit.**
```bash
git add src/Disks/tests/gtest_cas_gc_leak.cpp
git commit -m "CA B199-S2: nested-manifest reclaim test (proves unified-walk subtree recursion) + full CA suite green (only the known FreezeViaHardLinksIntoShadow red)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Post-plan validation (not a code task)
After all tasks: rebuild `clickhouse`, run a fresh ca-soak (multi-leader stale-delete is S2's trigger) and confirm the steady-state `unreachable` drains toward 0 (vs the ~20 floor before), with `dangling=0` and all regression watches 0. Update backlog B199 → S2 DONE.

## Notes for the implementer
- Branch is `cas-vfs-path-mapping`; verify with `git branch` before committing; never commit to master.
- Always redirect `ninja` output to `build/build_*.log` and summarize via a subagent/`tail`; never dump full build logs.
- Allman braces; `ASan` not `ASAN` in any message; wrap `MergeTree`/symbol names in backticks in comments/commits.
- The S1 fix (`6d1e2daae40`) already handles the absent-tree retire case — do NOT re-touch it; S2 relies on it for the "tree object gone" half.
