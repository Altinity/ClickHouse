# CAS GC Baseline Guard + Raw Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fail-closed guard against "fresh GC over trimmed journals mass-deletes live data" + `Gc::rebuildBaseline` disaster recovery for lost/corrupt `gc/state`, surfaced as `SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE]` and `clickhouse-disks ca-gc-rebuild`.

**Architecture:** The rebuild is deliberately NOT a new scanner: it reuses the round's own bricks — `discoverUniverse` for the shard universe, `foldManifestEdges(id, +1, …)` for edge emission, and `foldDeltasIntoGeneration` with EMPTY `prior_runs` for run production, iterated over attempt numbers for O(budget) memory. The guard is ~15 lines inside the existing per-shard fold classification. Spec: `docs/superpowers/specs/2026-07-03-cas-gc-rebuild-design.md`.

**Tech Stack:** C++ (Allman braces), gtest (`src/Disks/tests/`), TLA+/TLC (`docs/superpowers/models/`), ClickHouse SQL parser/interpreter, `clickhouse-disks`.

## Global Constraints

- Rebuild writes ONLY the gc plane (`gc/gen/...`, `gc/state`); NEVER ref shards, manifests, blobs; NEVER deletes content (spec §Part 2).
- Rebuilt `retired_refs` is EMPTY; `new_round = max(observed_gc_round over all mounts, fence_round over all shards, surviving gc/gen numbers) + 1` (spec §Algorithm 7).
- Fail-closed refusals: missing/invalid committed manifest body, undecodable ref shard, unreadable mount body, healthy state without FORCE, lease not acquired (spec §Refusal conditions).
- Live-conservative only; no quiesce mode (spec §Part 2).
- The guard gates GC rounds only — never server startup, never reads/writes (spec §Part 1).
- DRY/KISS (user directive): reuse `discoverUniverse` / `foldManifestEdges` / `foldDeltasIntoGeneration` / `computeHeartbeatFloor` / `prefixEligible`; no new scanner classes, no new run-merge machinery.
- Do not use `sleep` in C++ tests; branch `cas-gc-rebuild` off `cas-shard-mutation-queue`; commit per task.

## Design deltas vs the spec (resolved while planning — the spec is amended by this section)

1. **Guard is PER-SHARD, not global:** the fold already computes per-shard coverage; the guard
   fires for a shard with NO sealed cursor whose journal proves trimmed history. This subsumes the
   spec's empty-baseline rule and additionally catches per-shard coverage loss. A shard born after
   the baseline passes (its journal starts at `transition_version` 1).
2. **Trimmed-but-live precommits** (spec gap): a precommit whose create event was trimmed (its +1
   was activated into the LOST snapshot) while the build is still running has NO journal evidence.
   The rebuild therefore also LISTs `cas/manifests/` per namespace and includes edges of every
   manifest that is (a) not named by any committed ref, (b) not named by a live-precommit journal
   binding, and (c) NOT provably build-dead (`!prefixEligible(...)` — the watermark fact). This
   over-protects: an unowned-alive-at-rebuild manifest that later dies without journal evidence
   leaks its edges (bounded, fsck-visible, cleared by a future rebuild). Documented limitation —
   the alternative (refusing while any in-flight build exists) would make live rebuild unusable.
3. **Memory O(budget) via attempt iteration, not a new spill/merge:** edges accumulate per batch of
   namespaces; batch k folds via `foldDeltasIntoGeneration(prior_runs = attempt k-1 runs, scattered
   = batch k edges, attempt = k)`. Superseded attempts are existing attempt-debris. Zero new merge
   code.
4. **`HeartbeatFloor` gains `max_ack`** (one line in the existing mount loop) — the rebuild needs
   the MAX over mount acks for the round mint; DRY over a second enumeration.

## File Structure

- `docs/superpowers/models/CaGcAckFloorCore.tla` (+4 cfgs) — Task 1.
- `Core/CasGc.h/.cpp` — guard (fold classification site ~`CasGc.cpp:696-778`), `RebuildReport`,
  `Gc::rebuildBaseline(bool force)` — Tasks 2, 3.
