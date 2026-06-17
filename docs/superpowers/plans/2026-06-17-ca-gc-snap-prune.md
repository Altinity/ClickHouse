# P9 — GC snapshot prune (eliminate the 404-HEAD storm) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** stop the regular-GC `retire` observe loop from re-`HEAD`ing already-deleted candidates by removing a node from the durable in-degree snapshot (`gc/snap`) the moment GC knows the object is gone — at the delete site (`cascade`) and on a genuine retire-time `HEAD`-404 — without weakening `INV-NO-LOSS`/`INV-NO-DANGLE`/`INV-NO-RETURN`.

**Architecture:** add one snapshot primitive `GcSnap::forget(kind, hash)` (removes a node from `known`/`everEdged`); call it from the cascade for every confirmed-deleted node (primary, keeps `known` tight by construction) and from the retire observe loop on a genuine 404 (defensive/self-healing); surface two counters (`forgotten_on_delete`, `forgotten_absent`) end-to-end into `system.content_addressed_garbage_collection_log`. The TLA+ model gains one `GForget` action, re-verified against all invariants, and gates the code.

**Tech Stack:** C++ (ClickHouse `Cas` core under `src/Disks/.../ContentAddressed/`), GoogleTest (`src/Disks/tests/gtest_cas_*`), stateless SQL tests (`tests/queries/0_stateless`), TLA+/TLC (`docs/superpowers/models/CaIncarnationCore.tla`).

**Spec:** `docs/superpowers/specs/2026-06-17-ca-gc-snap-prune-design.md`

---

## File Structure

- `docs/superpowers/models/CaIncarnationCore.tla` — add `GForget(h)`; add to `Next`. (Task 1)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.{h,cpp}` — `forget`. (Task 2)
- `src/Disks/tests/gtest_cas_gc_snap.cpp` — `forget` unit tests. (Task 2)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h` — `RoundReport` fields; `RecheckResult.deleted_nodes`; `retire` signature. (Tasks 3,4)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — cascade prune + `snap_changed` fix (Task 3); retire-404 prune (Task 4).
- `src/Disks/tests/gtest_cas_gc_round.cpp` — round-level prune tests. (Tasks 3,4)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h` — two counter fields. (Task 5)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp` — copy counters into the record. (Task 5)
- `src/Interpreters/ContentAddressedGarbageCollectionLog.{h,cpp}` — two columns. (Task 5)
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` — copy counters into the log element. (Task 5)
- `src/Disks/tests/gtest_cas_gc_log.cpp` — counter assertion. (Task 5)
- `tests/queries/0_stateless/` — functional coverage (new test via `add-test`). (Task 6)

---

## Task 1: TLA+ — `GForget` action + re-verify

**Files:**
- Modify: `docs/superpowers/models/CaIncarnationCore.tla` (add action after `Land`/`ApplyPendCascade`, ~line 559; add disjunct to `Next`, ~line 680)

**Context:** `everEdged` (the impl's `known`) is the journal-known set. Today `Land` (the delete, line 530) leaves `everEdged` UNCHANGED (line 547), so after a delete the state `~present[h] ∧ h ∈ everEdged ∧ InDeg(h)=0` is already reachable — this is the model's image of the lingering-known node that the impl re-`HEAD`s. `GForget` consumes it. A single always-enabled `GForget` conservatively covers BOTH impl prune sites (the cascade delete-time prune and the retire-404 prune): if invariants hold when the prune may fire any time after a delete, they hold when it fires atomically at delete. `InDeg(h) == Cardinality({e \in rootEdges : e[2]=h}) + Cardinality({e \in treeEdges : e[2]=h})` (line 121). The full `vars` tuple is at line 111.

- [ ] **Step 1: Add the `GForget` action**

Insert after `ApplyPendCascade` (after line 559), before the "next / spec" banner (line 664):

```tla
\* P9 (2026-06-17): GC removes an absent, zero-in-degree, journal-known node from `everEdged`.
\* Models BOTH implementation prune sites as one abstract operation — the cascade's delete-time
\* prune and the retire observe loop's HEAD-404 prune both do exactly this. Always enabled when the
\* node is gone: a single unconditional action is the STRONGEST verification (the prune may lag the
\* delete arbitrarily; atomic-at-delete is a special case). everEdged is re-added only by GFold when
\* a future journal Add re-references the hash (resurrection), exactly as in the implementation.
GForget(h) ==
    /\ ~present[h] /\ h \in everEdged /\ InDeg(h) = 0
    /\ everEdged' = everEdged \ {h}
    /\ UNCHANGED << present, tokOf, nextTok, deadTok, man, retired, inflight, gcRound, gcPhase,
                    roundOf, fencedSet, fencePos, cursor, trimBase, rootEdges, treeEdges, marker,
                    pendCasc, wDeps, wView, creator, hbAlive, hbSeq, wedged, hbObs, fgPhase, fgCut,
                    fgRefs, fgSeen, reg, wEv >>
