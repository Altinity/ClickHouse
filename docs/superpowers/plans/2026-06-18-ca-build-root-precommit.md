# Build-root / precommit implementation plan (B171)

> **For agentic workers:** TDD, frequent commits. Implementation spec:
> `docs/superpowers/specs/2026-06-18-ca-build-root-precommit-cpp-impl.md` (read it — it has the
> file-by-file detail). Design: `…-build-root-precommit-design.md`. TLA+ proof:
> `models/CaBuildRootPrecommit*` (buggy reproduces, fixed exhaustive-clean).

**Goal:** Make B140-dangle impossible by protecting a build's referenced objects via reachability from
a durable build root (precommit), with a fail-closed commit, instead of the revocable `cas_owner`
hint.

**Branch:** `cas-mergetree-poc`. New commits only (no amend/rebase). Allman braces. Build logs →
`tmp/`; a subagent analyzes each build log and returns a summary (never dump full logs).

**Build/test commands:**
- Build the unit test binary: `ninja -C build unit_tests_dbms > tmp/b171_build_<task>.log 2>&1`
  (the `build` dir was compiled today; confirm). Grep the log for `error:`/`FAILED`.
- Run gtests: `build/src/unit_tests_dbms --gtest_filter='CasBuildRoot*:CasGc*.*:CasGcRound.*:CasGcFold.*:CasReuseGcRace.*:CasEvent.*:CasGcRetire.*' > tmp/b171_test_<task>.log 2>&1`.
  (Find the actual binary path; it may be `build/src/unit_tests_dbms`.) The intentional red
  `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` is expected to fail — ignore it.

---

## Task 1: RED — taxonomy + build-root layout + `precommit` stub + failing dangle test

**Files:** `Core/CasEvent.{h,cpp}`, `Core/CasLayout.h`, `Core/CasBuild.{h,cpp}`, `Core/CasStore.{h,cpp}`,
`src/Disks/tests/gtest_cas_build_root_dangle.cpp` (new; use the in-memory backend like
`gtest_cas_event_log.cpp` / `gtest_cas_b140_dangle.cpp`).

- [ ] **Step 1:** Add `CasEventType::Precommit`, `PrecommitRemoved`, `PrecommitReclaim` + `toString`
  (snake_case `precommit`/`precommit_removed`/`precommit_reclaim`).
- [ ] **Step 2:** `Layout`: reserve `_builds` in `checkNamespace` (mirror `_files`); add
  `static bool isBuildRootNamespace(const RootNamespace &)` (string starts with `"_builds/"`).
- [ ] **Step 3:** `Build`: declare+define `void precommit(const TreeId & manifest)` as a STUB that does
  nothing yet (so the test compiles and the dangle still reproduces). Add helpers `RootNamespace
  buildRootNs() const` (= `_builds/<server_hex>`) and `uint64_t buildShard() const` (= `build_seq`).