- `Core/CasServerRoot.h/.cpp` — `HeartbeatFloor.max_ack` — Task 3.
- `src/Parsers/ASTSystemQuery.h`, `src/Parsers/ParserSystemQuery.cpp`,
  `src/Interpreters/InterpreterSystemQuery.cpp`, `src/Access/Common/AccessType.h`,
  `ContentAddressedMetadataStorage.h/.cpp` — Task 4.
- `programs/disks/CommandCaGcRebuild.cpp` (new, after `CommandFsck.cpp`), `programs/disks/DisksApp.cpp` registration — Task 5.
- Tests: `src/Disks/tests/gtest_cas_gc_rebuild.cpp` (new), extensions in `gtest_cas_gc_round.cpp` — Tasks 2, 3.
- Docs: `docs/superpowers/cas/04-gc-protocol.md`, `08-testing-and-soak.md`, `ROADMAP.md` — Task 5.

---

### Task 1: TLA+ gate — `GRebuild` + witness + 3 sabotages

**Files:**
- Modify: `docs/superpowers/models/CaGcAckFloorCore.tla`
- Create: `docs/superpowers/models/CaGcAckFloorCore_witness_rebuild.cfg`,
  `CaGcAckFloorCore_sab_rebuilddropedge.cfg`, `CaGcAckFloorCore_sab_rebuildkeepretired.cfg`,
  `CaGcAckFloorCore_sab_rebuildlowround.cfg`

**Interfaces:**
- Consumes: the existing module (variables `retired`, `round`, `folded`, `wAck`, `gcPhase`; the
  `copyForwardEver` pattern for witness flags; `MaxRound`).
- Produces: constants `SabotageRebuildDropEdge`, `SabotageRebuildKeepRetired`,
  `SabotageRebuildLowRound`; variable `rebuiltEver`; action `GRebuild`; invariant
  `W_RebuildHappens == ~rebuiltEver`.