```

- [ ] **Step 2: Add `GForget` to `Next`**

In `Next` (line 665), add a disjunct alongside the other GC actions (after the `GDebrisRetire` line, ~line 682):

```tla
    \/ \E h \in Hashes : GForget(h)
```

- [ ] **Step 3: Run TLC on a safety stage and verify it passes**

Run: `cd docs/superpowers/models && ./run_tlc.sh CaIncarnationCore_stage4.cfg`
Expected: `Model checking completed. No error has been found.` (exit=0). This stage checks `TypeOK`, `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN`, `INV_JOURNAL_COVERAGE`, `MonotoneGC` with trees + debris + full-GC over two shards.

- [ ] **Step 4: Run TLC on the registry safety stage**

Run: `cd docs/superpowers/models && ./run_tlc.sh CaIncarnationCore_stage6_registry.cfg`
Expected: `Model checking completed. No error has been found.` (exit=0).

- [ ] **Step 5: Run TLC on the liveness stage and verify no-leak-forever still holds**

Run: `cd docs/superpowers/models && ./run_tlc.sh CaIncarnationCore_stage2_live.cfg`
Expected: `Model checking completed. No error has been found.` (exit=0). `NoLeakForever` must still hold (`GForget` fires only on already-`~present` nodes, so it never affects the eventually-deleted goal).

- [ ] **Step 6: Confirm a sabotage config STILL finds its counterexample (no masking)**

Run: `cd docs/superpowers/models && ./run_tlc.sh CaIncarnationCore_sab_unconddelete.cfg`
Expected: TLC reports an invariant violation (`INV_NO_RETURN` or `INV_NO_LOSS` violated) — i.e. exit non-zero with a counterexample. Adding `GForget` must NOT mask the existing bug-detection. (A `SabotageUncondDelete` run is designed to fail; "Error: ... is violated" in the log is the PASS condition for this step.)

- [ ] **Step 7: Record results and commit**

Append a short P9 section to `docs/superpowers/models/CaIncarnationCore_RESULTS.md` noting: `GForget` added; stage4 / stage6_registry / stage2_live pass; sab_unconddelete still finds its counterexample; state counts from the logs.

```bash
git add docs/superpowers/models/CaIncarnationCore.tla docs/superpowers/models/CaIncarnationCore_RESULTS.md
git commit -m "CA P9 model: add GForget (prune absent zero-in-degree node from everEdged); re-verify invariants

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `GcSnap::forget` primitive

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h` (declare `forget`, after `stripTree` decl ~line 66)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.cpp` (define `forget`, after `stripTree` ~line 220)
- Test: `src/Disks/tests/gtest_cas_gc_snap.cpp`

**Context:** `known` is a `std::set<NodeKey>` where `NodeKey = std::pair<uint8_t, UInt128>`; `indeg` is `std::map<NodeKey, uint64_t>` storing only nonzero counts (a zero count is erased). `isKnown`/`inDegree`/`zeroInDegreeKnown` already use `NodeKey{static_cast<uint8_t>(kind), hash}`. The codec encodes `known.size()` then iterates (lines 286, 359), so a smaller `known` round-trips with no codec change.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_snap.cpp` (match the file's existing namespace/usings; `idOf`/`u128Of` style helpers or raw `UInt128` as the file already uses):

```cpp
TEST(CasGcSnap, ForgetRemovesNodeFromKnown)
{
    using namespace DB::Cas;
    GcSnap snap;
    const UInt128 tree{0, 1};
    const UInt128 blob{0, 2};
    /// A root edge makes `tree` known; expanding it makes `blob` known and pins it.
    snap.addRootEdge("rs", "all_0_0_0", tree);
    snap.addTreeEdge(tree, ObjectKind::Blob, blob);
    snap.markExpanded(tree);

    /// Drop the root edge: tree -> in-degree 0, still known => a zero-in-degree candidate.
    snap.removeRootEdge("rs", "all_0_0_0");
    ASSERT_TRUE(snap.isKnown(ObjectKind::Tree, tree));
    bool tree_is_candidate = false;
    for (const auto & c : snap.zeroInDegreeKnown())
        if (c.kind == ObjectKind::Tree && c.hash == tree) tree_is_candidate = true;
    ASSERT_TRUE(tree_is_candidate);

    /// Forget it: gone from `known` and from the candidate set; idempotent.
    snap.forget(ObjectKind::Tree, tree);
    EXPECT_FALSE(snap.isKnown(ObjectKind::Tree, tree));
    for (const auto & c : snap.zeroInDegreeKnown())
        EXPECT_FALSE(c.kind == ObjectKind::Tree && c.hash == tree);
    snap.forget(ObjectKind::Tree, tree);   // no-op, must not throw

    /// A later edge re-references the hash => re-added to `known` (the resurrection path).
    snap.addRootEdge("rs", "all_0_0_0", tree);
    EXPECT_TRUE(snap.isKnown(ObjectKind::Tree, tree));
}