- [ ] **Step 4:** Write `gtest_cas_build_root_dangle.cpp` with
  `CasBuildRootDangle.SharedBlobSurvivesSourceDropDuringBuild`:
  - Build A: `putBlob(b)`, `putTree(t1=[b])`, `publish(ns="srv/tbl", "refA", t1)`; let A's build retire
    (dtor) so `min_active` advances past A.
  - Build B: `startBuild`; `reuseBlob(b)` (adopt — dedup); `putTree(t2=[b])`; **`precommit(t2)`**.
  - `dropRef(ns,"refA")`; `renewWatermarkOnce()`; run GC to fixpoint.
  - Build B `publish(ns,"refB",t2)`.
  - **Assert:** `backend->head(layout.blobKey(b)).exists` (refB's closure present — no dangle), and
    `resolveRef(ns,"refB")` resolves with the blob readable. Drive GC with the existing test helper
    pattern (`runGcToFixpoint`).
- [ ] **Step 5:** Build `unit_tests_dbms`; run the new test → expect **RED** (precommit is a stub, GC
  deletes b under the retired A-owner, B publishes a dangle). Confirm it fails for the RIGHT reason
  (blob deleted), not a harness error.
- [ ] **Step 6:** Commit `CA B171 (RED): build-root taxonomy + precommit stub + failing dangle repro`.

## Task 2: Implement `precommit` + GC folds the build root (pending-tolerance)

**Files:** `Core/CasBuild.cpp`, `Core/CasGc.cpp`.

- [ ] **Step 1:** Implement `Build::precommit`: `store->ensureRegistered(buildRootNs())`; `store->
  mutateShard(buildRootNs(), buildShard(), [&](RootShard & r){ r.refs["part"] = RefPayload{...tree_id=
  manifest...}; r.journal.push_back(Add{...}); })`. Emit `CasEvent::Precommit` (non-empty reason).
- [ ] **Step 2:** GC fold: the build-root namespace is discovered via the registry (precommit
  registered it) and folded by `foldShardRecords` → root edge → `tree_expand` → child in-degree. Add
  **pending-tolerance**: at the fold site that fails closed on a live-ref→missing-tree (the
  `FailClosed`/INV-NO-DANGLE guard), SKIP the alarm when `Layout::isBuildRootNamespace(ns)` — a
  build-root edge to an absent target is legal (pending/aborting). Thread `ns`/its build-root-ness to
  that site (the fold already has `ns`).
- [ ] **Step 2b:** Ensure GC does not attempt to delete an absent object referenced only by a build
  root (no present object → nothing to delete; the candidate set is present objects, so this should be
  automatic — verify).
- [ ] **Step 3:** Build; run the dangle test → b should now SURVIVE (in-degree ≥ 1 via build root) even
  though `protectedByLiveBuild` would have failed. Test may still fail on the commit step until Task 3
  (fail-closed) if any window remains — note progress. Run the full filter; keep other tests green.
- [ ] **Step 4:** Commit `CA B171: implement precommit + GC build-root fold with pending-tolerance`.

## Task 3: Fail-closed commit + remove precommit

**Files:** `Core/CasBuild.cpp`.

- [ ] **Step 1:** In `Build::publish`, make the closure presence-verification **unconditional**: always
  call `revalidateDeps()` at the top of the commit `mutateShard` lambda (drop the `if (view.round() <
  fence_round)` gate; fold/keep `gateCheckDeps`). Every dep proven present; missing non-recreatable
  blob → `ABORTED` (caller retries). This is INV-COMMIT-FAILCLOSED.
- [ ] **Step 2:** After the successful table-shard CAS returns, remove the precommit:
  `store->dropRef(buildRootNs(), "part")`; emit `CasEvent::PrecommitRemoved`. Order: table ref Add
  committed FIRST, THEN precommit removed. A transient `dropRef` failure is safe to leave (GC reclaims
  it); emit/log.
- [ ] **Step 3:** Add test `CasBuildRootDangle.PrematureReclaimCommitFailsClosed`: simulate the
  precommit being reclaimed mid-build (drop the build-root ref + advance min_active), delete the
  now-unprotected blob, then `publish` → expect `ABORTED` (throws), never a dangle.
- [ ] **Step 4:** Build; run both dangle tests → GREEN (the primary repro now passes end-to-end). Full
  filter green.
- [ ] **Step 5:** Commit `CA B171: fail-closed commit (unconditional revalidate) + remove precommit`.

## Task 4: Precommit reclaim + delete `protectedByLiveBuild` + delete `cas_owner`

**Files:** `Core/CasGc.cpp`, `Core/CasBuild.{h,cpp}`, `Core/CasStore.{h,cpp}`.

- [ ] **Step 1:** GC precommit reclaim: while folding a build-root shard, derive `(server_hex,
  build_seq)` from `ns`/shard; consult `watermarkOf(server)` + the K=2 liveness verdict; if the server
  is dead (epoch mismatch/farewell) OR `build_seq < min_active`, the precommit is abandoned → remove
  the build-root ref (`mutateShard` drop + journal Remove). Emit `CasEvent::PrecommitReclaim`.
- [ ] **Step 2:** Delete `Gc::protectedByLiveBuild` and its retire-decision call site (the `skip:
  protectedByLiveBuild` branch). Retire decision becomes: present ∧ known ∧ inDeg=0 ⇒ condemn. KEEP
  `watermarkOf` + the liveness verdict (now used by Step 1). Remove the now-dead per-candidate
  protection plumbing if unused.
- [ ] **Step 3:** Delete `Build::ownerMeta()`; replace `putIfAbsentStream(key, ownerMeta())` /
  `putOverwrite(..., ownerMeta())` with no-owner-metadata variants. Keep `minActive`/`allocateBuildSeq`
  /`retireBuildSeq`.
- [ ] **Step 4:** Add test `CasBuildRoot.AbandonedPrecommitReclaimed`: publish a precommit, retire the
  build (advance min_active), GC → precommit reclaimed → the exclusively-owned blob becomes collectable
  (deleted by a later GC round).
- [ ] **Step 5:** Build; run full filter. Update/repair any test that asserted on
  `protectedByLiveBuild`/`cas_owner` to the reachability model. Green.
- [ ] **Step 6:** Commit `CA B171: precommit reclaim; delete protectedByLiveBuild + cas_owner`.

## Task 5: Bridge precommit into the integration build flow + full-suite green

**Files:** `MetadataStorages/ContentAddressed/*` (the transaction/build driver that calls `startBuild`/
`publish`), `Core/*` as needed.

- [ ] **Step 1:** Find where the metadata-storage transaction drives a `Build` (startBuild → putBlob/
  reuseBlob/putTree → publish). Insert a `build->precommit(manifestTree)` call after the manifest tree
  is assembled and BEFORE `publish` (and before the source refs of any adopted/dedup'd objects could
  be dropped). For a merge/replace that consumes source parts, precommit before the source-removal
  commit.
- [ ] **Step 2:** Build `clickhouse` + `unit_tests_dbms`. Run the full CA gtest filter + a quick
  smoke of the inline-CA-disk stateless pattern if cheap. Green.
- [ ] **Step 3:** Commit `CA B171: wire precommit into the integration build/commit flow`.

## After the plan
Rebuild `clickhouse`; run the 12h chaos soak (WORKERS=2, B170 event log on, 20-min watcher,
keep-alive on failure); verify zero dangling / zero CORRUPTED_DATA and that `precommit`/
`precommit_removed`/`precommit_reclaim` events behave. (Task #141.)