- [ ] **Step 1: Add the three sabotage CONSTANTS** to the `CONSTANTS` block (comment each), the `rebuiltEver` variable (mirror how `copyForwardEver` was added: `vars`, `Init` gets `rebuiltEver = FALSE`, every action's `UNCHANGED` list gets `rebuiltEver` — reuse the same sed over `copyForwardEver >>` → `copyForwardEver, rebuiltEver >>`).

- [ ] **Step 2: Add the action** after `GComplete`:

```tla
(* Raw rebuild (spec 2026-07-03-cas-gc-rebuild-design.md): recompute the baseline from owner state.
   The model's `folded` IS the owner-derived edge truth, so an honest rebuild keeps it; the retired
   list restarts EMPTY (over-protect) and the round is minted ABOVE every mount ack so no stale ack
   can float a fresh condemnation past the floor unobserved. Runs at idle under the GC lease. *)
MaxAck == CHOOSE m \in Rounds : /\ \E w \in Writers : wAck[w] = m
                                /\ \A w \in Writers : wAck[w] <= m
GRebuild ==
    /\ gcPhase = "idle" /\ round < MaxRound
    /\ LET base == IF SabotageRebuildLowRound THEN 0 ELSE MaxAck
       IN round' = IF base + 1 > MaxRound THEN MaxRound ELSE base + 1
    /\ retired' = IF SabotageRebuildKeepRetired THEN retired ELSE {}
    /\ folded' = IF SabotageRebuildDropEdge /\ folded # {}
                 THEN folded \ {CHOOSE rf \in folded : TRUE}
                 ELSE folded
    /\ rebuiltEver' = TRUE
    /\ UNCHANGED << present, tok, nextTok, deadTok, landed, wStatus, wView, wAck, wPending,
                    gcPhase, minAckL, sparedEver, recreatedEver, deletedEver, copyForwardEver >>
```

Wire `\/ GRebuild` into `Next`; add `W_RebuildHappens == ~rebuiltEver` next to the other witnesses.

- [ ] **Step 3: Create the 4 cfgs** — copy `CaGcAckFloorCore_stage1.cfg`, add the three new
  constants `= FALSE` to ALL existing cfgs (TLC errors on missing constants — update the 12
  existing cfgs too), then: witness cfg checks `W_RebuildHappens` only; each sabotage cfg sets its
  one flag TRUE and checks `INV_NO_DANGLE INV_NO_RETURN` (keep-retired: also `INV_NO_RETURN` is
  the expected violation via a wrong-token graduate — if TLC shows the violation on
  `INV_NO_DANGLE` instead, record which fired in the commit message; the requirement is: a
  counterexample exists).

- [ ] **Step 4: Run** from `docs/superpowers/models/`:
  `./run_ackfloor.sh CaGcAckFloorCore_stage1` (expect: clean, "No error"),
  the witness cfg (expect: violation = reachable), the 3 sabotage cfgs (expect: violation each),
  plus re-run the 7 old sabotage cfgs + 4 old witnesses (expect: unchanged results).

- [ ] **Step 5: Commit** `git add docs/superpowers/models && git commit -m "CAS gc-rebuild Task 1: TLA+ gate — GRebuild action, witness, 3 sabotages"`.

---

### Task 2: the guard — fail-closed on trimmed history without a baseline

**Files:**
- Modify: `Core/CasGc.cpp` (fold per-shard classification, ~lines 696-778) + the (б) seal-absence audit near `readFoldSeal`/`readSealedCursors` (`CasGc.cpp:1142-1157`)
- Test: `src/Disks/tests/gtest_cas_gc_rebuild.cpp` (create; suite `CasGcBaselineGuard`)

**Interfaces:**
- Consumes: `readSealedCursors(generation, attempt) -> std::map<String, ShardCoverage>` (empty =
  fresh pool); per-shard fold site where `RootShard peek` and the shard's `cursorKey(ns, shard)`
  coverage entry are both in scope.
- Produces: `ErrorCodes::CORRUPTED_DATA` throw with the message below (Tasks 3-5 reference it in
  docs); no API change.

- [ ] **Step 1: Write the failing tests** (create the file with `./tests` conventions of `gtest_cas_gc_ack_floor.cpp`: same includes, `openStoreForTest`, `writeBlobBody`/`writeManifestRaw`/`publishCommittedTransition`/`dropRefTransition`, `Gc gc(store, kGc)`):

```cpp
/// Guard (spec Part 1): a fold with NO baseline must refuse when a shard journal proves trimmed
/// history — otherwise a fresh GC folds only the surviving tails and mass-deletes live data.
TEST(CasGcBaselineGuard, FreshStateOverTrimmedJournalsFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);   /// gc_trim_min_events = 0 => eager trim
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    gc.runRegularRound();   /// second round trims the folded events (eager gate)

    /// Disaster: gc/state vanishes on a lived-in pool.
    ASSERT_TRUE(backend->head(store->layout().gcStateKey()).exists);
    const Token t = backend->head(store->layout().gcStateKey()).token;
    ASSERT_EQ(backend->deleteExact(store->layout().gcStateKey(), t).kind, DeleteOutcome::Kind::Deleted);

    /// A fresh GC (fresh leader id — the old lease died with the state) must REFUSE, not delete.
    Gc gc2(store, hexToU128("00000000000000000000000000000002"));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists)
        << "the guard must fire BEFORE any destructive step";
}

/// A genuinely fresh pool (journals start at version 1) passes the guard — rounds run as today.
TEST(CasGcBaselineGuard, GenuinelyFreshPoolIsUnaffected)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    EXPECT_NO_THROW(gc.runRegularRound());
}

/// (б) audit: snap_generation > 0 whose adopted fold seal is ABSENT must be CORRUPTED_DATA,
/// never silently treated as an empty baseline.
TEST(CasGcBaselineGuard, AbsentAdoptedSealFailsClosed)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    const ManifestRef r = ref("srv-a:1", 1, 0xAA);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeManifestRaw(*backend, store->layout(), ns, r, {blobEntryFor("a", DB::UInt128(1))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl", std::nullopt, r);
    Gc gc(store, kGc);
    gc.runRegularRound();

    /// Corrupt (б): delete the adopted fold seal out from under a healthy gc/state.
    const GcState st = decodeGcState(backend->get(store->layout().gcStateKey())->bytes);
    const String seal_key = store->layout().foldSealKey(st.snap_generation, st.snap_attempt);
    ASSERT_TRUE(backend->head(seal_key).exists);
    backend->deleteExact(seal_key, backend->head(seal_key).token);

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc.runRegularRound(); });
}
```

- [ ] **Step 2: Run to verify they fail**: `ninja -C build unit_tests_dbms > build/build_rebuild_t2.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasGcBaselineGuard.*'`. Expected: test 1 FAILS (today the fresh round runs and deletes), test 3 FAILS if the absent seal is treated as empty (if it already throws `CORRUPTED_DATA`, record that the audit found the path closed and keep the test as a pin), test 2 PASSES (sanity).

- [ ] **Step 3: Implement the guard** at the fold's per-shard classification (the site computing `cov.classification` from `peek` — `CasGc.cpp` ~696-778). Add BEFORE events are folded for a shard, where `sealed` (the `readSealedCursors` map) and the shard's `cursor_key` are in scope:

```cpp
        /// Baseline guard (spec 2026-07-03-cas-gc-rebuild-design.md Part 1): a shard with NO sealed
        /// cursor whose journal PROVES trimmed history means the baseline that folded (and allowed
        /// trimming) those events is gone — folding from scratch would mass-condemn every blob whose
        /// edges lived only in the lost snapshot. Fail closed; recovery is the explicit rebuild.
        /// A shard born after the baseline passes: its journal starts at transition_version 1.
        const bool has_sealed_cursor = sealed.contains(cursor_key);
        const bool proves_trim = peek.journal.empty()
            ? peek.shard_version > 0
            : peek.journal.front().transition_version > 1;
        if (!has_sealed_cursor && proves_trim)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS GC baseline guard: ref shard {}/{} journal starts at transition_version {} "
                "(shard_version {}) but no sealed baseline covers it — gc/state was lost or "
                "regressed. GC refuses to run; recover with SYSTEM CONTENT ADDRESSED GC REBUILD.",
                ns.string(), shard,
                peek.journal.empty() ? 0 : peek.journal.front().transition_version,
                peek.shard_version);
```

For the (б) audit: where `readSealedCursors`/`readFoldSeal` return empty/`nullopt`, add the state
cross-check — `snap_generation > 0` AND seal absent ⇒ `CORRUPTED_DATA` ("adopted fold seal {} is
missing under a live gc/state — run SYSTEM CONTENT ADDRESSED GC REBUILD"); `snap_generation == 0`
keeps the empty-baseline meaning.

- [ ] **Step 4: Run** the suite: all `CasGcBaselineGuard.*` PASS, then FULL `Cas*` (guard must not trip any existing test — if one trips, it found a test writing trimmed journals without a baseline: fix the TEST setup, never weaken the guard; report it in the task summary).

- [ ] **Step 5: Commit** `git add src/Disks && git commit -m "CAS gc-rebuild Task 2: baseline guard — fold fails closed on trimmed history without a sealed cursor"`.

---

### Task 3: `Gc::rebuildBaseline`

**Files:**
- Modify: `Core/CasGc.h` (RebuildReport + method decl), `Core/CasGc.cpp` (implementation),
  `Core/CasServerRoot.h/.cpp` (`HeartbeatFloor.max_ack`)
- Test: `src/Disks/tests/gtest_cas_gc_rebuild.cpp` (suite `CasGcRebuild`)

**Interfaces:**
- Consumes: `discoverUniverse()` (`CasGc.cpp:1159`), `foldManifestEdges(const ManifestId &, int
  sign, std::vector<BlobDelta> &, std::map<ManifestId, Token> &)` (`CasGc.cpp:448`),
  `foldDeltasIntoGeneration(backend, layout, prior_runs, new_generation, attempt, shard, scattered,
  out_runs)` (defaults leave retired/condemn OFF — exactly right for rebuild),
  `acquireOrRenewLease(GcState &, Token &)` (`CasGc.cpp:1534`), `computeHeartbeatFloor(...)`
  (`CasServerRoot.cpp:406`), `prefixEligible(Store &, const RootNamespace &, const BuildPrefix &)`
  (`CasOrphanManifestSweep.h:42`), `encodeFoldSeal`/`encodeGcState`, `putDeterministicArtifact`.
- Produces:

```cpp
struct RebuildReport
{
    bool performed = false;          /// false = refused; `refusal` says why
    String refusal;                  /// human-readable refusal reason (empty when performed)
    uint64_t round = 0;              /// minted round
    uint64_t generation = 0;         /// minted generation
    uint64_t namespaces = 0, shards = 0, committed_refs = 0, live_precommits = 0;
    uint64_t unowned_alive_manifests = 0;   /// over-protect class (documented leak bound)
    uint64_t edges = 0, clamped_shards = 0;
};
RebuildReport Gc::rebuildBaseline(bool force);
```

- [ ] **Step 1: Write the failing tests** (same fixture family; the core scenarios from spec Testing gates 2-7):

```cpp
/// (а): lose gc/state on a lived-in pool -> guard blocks rounds -> rebuild -> rounds converge:
/// dropped blobs reclaimed, live blobs intact, round minted above every mount ack.
TEST(CasGcRebuild, RecoversLostStateAndConverges)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const RootNamespace ns{"00/aa@cas@"};
    /// live part keeps blob 1; dropped part released blob 2 (its -1 already folded and trimmed).
    const ManifestRef live_r = ref("srv-a:1", 1, 0xA1);
    const ManifestRef dead_r = ref("srv-a:1", 2, 0xA2);
    writeBlobBody(*backend, store->layout(), DB::UInt128(1));
    writeBlobBody(*backend, store->layout(), DB::UInt128(2));
    writeManifestRaw(*backend, store->layout(), ns, live_r, {blobEntryFor("a", DB::UInt128(1))});
    writeManifestRaw(*backend, store->layout(), ns, dead_r, {blobEntryFor("b", DB::UInt128(2))});
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_live", std::nullopt, live_r);
    publishCommittedTransition(*backend, store->layout(), ns, "tbl_dead", std::nullopt, dead_r);
    Gc gc(store, kGc);
    gc.runRegularRound();
    dropRefTransition(*backend, store->layout(), ns, "tbl_dead", dead_r);
    gc.runRegularRound();   /// -1 folds; trim cuts (eager)
    store->renewWatermarkOnce();   /// mount ack advances to the current round

    const Token t = backend->head(store->layout().gcStateKey()).token;
    backend->deleteExact(store->layout().gcStateKey(), t);

    Gc gc2(store, hexToU128("00000000000000000000000000000003"));
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { gc2.runRegularRound(); });

    const RebuildReport rep = gc2.rebuildBaseline(/*force*/ false);
    ASSERT_TRUE(rep.performed) << rep.refusal;
    /// Round strictly above the mount's stale ack (the ack-floor mint rule).
    const auto mount_got = backend->get(store->layout().mountKey("test"));
    ASSERT_TRUE(mount_got.has_value());
    EXPECT_GT(rep.round, decodeMountLease(mount_got->bytes).observed_gc_round);
    EXPECT_EQ(rep.committed_refs, 1u);

    /// Regular rounds converge: blob 2 (unreferenced) reclaimed, blob 1 intact.
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc2, *backend, store->layout(), DB::UInt128(2)));
    EXPECT_TRUE(backend->head(store->layout().blobKey(BlobId(u128ToHex(DB::UInt128(1))))).exists);
}

/// (б): a run object named by a healthy state is corrupt -> plain rebuild (no FORCE) recovers.
TEST(CasGcRebuild, RecoversCorruptGenerationArtifact)
{ /* publish -> round -> overwrite one blob_target_run object with garbage bytes via
     putOverwrite(key, "garbage", token) -> regular round throws (CORRUPTED_DATA) ->
     rebuildBaseline(false) performed (health check sees the broken artifact) -> converges. */ }

/// FORCE: a healthy state refuses plain rebuild; FORCE rebuilds and converges.
TEST(CasGcRebuild, HealthyStateRequiresForce)
{ /* publish -> round -> rebuildBaseline(false): performed == false, refusal mentions FORCE ->
     rebuildBaseline(true): performed -> a subsequent regular round runs clean. */ }

/// Refusal: a committed owner with a MISSING manifest body is data loss — rebuild refuses and
/// names the owner; nothing was written (gc/state still absent).
TEST(CasGcRebuild, MissingCommittedManifestRefuses)
{ /* publish A + B -> rounds+trim -> delete gc/state AND delete B's manifest body ->
     rebuildBaseline(false): performed == false, refusal contains "tbl_b";
     backend->head(gcStateKey()).exists == false. */ }

/// Live precommit with a durable body: its edges are INCLUDED (no clamp needed);
/// one with an ABSENT body clamps that shard's cursor below the precommit transition.
TEST(CasGcRebuild, LivePrecommitEdgesIncludedAbsentBodyClamps)
{ /* append a create-precommit journal event via appendOwnerEvent test helper with body present
     for shard A (edges included: rep.live_precommits == 1) and body absent for shard B
     (rep.clamped_shards == 1); rebuild performed; a later regular round folds shard B's event
     once the body lands. */ }

/// Trimmed-but-live precommit (design delta 2): an unowned, not-provably-dead manifest body's
/// edges are included (over-protect) and counted in unowned_alive_manifests.
TEST(CasGcRebuild, UnownedAliveManifestOverProtected)
{ /* writeManifestRaw WITHOUT any owner event; keep the writer's mount alive so
     prefixEligible == false (not provably dead) -> rebuild -> rep.unowned_alive_manifests == 1;
     subsequent rounds do NOT condemn its blob. */ }

/// Live-writer race: a publish lands AFTER the rebuild read a shard -> the event sits above the
/// recorded cursor and the next regular round folds it (no lost +1).
TEST(CasGcRebuild, PublishAfterShardReadIsFoldedNextRound)
{ /* use a delegating backend that, on the FIRST get() of the target shard key, first forwards the
     read, then (once) publishes another committed transition into that shard through the RAW
     helpers (the same trick as HeadThenDisplaceOnceBackend in gtest_cas_build.cpp) -> rebuild ->
     regular round -> the late part's blob has in-degree (not condemned after further rounds). */ }

/// O(budget) attempt iteration: a tiny edge budget forces multiple batches; the final in-degree
/// equals the single-batch result.
TEST(CasGcRebuild, BatchedRebuildMatchesSingleBatch)
{ /* N namespaces x M refs; run rebuildBaseline with rebuild_edge_budget forced to a few edges
     (test hook: setRebuildEdgeBudgetForTest(4)); assert previewDeletes/zeroInDegree equals the
     default-budget rebuild on an identical second backend. */ }
```

- [ ] **Step 2: Run to verify failure**: compile error (`rebuildBaseline` undeclared) — expected RED.

- [ ] **Step 3: `HeartbeatFloor.max_ack`** — in `Core/CasServerRoot.h` add `uint64_t max_ack = 0;`
  to `HeartbeatFloor`; in `computeHeartbeatFloor`'s mount loop add
  `floor.max_ack = std::max(floor.max_ack, lease.observed_gc_round);` for EVERY decoded mount
  (including fenced/terminated — a stale ack from a fenced mount still poisons a low round).
  Extend one existing `CasHeartbeatFloor` test with a `max_ack` assertion.

- [ ] **Step 4: Implement `rebuildBaseline`** in `CasGc.cpp` (structure; follow the anchors):

```cpp
Gc::RebuildReport Gc::rebuildBaseline(bool force)
{
    RebuildReport rep;
    Backend & backend = store->backend();
    const Layout & layout = store->layout();

    /// 1. Lease (same primitive as the round; a refused lease = another leader lives).
    ///    Reuse acquireOrRenewLease against the CURRENT state when decodable; when gc/state is
    ///    absent/undecodable, mint a lease-bearing bootstrap the same way the fresh-pool path does
    ///    (CasGc.cpp:1549-1554) but DO NOT CAS it yet — the rebuild's own final CAS is the commit.
    /// 2. Health check (б-detector): state decodes AND foldSealKey(snap_generation, snap_attempt)
    ///    present AND every seal-referenced run + every retired_refs object HEAD-present.
    ///    healthy && !force  => rep.refusal = "... healthy state; re-run with FORCE", return rep.
    /// 3. Universe: discoverUniverse(); group by namespace. For each shard: readShard once,
    ///    record (shard_version, token) into a coverage draft; decode failure => refusal.
    /// 4. Owner replay per shard:
    ///      committed: root.refs -> for each: ManifestId{ns, manifest_ref};
    ///      live precommits: replay root.journal in order (old_binding erases, new_binding inserts;
    ///      keep entries with owner_kind == Precommit).
    /// 5. Edges: for each committed id: foldManifestEdges(id, +1, deltas, mf_cleanup_unused)
    ///    returning false (missing/invalid body) => rep.refusal names ns/ref, return rep.
    ///    For each live precommit id: body present ? edges : clamp coverage below its transition
    ///    (classification = 4, folded_cursor = transition_version - 1), ++rep.clamped_shards.
    /// 6. Unowned-alive sweep (design delta 2): LIST manifestNamespacePrefix(ns); for keys not in
    ///    {committed ∪ live-precommit} manifest keys: parseBuildPrefix + !prefixEligible(...) =>
    ///    decode body; valid => edges + ++rep.unowned_alive_manifests; invalid/undecodable body
    ///    for an UNOWNED key is debris => skip (never a refusal — no owner claims it).
    /// 7. Numbering: floor = computeHeartbeatFloor(backend, layout, now_ms(), skew);
    ///    round = max(floor.max_ack, max shard fence_round, max surviving gc/gen number) + 1;
    ///    generation likewise above surviving gc/gen prefixes (LIST layout.gcGenPrefix()).
    /// 8. Batched fold: partition deltas by gc-shard (existing shardOfBlobHash reducer used by the
    ///    round's fold); within each gc-shard, feed batches of <= rebuild_edge_budget edges through
    ///    foldDeltasIntoGeneration(backend, layout, prior_runs=prev attempt's out_runs, generation,
    ///    attempt=k, shard, batch, out_runs). Final attempt's out_runs -> seal.blob_target_runs.
    /// 9. Seal: per_ns_shard[cursorKey(ns, shard)] = ShardCoverage{classification=2 (or 4 clamp),
    ///    folded_token, folded_cursor = shard_version (or clamp)}; putDeterministicArtifact at
    ///    foldSealKey(generation, final_attempt).
    /// 10. GcState: round/generation/attempt as minted; retired_refs = {}; gc_shards from config;
    ///     lease = ours; manifest_sweep_cursor = "". CAS: casPut(gcStateKey, body, observed token)
    ///     — expected-absent for (а), the observed broken/old token for (б)/(в)/(г). Conflict =>
    ///     refusal ("state changed under the rebuild — re-run").
    /// 11. Fill rep counters; emit a `gc_rebuild` CasEvent (outcome performed/refused, detail =
    ///     the counters); return rep.
}
```

Add `uint64_t rebuild_edge_budget = 8'000'000;` to `PoolConfig` (~256 MB of `BlobDelta`) + a
`setRebuildEdgeBudgetForTest` hook on `Gc`.

- [ ] **Step 5: Run** `CasGcRebuild.*` + `CasGcBaselineGuard.*` + full `Cas*`. All green.

- [ ] **Step 6: Commit** `git commit -m "CAS gc-rebuild Task 3: Gc::rebuildBaseline — lease, owner replay, over-protect edges, attempt-batched fold, minted round above all acks"`.

---

### Task 4: `SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE]`

**Files:**
- Modify: `src/Parsers/ASTSystemQuery.h` (enum next to `CONTENT_ADDRESSED_GARBAGE_COLLECTION` at
  line ~149 + its string), `src/Parsers/ParserSystemQuery.cpp` (mirror the GC-collection branch:
  optional disk name, optional trailing `FORCE` keyword), `src/Interpreters/InterpreterSystemQuery.cpp`
  (mirror lines ~2185-2210), `src/Access/Common/AccessType.h` (reuse the SAME access type the
  existing CONTENT ADDRESSED GARBAGE COLLECTION uses — no new grant),
  `ContentAddressedMetadataStorage.h/.cpp` (`Cas::RebuildReport runGcRebuildNow(bool force)` —
  construct `Cas::Gc` exactly as `runGarbageCollectionRoundNow` does, call `rebuildBaseline`,
  forward the report into a `GcRoundLogRecord` Finish row with `trigger = Manual` and the counters
  in `profile_events`-adjacent fields or `error` on refusal).
- Test: `src/Disks/tests/gtest_cas_gc_rebuild.cpp` — unit-level: `runGcRebuildNow` on a metadata
  storage over InMemory (the parser/interpreter wiring itself is compile-checked; a stateless SQL
  test is NOT required for this internal command at this stage).

**Steps:** failing unit test (`runGcRebuildNow` undeclared) → implement → suite green → full
`clickhouse` link (`ninja -C build clickhouse`, log to `build/build_rebuild_t4.log`) → commit
`"CAS gc-rebuild Task 4: SYSTEM CONTENT ADDRESSED GC REBUILD [FORCE]"`.

**Refusal semantics at the SQL layer:** `performed == false` ⇒ throw `BAD_ARGUMENTS` with
`rep.refusal` (the operator must SEE the refusal, not a silent OK); the log row records it too.

---

### Task 5: `clickhouse-disks ca-gc-rebuild` + docs + memory

**Files:**
- Create: `programs/disks/CommandCaGcRebuild.cpp` — copy the `CommandFsck.cpp` shape: read-only
  open REQUIRED (same check + message: the tool must never claim the live server's mount);
  options `--force`; body = construct `Cas::Gc gc(ca->store(), <random u128 via thread_local_rng>)`,
  `const auto rep = gc.rebuildBaseline(force);`, print the report key=value one-per-line
  (`performed= round= generation= committed_refs= live_precommits= unowned_alive_manifests= edges=
  clamped_shards=`), on refusal print `refusal=<text>` and `throw Exception(BAD_ARGUMENTS, ...)`
  (nonzero exit). Register in `programs/disks/DisksApp.cpp` + `CMakeLists` next to `makeCommandFsck`.
- Verify (part of this task): a READ-ONLY-opened store performs gc-plane writes — the read-only
  gate lives on the metadata-storage mutation API and `mayMutate`/mount fencing gates `mutateShard`
  only; `rebuildBaseline` writes via `backend()` directly. If a read-only open blocks `Store`
  construction of GC machinery, thread a `gc_maintenance` open flag — smallest change that lets
  `Gc` run; document it in the command's header comment.
- Modify: `docs/superpowers/cas/04-gc-protocol.md` (§gc-rebuild: guard semantics, algorithm
  summary, round-mint rule, over-protect limitation from design delta 2, refusal list),
  `docs/superpowers/cas/08-testing-and-soak.md` (operator runbook: symptoms — the guard's
  `CORRUPTED_DATA` message / stuck `Error` rounds in the gc log — then the two command forms),
  `docs/superpowers/cas/ROADMAP.md` (flip the rebuild row to DONE, pointer to spec+docs).
- Test: lease-conflict refusal — hold the GC lease via a live `Gc` (`acquireOrRenewLease` through a
  first `runRegularRound`) and assert `rebuildBaseline` on a SECOND `Gc` instance refuses with the
  lease reason and writes nothing.
- Memory: update `project_ca_gc_ack_floor_fence.md` follow-ups (rebuild DONE) + one-line MEMORY.md
  hook if a new memory file is warranted (it is not — the repo docs carry it).

**Steps:** failing lease-conflict test → implement command + registration → `ninja -C build
clickhouse` + run `clickhouse-disks --disk <ca_ro> ca-gc-rebuild` smoke against an InMemory-backed
config is NOT possible (disks needs a real config) — the unit test carries the semantics; the
command body is a thin shell → docs edits → full `Cas*` suite + link → commit
`"CAS gc-rebuild Task 5: clickhouse-disks ca-gc-rebuild + operator docs"`.

---

## Validation (queued, not in this plan)

Soak scenario: mid-soak `mc rm` of `gc/state` → assert the guard's `CORRUPTED_DATA` appears in the
gc round log (`outcome='Error'`) and NOTHING is deleted afterwards → `SYSTEM CONTENT ADDRESSED GC
REBUILD` on one replica → rounds resume, fsck converges to `dangling=0`, `pending-gc` drains.

## Self-review notes

- Spec coverage: Part 1 → Task 2; Part 2 → Task 3; surfaces → Tasks 4-5; TLA+ → Task 1; testing
  gates 1→T2, 2-7→T3, refusals→T3+T5 (lease), memory-budget→T3 test 8. Design deltas 1-4 recorded
  above and to be folded back into the spec file by whoever executes Task 5 (docs step).
- The guard lands BEFORE the rebuild exists (Task 2 independent) — matches spec "ships first".
- No new scanner/merge classes anywhere: `discoverUniverse`, `foldManifestEdges`,
  `foldDeltasIntoGeneration`, `computeHeartbeatFloor`, `prefixEligible` are the whole engine.