TEST(CasGcSnap, ForgetSurvivesEncodeDecode)
{
    using namespace DB::Cas;
    GcSnap snap;
    const UInt128 a{0, 7};
    const UInt128 b{0, 8};
    snap.addRootEdge("rs", "p1", a);
    snap.addRootEdge("rs", "p2", b);
    snap.removeRootEdge("rs", "p1");   // a -> in-degree 0, known
    snap.forget(ObjectKind::Tree, a);

    const GcSnap round = decodeGcSnap(encodeGcSnap(snap));
    EXPECT_FALSE(round.isKnown(ObjectKind::Tree, a));
    EXPECT_TRUE(round.isKnown(ObjectKind::Tree, b));
}
```

- [ ] **Step 2: Run to verify it fails to compile**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20` (redirect per repo convention; analyze via subagent)
Expected: compile error — `forget` is not a member of `GcSnap`.

- [ ] **Step 3: Declare `forget` in the header**

In `CasGcSnap.h`, after the `stripTree` declaration (line 66):

```cpp
    /// Remove a node from `known` (the inverse of addEdge's known.insert). Set semantics: forgetting
    /// a node not in `known` is a no-op (idempotent crash-replay). Edges/markers are untouched — a
    /// node is only forgotten when its in-degree is already 0, so it has no incoming edge; a later
    /// folded Add re-inserts it via addEdge. P9: keeps `known` from growing past live nodes.
    void forget(ObjectKind kind, const UInt128 & hash);
```

- [ ] **Step 4: Define `forget` in the .cpp**

In `CasGcSnap.cpp`, after `stripTree` (after line ~220, before `markExpanded`):

```cpp
void GcSnap::forget(ObjectKind kind, const UInt128 & hash)
{
    /// indeg holds only nonzero counts; a zero-in-degree node has no indeg entry, so erasing from
    /// `known` is sufficient. Erasing a key not present is a no-op (idempotent).
    known.erase(NodeKey{static_cast<uint8_t>(kind), hash});
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20 && ./src/unit_tests_dbms --gtest_filter='CasGcSnap.Forget*' 2>&1 | tail -20` (redirect to a log + subagent-analyze)
Expected: `[  PASSED  ] 2 tests.`

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.cpp src/Disks/tests/gtest_cas_gc_snap.cpp
git commit -m "CA P9: add GcSnap::forget (remove an absent zero-in-degree node from known)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: cascade delete-time prune + `RoundReport` counters + `snap_changed` fix

**Files:**
- Modify: `src/Disks/.../Core/CasGc.h` (`RoundReport` fields ~line 28; `RecheckResult.deleted_nodes` ~line 208)
- Modify: `src/Disks/.../Core/CasGc.cpp` (recheck builds `deleted_nodes` ~line 311; cascade prunes + `snap_changed` ~line 364/384)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

**Context:** `recheck` (CasGc.cpp:154) builds `RecheckResult` and, in the loop at lines 311-335, classifies each final outcome — pushing `Deleted`/`Absent` trees into `result.deleted_trees`. `cascadeAndPersist` (line 342) iterates `rechecked.deleted_trees` for `stripTree` and computes `snap_changed = !rechecked.deleted_trees.empty() || rechecked.fence_window_records_folded` (line 384). Both `recheck` and `cascadeAndPersist` already take `RoundReport & report`. `Candidate{ObjectKind kind; UInt128 hash;}` is defined in `CasGcSnap.h`.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_round.cpp` (use the file's existing helpers for publishing/dropping/running rounds; mirror `gtest_cas_gc_log.cpp`'s `publishPart` + `dropRef` + `renewWatermarkOnce` pattern if the round-test file lacks one):

```cpp
TEST(CasGcRound, DeleteTimePruneRemovesNodeFromNextRoundCandidates)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p"});
    const RootNamespace ns{"srv1/tbl"};

    /// Publish then drop a part; advance the watermark so the build guard no longer spares it.
    /* publishPart(store, ns.string(), "all_0_0_0", "p9-delete-time"); */
    /* store->dropRef(ns, "all_0_0_0"); store->renewWatermarkOnce(); */

    Gc gc(store, u128Of("gc-1"));
    /// Run rounds until something is physically deleted; capture that round's report.
    RoundReport deleting;
    for (int i = 0; i < 16; ++i)
    {
        const RoundReport r = gc.runRegularRound();
        if (r.deleted > 0) { deleting = r; break; }
    }
    ASSERT_GT(deleting.deleted, 0u) << "expected a deletion round";
    EXPECT_GT(deleting.forgotten_on_delete, 0u) << "deleted nodes must be forgotten in the same round";

    /// The KEY assertion: the round AFTER deletion does NOT re-HEAD the deleted nodes — it forgets
    /// nothing on the absent path (they are no longer candidates) and deletes nothing new.
    const RoundReport after = gc.runRegularRound();
    EXPECT_EQ(after.forgotten_absent, 0u) << "the storm is gone: no re-HEAD-404 of already-deleted nodes";
    EXPECT_EQ(after.deleted, 0u);
}
```

(Replace the commented helper calls with the round-test file's actual publish/drop helpers; if none exist, copy `publishPart` from `gtest_cas_gc_log.cpp` into an anonymous namespace at the top of this file.)

- [ ] **Step 2: Run to verify it fails**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20`
Expected: compile error — `RoundReport` has no member `forgotten_on_delete`.

- [ ] **Step 3: Add the counter fields to `RoundReport`**

In `CasGc.h`, inside `struct RoundReport` (after `cascaded`, line 30):

```cpp
    uint64_t forgotten_on_delete = 0;  /// nodes pruned from `known` because GC deleted them (cascade)
    uint64_t forgotten_absent = 0;     /// nodes pruned because a retire HEAD found them already gone (404)
```

- [ ] **Step 4: Add `deleted_nodes` to `RecheckResult`**

In `CasGc.h`, inside `struct RecheckResult` (after `deleted_trees`, line 211):

```cpp
    std::vector<Candidate> deleted_nodes;      /// EVERY confirmed-gone node (trees AND blobs/packs):
                                               /// the cascade forgets each from `known` (P9).
```

- [ ] **Step 5: Populate `deleted_nodes` in recheck**

In `CasGc.cpp`, in the outcome-classification loop (lines 311-335), inside the `Deleted` and `Absent` cases, push the node. Replace the `Deleted` and `Absent` case bodies:

```cpp
                case OutcomeKind::Deleted:
                    ++report.deleted;
                    result.deleted_nodes.push_back(Candidate{outcome.kind, outcome.hash});
                    if (outcome.kind == ObjectKind::Tree)
                        result.deleted_trees.push_back(outcome.hash);
                    break;
                case OutcomeKind::Absent:
                    ++report.absent;
                    result.deleted_nodes.push_back(Candidate{outcome.kind, outcome.hash});
                    if (outcome.kind == ObjectKind::Tree)
                        result.deleted_trees.push_back(outcome.hash);
                    break;
```

- [ ] **Step 6: Prune in the cascade and fix `snap_changed`**

In `CasGc.cpp` `cascadeAndPersist`, after the `stripTree` loop (after line 368) add the forget loop:

```cpp
    /// P9: forget every confirmed-gone node (trees AND blobs/packs) from `known`, so the next
    /// round's stateless candidate scan no longer re-derives — and re-HEAD-404s — them. This is
    /// the PRIMARY prune site: a node deleted in round R is out of `known` before R's retired sets
    /// drop, keeping `known` tight by construction. Orthogonal to stripTree (which clears a deleted
    /// tree's OUTGOING edges/marker; this clears the node's INCOMING `known` membership).
    for (const Candidate & node : rechecked.deleted_nodes)
    {
        snap.at(hashPrefixShard(node.hash, state.snap_shards)).forget(node.kind, node.hash);
        ++report.forgotten_on_delete;
    }
```

Then change the `snap_changed` computation (line 384) to include the prune:

```cpp
    const bool snap_changed = !rechecked.deleted_trees.empty() || rechecked.fence_window_records_folded
        || report.forgotten_on_delete > 0;
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20 && ./src/unit_tests_dbms --gtest_filter='CasGcRound.DeleteTimePrune*' 2>&1 | tail -20` (log + subagent-analyze)
Expected: `[  PASSED  ] 1 test.`

- [ ] **Step 8: Add a blob-only deletion round test (covers the `snap_changed` fix)**

A round whose only physical deletes are blobs (no trees) must still persist the pruned snap. The happy-path test above already drives blob deletes (a part's content blob is deleted in a round after its tree); assert the persist took effect by re-opening the snap. Add:

```cpp
TEST(CasGcRound, BlobOnlyDeletePruneIsPersisted)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p"});
    const RootNamespace ns{"srv1/tbl"};
    /* publishPart(store, ns.string(), "all_0_0_0", "p9-blob-only"); */
    /* store->dropRef(ns, "all_0_0_0"); store->renewWatermarkOnce(); */

    Gc gc(store, u128Of("gc-2"));
    bool saw_blob_forget = false;
    for (int i = 0; i < 16; ++i)
    {
        const RoundReport r = gc.runRegularRound();
        if (r.forgotten_on_delete > 0 && r.children_cascaded == 0 && r.deleted > 0)
            saw_blob_forget = true;   // a round that forgot nodes with no tree cascade => blob-only
    }
    EXPECT_TRUE(saw_blob_forget);

    /// A FRESH Gc instance (empty resident cache) reloads the durable snap; the forgotten blob must
    /// not reappear as a candidate (i.e. the prune was persisted, not just in-memory).
    Gc gc2(store, u128Of("gc-3"));
    const RoundReport r = gc2.runRegularRound();
    EXPECT_EQ(r.forgotten_absent, 0u) << "persisted prune: no re-HEAD-404 after a fresh reload";
}
```

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20 && ./src/unit_tests_dbms --gtest_filter='CasGcRound.BlobOnlyDelete*' 2>&1 | tail -20`
Expected: `[  PASSED  ] 1 test.`

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CA P9: prune deleted nodes from the GC snapshot at the delete site (cascade)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: retire-404 defensive prune

**Files:**
- Modify: `src/Disks/.../Core/CasGc.h` (`retire` signature, line ~182)
- Modify: `src/Disks/.../Core/CasGc.cpp` (`retire` body ~line 601; `runRegularRound` call site line 78)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

**Context:** `retire` is declared `std::map<uint64_t, RetiredSet> retire(GcState & state, Token & state_token, const std::map<uint64_t, GcSnap> & snap);` (CasGc.h:182) and defined at CasGc.cpp:601; it iterates `snap` (const) and on `!observed.exists` does `continue` (line 637-641). It is called at `runRegularRound` line 78 with `folded.snap` (which is mutable — `FoldResult::snap`). `head().exists == false` is a genuine 404 (verified: `getObjectInfoIfExists` returns absent only for `NO_SUCH_KEY`/`NO_SUCH_BUCKET`/`RESOURCE_NOT_FOUND`; other errors throw). The shard iterated is `snap_shard`, which equals `hashPrefixShard(candidate.hash, snap_shards)` by construction.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_round.cpp`:

```cpp
TEST(CasGcRound, RetireForgetsOutOfBandDeletedCandidate)
{
    using namespace DB::Cas;
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = Store::open(backend, PoolConfig{.pool_prefix = "p"});
    const RootNamespace ns{"srv1/tbl"};
    const TreeId tree = /* publishPart(store, ns.string(), "all_0_0_0", "p9-oob"); */ TreeId{};
    /* store->dropRef(ns, "all_0_0_0"); store->renewWatermarkOnce(); */

    /// Out-of-band delete the tree object so the next round's retire HEAD sees a genuine 404 while
    /// the node is still a zero-in-degree known candidate. Use the backend's own delete with the
    /// observed token (the Layout key for a tree object).
    const String tree_key = objectKey(store->layout(), ObjectKind::Tree, tree.value);
    const HeadResult hr = backend->head(tree_key);
    ASSERT_TRUE(hr.exists);
    ASSERT_EQ(backend->deleteExact(tree_key, hr.token).kind, DeleteOutcome::Kind::Deleted);

    Gc gc(store, u128Of("gc-oob"));
    const RoundReport r = gc.runRegularRound();   // must not throw on the 404
    EXPECT_GT(r.forgotten_absent, 0u) << "a genuine retire-time 404 forgets the node";

    /// Self-healed: a second round no longer re-derives the forgotten node.
    const RoundReport after = gc.runRegularRound();
    EXPECT_EQ(after.forgotten_absent, 0u);
}
```

(Wire the `publishPart`/`dropRef` helpers as in Task 3; `objectKey` and `ObjectKind` are in the `Cas` core headers already included by the round-test file. Use the real returned `TreeId` from `publishPart`.)

- [ ] **Step 2: Run to verify it fails**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20 && ./src/unit_tests_dbms --gtest_filter='CasGcRound.RetireForgets*' 2>&1 | tail -20`
Expected: FAIL — `forgotten_absent` is 0 (retire currently just `continue`s on 404).

- [ ] **Step 3: Change the `retire` signature to take a mutable snap + the report**

In `CasGc.h` (line 182), change the declaration:

```cpp
    std::map<uint64_t, RetiredSet> retire(GcState & state, Token & state_token,
                                          std::map<uint64_t, GcSnap> & snap, RoundReport & report);
```

In `CasGc.cpp` (line 601), change the definition signature to match:

```cpp
std::map<uint64_t, RetiredSet> Gc::retire(GcState & state, Token & state_token,
                                          std::map<uint64_t, GcSnap> & snap, RoundReport & report)
```

- [ ] **Step 4: Forget the node on a genuine 404**

In `CasGc.cpp` `retire`, the observe loop currently iterates `for (const auto & [snap_shard, shard_snap] : snap)`. Change it to a mutable binding and replace the `!observed.exists` branch:

```cpp
    for (auto & [snap_shard, shard_snap] : snap)
    {
        for (const Candidate & candidate : shard_snap.zeroInDegreeKnown())
        {
            const HeadResult observed = backend.head(objectKey(layout, candidate.kind, candidate.hash));
            if (!observed.exists)
            {
                /// P9 defensive prune. The object is gone but its node still sits in `known`
                /// (in-degree 0). In correct single-leader operation the delete-time prune (the
                /// cascade) already keeps `known` tight, so this path is rare; it self-heals the
                /// cases that prune cannot reach from THIS leader's snapshot: a stale leader
                /// observing a node a live leader already deleted (split-brain — the lease is
                /// work-dedup only, by design), the crash/resume window before a delete-time prune
                /// is durable, and any out-of-band deletion. `exists == false` is a GENUINE 404
                /// (getObjectInfoIfExists returns absent only for NO_SUCH_KEY/NO_SUCH_BUCKET/
                /// RESOURCE_NOT_FOUND; every other backend error throws and aborts the round), so a
                /// transient error never masquerades as absence — we never forget a live node. NOT
                /// a LOGICAL_ERROR: throwing here would crash on benign split-brain races.
                shard_snap.forget(candidate.kind, candidate.hash);
                ++report.forgotten_absent;
                continue;
            }
            ...   // (protectedByLiveBuild + retire-entry construction unchanged)
```

Note: `zeroInDegreeKnown()` returns a value (a fresh `std::vector<Candidate>`), so mutating `shard_snap` via `forget` inside the loop does not invalidate the iteration over that snapshot copy.

- [ ] **Step 5: Update the `runRegularRound` call site**

In `CasGc.cpp` line 78, pass the report and drop the `const`:

```cpp
    const std::map<uint64_t, RetiredSet> retired = retire(state, state_token, folded.snap, report);
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20 && ./src/unit_tests_dbms --gtest_filter='CasGcRound.RetireForgets*' 2>&1 | tail -20`
Expected: `[  PASSED  ] 1 test.`

- [ ] **Step 7: Run the whole round suite (no regression)**

Run: `cd build && ./src/unit_tests_dbms --gtest_filter='CasGcRound.*:CasGcSnap.*:CasGcLog.*:CasRetireView.*' 2>&1 | tail -25` (log + subagent-analyze)
Expected: all green except any pre-existing B140 red (note it explicitly if seen).

- [ ] **Step 8: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CA P9: forget already-gone candidates on a genuine retire-time HEAD-404

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: plumb the two counters into the GC log table

**Files:**
- Modify: `src/Disks/.../ContentAddressed/CasGcRoundLogRecord.h` (two fields, after `children_cascaded` line 30)
- Modify: `src/Disks/.../ContentAddressed/CasGcScheduler.cpp` (copy from report, after line 135)
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.h` (two element fields, after `children_cascaded` line 31)
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp` (two columns: `getColumnsDescription` after line 44, `appendToBlock` after line 70)
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedMetadataStorage.cpp` (copy into element, after line 210)
- Test: `src/Disks/tests/gtest_cas_gc_log.cpp`

**Context:** the chain `RoundReport` → `GcRoundLogRecord` (scheduler, CasGcScheduler.cpp:130-135) → `ContentAddressedGarbageCollectionLogElement` (ContentAddressedMetadataStorage.cpp:205-210) → columns (ContentAddressedGarbageCollectionLog.cpp). `appendToBlock` inserts columns in declaration order via an index `i++`; new columns must be appended in the SAME position in all three (record/element/columns) — append them after `children_cascaded`, before `duration_ms`, in ALL of: the columns vector, `appendToBlock`, the element struct, the record struct, the scheduler copy, and the metadata-storage copy. Keeping them adjacent to `children_cascaded` (a count) is the least error-prone.

- [ ] **Step 1: Write the failing test**

Extend `src/Disks/tests/gtest_cas_gc_log.cpp` `CasGcLog.EmitsStartFinishWithCounts` — after the deletion round is found, assert the record carries the delete-time forget count. Add near the existing `EXPECT_GT(rows[deleting_finish_idx].objects_deleted, 0u);`:

```cpp
    /// P9: the deletion round forgot the deleted nodes; the record surfaces it.
    EXPECT_GT(rows[deleting_finish_idx].forgotten_on_delete, 0u);
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20`
Expected: compile error — `GcRoundLogRecord` has no member `forgotten_on_delete`.

- [ ] **Step 3: Add the two fields to `GcRoundLogRecord`**

In `CasGcRoundLogRecord.h`, after `children_cascaded` (line 30):

```cpp
    UInt64 forgotten_on_delete = 0;
    UInt64 forgotten_absent = 0;
```

- [ ] **Step 4: Copy them in the scheduler**

In `CasGcScheduler.cpp`, after line 135 (`fin.children_cascaded = rep.cascaded;`):

```cpp
        fin.forgotten_on_delete = rep.forgotten_on_delete;
        fin.forgotten_absent = rep.forgotten_absent;
```

- [ ] **Step 5: Add the two fields to the log element**

In `ContentAddressedGarbageCollectionLog.h`, after `children_cascaded` (line 31):

```cpp
    UInt64 forgotten_on_delete = 0;
    UInt64 forgotten_absent = 0;
```

- [ ] **Step 6: Add the two columns (description + append)**

In `ContentAddressedGarbageCollectionLog.cpp` `getColumnsDescription`, after the `children_cascaded` column (line 44):

```cpp
        {"forgotten_on_delete", std::make_shared<DataTypeUInt64>(), "Nodes pruned from the GC snapshot because GC deleted them this round (P9)."},
        {"forgotten_absent", std::make_shared<DataTypeUInt64>(), "Nodes pruned because a retire HEAD found them already gone (404); >0 in steady state signals split-brain or out-of-band deletes (P9)."},
```

In `appendToBlock`, after `columns[i++]->insert(children_cascaded);` (line 70):

```cpp
    columns[i++]->insert(forgotten_on_delete);
    columns[i++]->insert(forgotten_absent);
```

- [ ] **Step 7: Copy them in the metadata storage**

In `ContentAddressedMetadataStorage.cpp`, after line 210 (`e.children_cascaded = r.children_cascaded;`):

```cpp
        e.forgotten_on_delete = r.forgotten_on_delete;
        e.forgotten_absent = r.forgotten_absent;
```

- [ ] **Step 8: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms 2>&1 | tail -20 && ./src/unit_tests_dbms --gtest_filter='CasGcLog.*' 2>&1 | tail -20`
Expected: `[  PASSED  ] 2 tests.`

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcRoundLogRecord.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CasGcScheduler.cpp src/Interpreters/ContentAddressedGarbageCollectionLog.h src/Interpreters/ContentAddressedGarbageCollectionLog.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_cas_gc_log.cpp
git commit -m "CA P9: surface forgotten_on_delete / forgotten_absent in the GC log table

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: functional SQL test + full build

**Files:**
- Create: `tests/queries/0_stateless/05008_ca_gc_snap_prune.sql` (+ `.reference`) via the `add-test` helper
- Build: `build/`

**Context:** the introspection feature's test is `tests/queries/0_stateless/05007_content_addressed_gc_introspection.sql`; mirror its CA-disk table setup. The GC-log default config block must be enabled in the test profile (it is, via the introspection feature). Use `add-test` so the number prefix is assigned correctly (do not hand-pick `05008`).

- [ ] **Step 1: Create the test skeleton**

Run: `./tests/queries/0_stateless/add-test ca_gc_snap_prune` (this assigns the next free prefix and creates both files; the actual prefix may differ from `05008` — use whatever it creates)

- [ ] **Step 2: Write the test body**

In the created `.sql` (mirror `05007`'s CA-disk DDL), drive a delete and assert the prune counter is visible:

```sql
-- Tags: no-fasttest
-- (CA disk + GC log; mirror 05007 for the disk/table setup.)
DROP TABLE IF EXISTS t_ca_p9 SYNC;
CREATE TABLE t_ca_p9 (a UInt64) ENGINE = MergeTree ORDER BY a
    SETTINGS disk = 'ca_disk', index_granularity = 8192;   -- use 05007's CA disk name
INSERT INTO t_ca_p9 SELECT number FROM numbers(1000);
TRUNCATE TABLE t_ca_p9;
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION ca_disk;
SYSTEM CONTENT ADDRESSED GARBAGE COLLECTION ca_disk;
SYSTEM FLUSH LOGS content_addressed_garbage_collection_log;
-- A round physically deleted objects and forgot them in the same round.
SELECT sum(forgotten_on_delete) > 0
FROM system.content_addressed_garbage_collection_log
WHERE event_type = 'Finish' AND disk_name = 'ca_disk';
DROP TABLE t_ca_p9 SYNC;
```

Set the `.reference` to a single line `1`.

- [ ] **Step 3: Full build (release/default, the soak image binary)**

Run: `cd build && ninja clickhouse > build_p9.log 2>&1; tail -5 build_p9.log` (redirect per repo convention; dispatch a subagent to analyze `build/build_p9.log` and return a concise pass/fail + any warnings)
Expected: build succeeds; binary at `build/programs/clickhouse`.

- [ ] **Step 4: Run the functional test**

Run via the praktika/local stateless runner with the freshly built binary symlinked at `ci/tmp/clickhouse` (see `reference_praktika_local_runs`). Redirect to `build/test_ca_gc_snap_prune.log`; subagent-analyze.
Expected: test PASS (the prune counter query returns `1`).

- [ ] **Step 5: Commit**

```bash
git add tests/queries/0_stateless/05*_ca_gc_snap_prune.*
git commit -m "CA P9: functional test — forgotten_on_delete surfaces after TRUNCATE+GC

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**1. Spec coverage:**
- Component 1 (`GcSnap::forget`) → Task 2. ✓
- Component 2 (prune on delete, cascade, `snap_changed` fix) → Task 3. ✓
- Component 3 (retire-404 prune, mutable snap, no throw, genuine-404) → Task 4. ✓
- Component 4 (two counters end-to-end) → Task 5. ✓
- Safety / TLA+ (`GForget`, re-verify invariants + liveness, no sabotage masking) → Task 1. ✓
- Testing (unit forget, delete-time prune incl. blob-only persist, retire-404, op-count non-return, functional) → Tasks 2/3/4/6. ✓
- Out of scope (full-GC, negative cache) → not implemented. ✓

**2. Placeholder scan:** the two round-test tasks use commented helper calls (`/* publishPart ... */`) because the exact helper in `gtest_cas_gc_round.cpp` must be matched at execution; each task states explicitly to copy `publishPart` from `gtest_cas_gc_log.cpp` if absent and to use the real returned `TreeId`. No `TODO`/`TBD`; every code step shows complete code.

**3. Type consistency:** `forget(ObjectKind, const UInt128&)` consistent across Tasks 2/3/4. `RoundReport.forgotten_on_delete`/`forgotten_absent` (Task 3) match `GcRoundLogRecord` and the log element (Task 5). `RecheckResult.deleted_nodes` is `std::vector<Candidate>` (Task 3) consumed in the cascade (Task 3). `retire` new signature `(GcState&, Token&, std::map<uint64_t,GcSnap>&, RoundReport&)` matches its call site (Task 4 Step 5). `Candidate{kind, hash}` aggregate init matches `CasGcSnap.h`.
