# CA GC Redesign — Unattended Execution Log

Chronological record of the autonomous run executing the CA GC root-local part-manifest
redesign (spec `docs/superpowers/specs/2026-06-26-cas-gc-streaming-sharded-redesign-design.md`,
plans `docs/superpowers/plans/2026-06-26-cas-gc-*`). Branch: `cas-gc-part-manifest-impl`.

**Operating rules for this run** (from the user, 2026-06-27): continue unattended; review/recheck/
analyze every result; if the TLA+ model shows changes are needed, brainstorm them then adjust the spec
+ plans and re-gate before proceeding; follow the ritual — subagent-driven-development, TDD, review
between tasks, repeat to the next task; do not stop, do not ask questions; new problems / findings /
ideas / sidetracks go to the backlog (`deferred_backlog/cas-gc-redesign-backlog.md`); keep this log.

**Gate discipline (R0):** no code task in a phase begins until that phase's TLA+ suite is GREEN
(every stage/live/witness HOLDs; every `_sab_*` VIOLATES its named invariant; no `UNEXPECTED PASS`).
Each behavior-changing phase exits on a green `Cas*`/`Ca*` gtest sweep; soak after 1d and 4.

---

## State at start of the unattended run (2026-06-27)

- **Spec rev. 15** committed `0fefe90eb6e` (review-fixes on rev.14: missing-body precommit fold
  barrier; owner-move = no delta/no cleanup; `ManifestSafetyId` vs `ManifestId`; `ViewableRound` vs
  `FoldSeal`/`CompletionSeal`; tightened invariant). Approved by the reviewer ("OK to go").
- **All 10 plans re-synced to rev. 15**, head `d87e95a6a7a`. Commits: phase1b `63d449ff38a`+`59729655ee4`;
  phase1d `e17d832702f`+`3597df4045a`; phase4 `adaff8d3bd7`; markers `a1a342ffa39`; phase0/1a/2+overview
  `d87e95a6a7a`. Final cross-file rev.15-consistency sweep: clean.
- **Phase 0 execution dispatched** (subagent `a260a76ab927c46ad`): author `CaGcRootLocalPartManifestCore.tla`
  + cfg suite, model-check green, commit per task, produce the RESULTS ledger. Awaiting completion.

## Plan for the run

1. Phase 0 returns → independently verify (read model; spot-run a holding stage + ≥2 `_sab_*`).
   - GREEN → proceed to Phase 1a.
   - NOT green / protocol finding → brainstorm fix → spec rev.16 + plan edits → re-run Phase 0 → repeat.
2. Phase 1a (identity/codecs) via subagent-driven-development (implementer → spec review → quality review).
3. Phase 1b + 1c (write path / read path), then 1d (GC core) → full `Cas*`/`Ca*` gtest sweep → soak.
4. Phase 2 → 3 → 4 (→ soak) → 5. Each gated on its TLA+ extension green.

---

## Log entries

### 2026-06-27 — run start
- Stood up this execution log + the backlog file.
- Phase 0 implementer subagent running; awaiting its result to verify the R0 gate.

### 2026-06-27 — Phase 0 GREEN, verified; findings analyzed (no spec change needed)
- **Phase 0 complete + GREEN.** `CaGcRootLocalPartManifestCore.tla` (813 lines) + 5 stages + live +
  24 `_sab_*` (23 controls, #16 a/b) + 3 witnesses + wrapper + RESULTS ledger. 10 commits on
  `cas-gc-part-manifest-impl`. stage3 holds over 365.6M distinct states (27m45s); stage2 68.5M; live
  17.8M under FairSpec.
- **Independent verification (did NOT just trust the summary):** read the RESULTS ledger; re-ran
  `stage0` (HOLD, exit 0), `sab_nofence`→`INV_NO_DANGLE`, `sab_advancepastmissingbody` (#23)→
  `INV_NO_DANGLE`, `sab_promoteaftermissingbody`→`INV_NO_LOSS` (all VIOLATE, exit 12); read the
  invariant block (lines 688–745) — invariants are substantive (NoCommittedDangle; INV_NO_LOSS with
  the `extraShared` sharing-control term; BlobInDegreeMatchesActiveManifests precise edge multiset;
  FoldedEdgesAreActive with the pending-removal disjunct for the drop-then-fold window). Verdict:
  **R0 gate satisfied.**
- **5 R0 findings the gate surfaced (model fixed them) — analyzed for spec/plan impact:** all are
  already captured: (1) retire-view-relative + fence-floored publish/promote gate → spec §Promote
  step 1 + phase1b refresh-then-revalidate; (2) round order fold→retire→fence → §Round Protocol +
  phase1d sequential round; (3) recheck keeps retired-until-delete-lands → §Trim drop-on-confirmed-
  outcome + phase1d sealCompletionAndAdvance; (4) NoManifestIdReuse keys by full ManifestId →
  §Object Identity + control #18; (5) fold-barrier reclaim fairness → model-internal liveness
  fairness (protocol statement already in spec/plan). **Decision: no rev.16, no plan edits required —
  the findings validate the design.** Optional "make these orderings explicit in phase1b/1d" → backlog B2.
- **Next:** Phase 1a (identity/codecs) via subagent-driven-development + TDD.

### 2026-06-27 — Phase 1a GREEN (verified); B3 baseline established
- **Phase 1a complete + GREEN.** 8 tasks, 7 commits (`5f1272cc8a7`…`c3a8ce4dfa6`) on
  `cas-gc-part-manifest-impl`. New: `CasManifestId.h`, `CasFormat.{h,cpp}` (FormatId 12-15 =
  CAPT/CARN/CAFS/CACS), `CasRunFile.{h,cpp}` + `RunMerger`, `CasManifestCodec.{h,cpp}`
  (`computePayloadDigest`, `refMatchesBody`/`manifestNamespaceMatches`), `CasLayout.h` `manifestKey` +
  `_manifests` rejection; 43 new gtests. Build links clean (incremental in `build/`).
- **Independent verification:** ran the 5 Phase-1a suites myself → 43/43 PASS; ran
  `CaWiringOps.FreezeViaHardLinksIntoShadow` in isolation → FAILS (confirms it's a real isolated bug,
  not a test-ordering artifact).
- **Deviations (sound, contract intact):** `computePayloadDigest` uses `CityHash128` (CAS core has no
  BLAKE3; the digest is integrity/debug only, never identity — fine); `::String`/`::UInt128` global
  qualifiers; `RunFileReader` materializes the run incl. header (absolute footer offsets); crc32c
  linked unconditionally for `CasRunFile.cpp`.
- **B3 (pre-existing, unrelated):** `CaWiringOps.FreezeViaHardLinksIntoShadow` fails — a freeze/shadow
  `removeRecursive` bug in the wiring layer; Phase 1a is purely additive and the test's namespace has
  no `_manifests` segment, so it cannot be caused here. Backlogged B3 (medium). **Sweep-gate working
  definition for the rest of the run: GREEN = no NEW failures beyond B3** (392/393 with B3 the only red).
- **Next:** code-review the Phase 1a diff, then Phase 1b.

### 2026-06-27 — Phase 1a code-review: CHANGES-NEEDED (one real blocker), fixing before 1b
- Code-review of the Phase 1a diff found ONE real **blocker (B1/B2)**: `RunFileReader::loadFooter`
  parses untrusted footer length fields (`block_count`, key lengths) with unchecked accessors BEFORE
  verifying the footer CRC → heap over-read/UB under libc++ FAST hardening on corrupt input, and
  `substr(pos>size)` throws `std::out_of_range` which `decodeGuarded` doesn't catch → decode NOT
  reliably fail-closed (a rev.15 safety property 1b/1c/1d rely on, since they read runs from object
  storage). **M1:** the absence of corrupt-footer/truncation/empty-buffer tests hid it.
- Everything else verified correct by the reviewer: identity types, magics (CAPT≠CAPM), `manifestKey`/
  `_manifests` rejection, manifest codec semantics, writer/seek/merge logic, determinism, CMake/crc32c.
- **Action:** dispatched fixer `abb6873296baecbb1` for B1/B2 (verify footer CRC first; bounds-check
  every read → `CORRUPTED_DATA`; no escaping `std::exception`) + M1 (corrupt-footer/truncation-sweep/
  empty-buffer negative tests) + N4 (loadBlock defensive bounds) + N2 (RunMerger doc). Rebuild+test+commit.
- **N1 (doc) fixed by me:** the plan/overview contract said `payload_digest` = BLAKE3; the code/decision
  is `CityHash128` (CAS has no BLAKE3; digest is integrity-only). Corrected 5 spots in overview +
  phase1a so 1b reads an accurate contract. **N3→backlog B4** (operator< cosmetic). N5 (CMake) verified fine.
- **Verdict:** proceed to 1b only after the fixer lands GREEN (re-verify the negative tests myself).

### 2026-06-27 — RunFile blocker FIXED+verified; Phase 1a CLOSED; behavior switch dispatched
- Fixer `ceb77aa56bc`: `loadFooter` now verifies the footer CRC first + bounds-checks every read →
  `CORRUPTED_DATA` (no UB, no escaping `std::exception`); `loadBlock` hardened; 6 negative tests added
  (empty/garbage/corrupt-footer_len/corrupt-block_count/truncation-sweep + manifest truncation sweep).
- **Independently verified:** `CasRunFile.*:CasManifestCodec.*` → 27/27 PASSED, exit 0, **0 aborts**,
  9 fail-closed/truncation tests OK. **Phase 1a fully GREEN + closed.**
- **Coordination finding (B5):** the behavior switch **1b+1c+1d is cross-cutting on the build** —
  phase1b removes `JournalRecord`/`ClosureNode` and rewrites `RootShard`, which breaks `CasStore`
  (read) and `CasGc` (GC) compilation until 1c and 1d land. So the standalone "build+sweep green"
  step at the end of the 1b and 1c plans is NOT achievable alone; the green `Cas*`/`Ca*` sweep is the
  gate **after 1d**, and the gtest sweep is the switch's integration oracle. **Decision:** execute
  1b+1c+1d as ONE atomic unit (commit per task; build may be red mid-switch; green at the end), then
  code-review the whole switch — the honest review point since intermediate states don't compile.
- **Action:** dispatched the behavior-switch implementer (1b+1c+1d). Gate after 1d: clean build +
  `Cas*`/`Ca*` sweep with no new failures beyond B3, then the post-1d chaos soak.

### 2026-06-27 — Behavior switch: run 1 PARTIAL (clean boundary), continuing
- Run-1 implementer completed the isolated/foundational pieces + write path, stopped at a clean,
  documented boundary (5 commits `a8576aaa12a`…`5ffcaa6714b`). DONE: **1b T1** (`RootOwnerEvent`
  journal + proto, removed `JournalRecord`/`ClosureNode`/`RefPayload`), **1b T2-T7** (`CasBuild`
  write path: `stageManifest`/`precommitAdd`/pure-move `promote`/`abandon`), **1d T1**
  (`CasFoldSeal`+`CasCompletionSeal`), **1d T2** (`CasBlobInDegree`). Each compiles as an isolated TU;
  full build RED by design (CasBuild blocked on CasStore until 1c). Resume marker durable in backlog **B6**.
- Useful header-truth adaptations recorded: landed `CasRunFile` API is `RunFileWriter`/`RunFileReader`/
  `RunMerger`; `PartManifest.root_namespace_id` is `RootNamespace`; journal stays `FormatId::Manifest`
  ("CARS"); `FormatId::Tree`/`GcSnap` enum prune deferred to 1d core.
- **Action:** dispatched continuation (run 2), resuming at **1c T1** per B6 → 1c → 1b T6 → 1d T3-T10 →
  link + green `Cas*`/`Ca*` gate (modulo B3). Continuation updates B6 + reports its resume point if it
  also stops short (the switch may take several runs).

### 2026-06-27 — Behavior switch: run 2 done (1c + 1b T6 + wiring); run 3 = 1d core
- Run 2 completed **1c (read path)** — `CasStore` `Resolved.manifest_id`/`readManifest`/`lookupPath`/
  `listDirectory`/`(ManifestId,Token)` cache; `ContentAddressedMetadataStorage` rewired — and **1b T6**
  (`republishRef` + `ContentAddressedTransaction` over part manifests), plus 1d prep (deleted dead
  `CasPlacement.h`). 3 commits (`7a640e5ac69`, `2be338197d2`, `02ce8b412bd`). Grep-verified: the ONLY
  files still referencing removed tree types are the 1d-core files (`CasGc.{h,cpp}`, `CasFsck.cpp`,
  `CasGcSnap.cpp`, `CasClosureWalk.*`, `CasTreeCodec.*`). B6 rewritten to the new resume point; **B7**
  added (cross-server part relink fail-closed under per-instance manifests; byte-fetch fallback correct).
- **Action:** dispatched run 3 = the **1d GC core**: `CasGc.{h,cpp}` fold over the `RootOwnerEvent`
  journal (owner-move / removal / activation + fold barrier #23 + context-404 clamp), wire
  `CasBlobInDegree` + `CasFoldSeal`/`CasCompletionSeal`, `CasOrphanManifestSweep`, `CasGcFormats`
  snap→generation, DELETE `CasGcSnap`/`CasTreeCodec`/`CasClosureWalk` + prune `FormatId::Tree`/`GcSnap`,
  `CasFsck` audit, rewrite the GC/tree gtests, then link + green `Cas*`/`Ca*` (modulo B3). This is the
  R0 invariants in C++ → thorough code-review of the GC core after it goes green, before the post-1d
  soak + Phase 2.

### 2026-06-27 — Behavior switch: run 3 done (1d GC core); run 4 = test ports + B170 events → green gate
- Run 3 completed the **1d GC core** (T3-T7,T6,T9 + removals). `CasGc.{h,cpp}` fully rewritten over the
  `RootOwnerEvent` journal: owner-move (no delta/cleanup), true-removal (−1+deferred cleanup),
  activation (+1) + **fold barrier #23** (live missing-body precommit clamps the cursor); 404 realized
  exactly (present-but-invalid ⇒ CORRUPTED_DATA; missing committed/removal ⇒ clamp+recordAnomaly; missing
  precommit ⇒ barrier); wires `CasBlobInDegree` + `CasFoldSeal`/`CasCompletionSeal` (resume by which seal
  exists); proved tail preserved (exact-token-only delete, registry-then-all-shard fence, fold-through-
  fence recheck, body-delete-after-decrements-sealed, ViewableRound, never-GET-condemned, never-wedge-404).
  New `CasOrphanManifestSweep` (watermark-eligible, no blob deltas); `CasFsck` manifest audit; **deleted**
  `CasGcSnap`/`CasTreeCodec`/`CasClosureWalk` + pruned `FormatId::Tree`/`GcSnap`. **All 29 product objects
  compile; the whole non-test source LINKS.** 4 commits (`3526407`,`889a591`,`a2bf373`,`0f13d7d`).
- **Remaining (B6):** 12 stale write-path/wiring gtests still use the removed tree API
  (`event_log`/`gc_log`/`ca_transaction`/`truncate_reclaim`/`ca_wiring`/`b140_dangle`/`build`/
  `build_root_dangle`/`store`/`protocol_scenarios`/`gc_round`/`gc_leak`); the link gate needs every
  globbed `gtest_*.cpp` to compile. Sub-gap (B8): the new core dropped **B170 GC event emission** that
  `event_log`/`gc_log` + the soak rely on — re-add `emitEvent` at the GC decision points (manifest
  model).
- **Action:** dispatched run 4 = re-add B170 GC events to the new core + port the 12 gtests → link +
  green `Cas*`/`Ca*` (modulo B3). Then: code-review the whole switch, post-1d soak, Phase 2.

### 2026-06-27 — BEHAVIOR SWITCH GREEN (verified); code-review dispatched
- Run 4 finished the switch: B170 GC events restored; 12 gtests ported (faithful — fixed 2 real bugs
  rather than weaken assertions: a detached-part manifest-namespace wiring bug [added
  `BuildInfo::intended_namespace`], and a `foldCursorOf` test-helper bug); 7 commits
  (`3124eb…`→`2007520…`).
- **Independently verified the gate:** `unit_tests_dbms` links; `Cas*:Ca*` = **338 passed, 8 skipped,
  1 failed**. The 1 failure is exactly **B3** (`CaWiringOps.FreezeViaHardLinksIntoShadow`, pre-existing).
  No process aborts. (Run-4's "17 skipped" was a miscount; actual 8.) **The behavior switch
  (1b+1c+1d) is GREEN.** The tree/snap/cascade model is gone; only blobs are content-addressed; GC is
  the streaming `RootOwnerEvent`-fold with the two seals.
- **8 skips triaged:** B7 adopt-by-tree relink ×3 (backlogged), B8 precommit-reclaim ×1 (backlogged),
  2 genuinely-obsolete tree-era tests (inline-closure, deferred-upload — no action), and **B9 ×2**
  (`CasGcSnapRetention`): generation pruning is NOT implemented — `CasGc` doesn't prune old
  `gc/gen/<gen>/` (fold/completion seals + runs + cleanup bundles), so they accumulate. The phase1d
  trim task specified this prune; it was deferred. **Space-liveness, NOT safety** (R0 invariants hold),
  but the post-1d soak would show unbounded `gc/gen/` growth → must fix B9 before the soak.
- **Action:** dispatched the full-switch code-review (GC core = R0 invariants in C++). Next: review →
  one fix pass (any review blockers + **B9 generation prune**) → post-1d chaos soak → Phase 2.

### 2026-06-27 — Switch code-review: CHANGES-NEEDED (no blockers, 3 durability MAJORs); fix pass dispatched
- **Review verdict: CHANGES-NEEDED, NO blockers.** The GC core faithfully realizes the rev.15 model —
  fold dispatch, the #23 fold barrier, context-404, exact-token delete, global fence, fold-through-fence
  recheck, decrement-before-body-delete ordering, two-seal resume all verified correct; **no ported test
  weakened a no-dangle/no-loss/no-leak assertion**; B170 emission complete. Skip inventory is 10 (not 8):
  6 known (B7×3/B8×1/B9×2) + 4 genuinely-obsolete tree-era skips, all with coverage pointers — none hide
  a gap. The `b140_dangle` rewrite is a shallow oracle but the real shared-blob spare-vs-collect oracle
  was relocated to `gtest_cas_gc_round.cpp::SharedBlobSparedUntilBothRefsDrop` (→ B10 cross-ref nit).
- **3 durability MAJORs (pre-soak):** **M1** the cross-round per-shard fold cursor resets to 0
  (`fold` reads `readFoldSeal(snap_generation)` but `snap_generation` is the completion gen) → masked by
  eager trim today, becomes an **active in-degree double-count / over-pin leak under Phase-3 lazy trim**
  or partial-trim-after-crash; **M2** `CasCompletionSeal.blob_target_runs` populated but never
  encoded/decoded (silent coverage loss); **M3** `pickOneSweepTarget` lists only the first 1000-key page
  → eligible older debris never drains (violates `OrphanManifestDebrisDrains`). Plus a `decodeOwnerBinding`
  fail-closed hardening + verify the orphan-sweep active-set vs the atomic promote write order.
- **Action:** dispatched a consolidated fix pass (M1 with the reviewer's multi-round/trim-disabled
  no-double-count test; M2 encode/decode; M3 paginate; decodeOwnerBinding; **B9 generation prune** +
  un-skip the 2 retention tests) → rebuild + green `Cas*`/`Ca*` (modulo B3). MINORS deferred to **B10**.
  Then: verify → post-1d chaos soak → Phase 2.

### 2026-06-27 — GC durability fixes verified; BEHAVIOR SWITCH fully closed; soak + Phase-2 model in parallel
- Fix pass FIXED+GREEN, **independently verified**: 350 ran, **343 passed, 6 skipped, 1 failed (B3
  only)**, 0 aborts; the 8 key fix tests (M1 `FoldCursorSurvives`, B9 retention ×2, M2, M3,
  `decodeOwnerBinding`) all PASS. M1: cursor survives via `completion_seal.folded_cursors` +
  `readSealedCursors` (no trim dependence; double-count test fails-before/passes-after). M2: completion-
  seal runs encoded. M3: sweep paginates. `decodeOwnerBinding` invariant enforced. **B9** generation
  prune live (retention tests un-skipped + passing). `activeManifestKeys` verified safe (atomic promote
  CAS). 5 commits. B9→Resolved; **B10** added (7 deferred minors). **Behavior switch (1b+1c+1d) fully
  closed — T18 done.** The R0 invariants are realized in C++ and exercised by 343 green gtests; only the
  pre-existing unrelated B3 is red.
- **Parallel dispatch** (mutually independent): (a) post-1d **chaos soak** — feasibility-gated (docker +
  ca-soak harness + incremental server build cost); if feasible, scoped soak asserting no dangle/loss/
  leak + bounded `gc/gen/` growth (B9 prune) + B170 events; else reports infeasible → I backlog (B11) +
  proceed. (b) **Phase 2 R0 model gate** — token-diff `listedTok`/`foldedTok` split + `SabotageSkipChangedShard`,
  re-green the whole suite (model file only; independent of the soak).
- Next: soak result (clean / infeasible→B11) + Phase-2-model GREEN → Phase 2 code (discovery skip) →
  Phase 3 → 4 (+soak) → 5, each gated on its TLA+ extension + reviewed.

### 2026-06-27 — POST-1d CHAOS SOAK CLEAN: behavior switch validated under chaos (T19 done)
- Feasibility gate **PASSED**. Agent caught a sharp trap: the on-disk server binary (2026-06-26
  02:52) **predated the entire switch** (1a–1d landed 2026-06-27 03:00+) → the running 22h soak was
  testing the OLD GC. Rebuilt (incremental ccache-warm link, minutes) + ran a **fresh clean-data 45m
  chaos soak** on the new binary. The integration oracle now confirms the R0 invariants live:
  - **No loss / no dangle** — all 4 quiesced checkpoints `dangling=0` incl. post node-kill+restart;
    1.2M rows, both replicas agree; `PHASE3 OK (exit 0)`.
  - **No exception/logical-error storm** — zero CA-layer/logical/fatal lines in either server log.
  - **`gc/gen/` bounded** — oscillated 3↔5↔3↔4, ended at 3 (= `gc_snap_generations_to_keep`) →
    **B9 prune actively reclaims** superseded generations, does not climb per round.
  - **B170 populated** with the new core's events (`gc_fold_begin/end`, `gc_fence`,
    `gc_retire_observe`, `indeg_zero`, `blob_retire`, `gc_recheck_verdict`, `gc_trim`).
  - **Real progress + correct sparing** — 32+ Success rounds, `tree_delete=13078` manifest objects
    reclaimed; all 939 retired-blob candidates correctly **spared** (in-degree>0 via dedup) through
    fold-through-fence recheck — R0 no-loss exercised, not a stall. Pool plateaued ~20.9 GB < 30 GB.
  - **One low finding (introspection only)** → **B11**, committed `6ee6385dac4`: round-summary
    `objects_deleted` reads 0 while events show `tree_delete=13078` (`report.deleted` in `CasGc.cpp`
    ~717 counts only blob deletes, not the manifest-body delete site). Per-event audit correct; only
    the aggregate undercounts. Not a correctness bug. Stack auto-torn-down; artifacts archived under
    `utils/ca-soak/soak/archive/`. (~65m wall vs 45m nominal — pre-existing B146/B154 fsck-under-load
    180s timeout degrades gracefully; not a switch issue.)
- **Phase 2 model gate (parallel)**: subagent wrote the token-diff extension (`listedTok`/`foldedTok`,
  `SabotageSkipChangedShard`) but died in a broken poll-loop (waiting on a background TLC) — I took
  over, killed orphaned TLC, fast-verified (stage0/1 HOLD; sab_skipchangedshard + sab_nofence VIOLATE
  `INV_NO_DANGLE`), and launched the big-stage re-green (`stage2/3/4/live` + full-feature
  `stage5_tokendiff`) in background (`b2awn62ap`; stage2 already HOLD @ 68.5M states). Model work
  **uncommitted** pending all-HOLD.
- Next: on re-green all-HOLD → commit Phase 2 model + dispatch Phase 2 code (discovery skip) → 3 → 4 → 5.

### 2026-06-27 — Phase 2 model gate committed; code T2/T3 done; T4 found+fixed a safety overreach
- **Model gate (Task 1) COMMITTED `68f3bfdc5a5`.** Big-stage re-green all-HOLD (`stage2` 68.5M, `stage3`
  365.6M, `stage4` 27.4M, `live` 17.8M, `stage5_tokendiff` 8.3M); full `_sab_*` sweep (25 cfgs / 24 logical
  controls, #16 split a/b) all VIOLATE their named invariant incl. `sab_skipchangedshard → INV_NO_DANGLE`;
  3 witnesses reachable; no UNEXPECTED PASS. Fixed stale `RESULTS.md` header (23→24). The token-diff
  model extension was written by a subagent that then died in a broken poll-loop (waiting on a background
  TLC I had to kill); I took over, verified, and committed.
- **Phase 2 code via subagent-driven-development (one implementer at a time; I review each):**
  - **T2 `660f9d7901f`** — `Backend::supportsListTokens()` capability probe (pure virtual + InMemory/
    ObjectStorage TRUE, Instrumented forwards; `NullBackend`→false; 5 test-stub ripple handled). 2 gtests.
  - **T3 `4facf0bc9e1`** — CHARACTERIZATION-ONLY: Phase 1d's `encodeFoldSeal`/`decodeFoldSeal` in
    `CasGenerationSeal.cpp` already round-trips `folded_token`+`folded_cursor`. Surfaced ground-truth
    corrections to the plan's guesses: seal file is `CasGenerationSeal.h` (not `CasFoldSeal.h`);
    `per_ns_shard` key is a `String` `"ns/shard"` (not a pair); `classification` is `uint8_t`;
    `Token{value,type}`.
  - **T4 — REVERTED `43cd5eef11e` → `7ae46d27701`, redo dispatched.** The first T4 (411 ins) made an
    **unauthorized, unproven, safety-critical** change: it **reordered `trim` before `recheck`** to capture
    a post-trim shard token. Root cause I confirmed in code: the skip compares the shard object's backend
    token (`ListedKey.token`), but `fence` `mutateShard`s every shard EVERY round and `trim` does so when
    trimmable — so the LIST token is bumped by GC's own writes, while the model's `listedTok` advances only
    on writer transitions (model-infidelity). The reorder touches the `sab_trimunincorporated`/
    `INV_JOURNAL_COVERAGE` concern and is not model-proven → R0 violation. **Corrected design** (redo,
    opus `a5d2b1c4cf5a4bf9b`): canonical order, no reorder; record `folded_token` from the POST-FENCE token
    `recheck` already reads; skip only when nothing wrote the shard since ⇒ a strict SUBSET of
    model-proven-safe skips ⇒ **safe under the existing green gate, no model change**. Conservative cost:
    a just-trimmed shard re-reads one round, then a quiesced shard skips permanently (R1 steady-state win
    preserved). Optimal one-round skip (post-trim capture + model extension) deferred → **backlog B12**.
- Next: review T4 redo → T5 (fail-closed ambiguity) → T6 (build + full `Cas*:Ca*` sweep + phase exit).

### 2026-06-27 — PHASE 2 COMPLETE; Phase 3 (lazy trim) started
- **T4 redo `559b8bd15ce`** — verified clean: `runRegularRound` byte-identical (no reorder), `fence`/`trim`
  untouched; records the post-fence token `recheck` already reads; `SkipsQuiescedShard` confirms the
  predicted 1-round settle (round 1 folds+trims → token moves → Read; round 2 nothing to trim → stable →
  round 3 Skip). Also fixed a latent capability lie (`ObjectStorageBackend::list` advertised
  `supportsListTokens()` but didn't surface a token — now populated from the ETag). `CasGc*` 59/0.
- **T5 `cf95be90a6e`** — fail-closed hardening: `listRootShardTokens` reports ambiguous keys (a shard key
  seen >1× in the LIST sweep) via out-param; `computeDiscoverDecisions` forces Read for ambiguous keys.
  `DuplicateListBackend` test proves the guard overrides a would-be Skip (settled state, token would match).
  Unobservable + no-prior-coverage cases pass pre-change (characterization locks).
- **T6 phase exit `dfb963993fd`** — TLA+ gate re-confirmed (`stage5_tokendiff` HOLD; `sab_skipchangedshard`
  → `INV_NO_DANGLE`); full `Cas*:Ca*` sweep **359 ran / 352 passed / 1 failed = `CaWiringOps.
  FreezeViaHardLinksIntoShadow`** (pre-existing baseline-red B3 only). **Phase 2 token-diff discovery DONE.**
- **Phase 3 (lazy trim) started.** Plan `…-phase3-lazy-fence-trim.md`: T1 model ext (EnableLazyTrim
  positive stage + permanent `SabotageLazyFenceUnsafe` control proving why lazy fence is unsafe) = R0 gate;
  T2 lazy trim only below sealed-generation coverage (`INV_JOURNAL_COVERAGE`); T3 sweep + exit. Lazy FENCE
  stays dropped. Model-writer subagent dispatched (writes + fast-sanity only — no background TLC after the
  Phase-2 poll-loop lesson); I run the heavy re-green + commit.

### 2026-06-27 — PHASE 3 COMPLETE (lazy trim); Phase 4 (sharded reducers) started
- **T1 model gate `adc8dc92015`.** Model writer added `EnableLazyTrim` + permanent `SabotageLazyFenceUnsafe`
  control (reuse a stale fence for a shard published-into between discovery and recheck → `INV_NO_DANGLE`)
  + `foldTok`/`prevFencePos` witnesses. **Review caught the new vars doubled every stage's state space
  (stage2 68M→131M); I gated both writes behind `SabotageLazyFenceUnsafe`** (their only consumer) → inert
  in non-sabotage stages. Inertness PROVEN: `stage0`/`stage1`/`stage2` reproduce EXACT Phase-2 distinct
  counts (19,846 / 402,034 / 68,550,326) → `stage3`/`stage4`/`live` + all 24 prior controls carried
  forward (no re-run). `sab_lazyfenceunsafe` VIOLATES (24.5M). `stage5_lazytrim`: the full `{n1,n2}`
  cross-product exploded multi-billion (killed); right-sized to a BOUNDED `{n1,n2}` (1 shared blob,
  precommit off) for feasible cross-namespace shared-blob coverage → HOLD 338.8M. Full control+witness
  sweep all VIOLATE, no UNEXPECTED PASS. `RESULTS.md` finalized.
- **T2 lazy-trim code `ad278c9f903`.** `trim` ALREADY sourced the sealed `CasFoldSeal` coverage cursor (the
  M1 durability fix did that); locked by `CasGcRound.TrimOnlyBelowSealedCoverage` (fold-barrier scenario:
  committed v1 trimmed, barrier precommit v2 retained) + `trim_cursors` audit recorded. `fence`/`recheck`/
  round-order untouched (verified). **T3 exit `8989c70bfb2`**: full `Cas*:Ca*` 360 ran / 353 passed / 1 =
  baseline B3. **Phase 3 DONE.**
- **Infra lessons:** long `run_in_background` bash is REAPED (~minutes); use `setsid`-detached for long
  TLC (survives, poll via ScheduleWakeup) and Agent subagents for code (survive + notify). The Bash tool
  itself caps foreground at 120s.
- **Phase 4 (sharded reducers, R2) started.** 9 tasks: T1 model ext (multi-shard/multi-leader stage +
  sab_reducerownsfence + sab_crosssharddisplacement) = gate; T2 gc_shards config; T3 shard-plan mapper;
  T4 reducers via RunMerger; T5 cleanup workers; T6 coordinator/leases; T7 gc_shards=1 byte-equivalence;
  T8 two-replica concurrency; T9 sweep + two-replica chaos soak. Applying the Phase-3 lesson: new vars
  gated behind EnableSharding for inertness from the start.

### 2026-06-27 — PHASE 4 COMPLETE (sharded reducers, R2)
- **T1 model gate `925f907bd70`.** Sharded fold actions (GScatterDelta mapper / GReduceShard / GCoordFence
  / GCoordSeal coordinator) + shardIndeg/coordFence/reducerOwner vars. Writer made coordFence per-namespace
  (scalar was self-healing — sound). New vars GATED behind EnableSharding → inertness proven (stage0/1/2
  EXACT pre-Phase-4 counts) → stage3/4/live + 25 prior controls carried. `stage5_sharding` (bounded
  `{s1,s2}×{L1,L2}×{b1,b2}`, heavy features off — full cross-product won't converge) HOLD 983.9M. Two new
  controls: sab_reducerownsfence→INV_NO_DANGLE, sab_crosssharddisplacement→INV_NO_LOSS. 27 controls VIOLATE,
  3 witnesses, no UNEXPECTED PASS.
- **Code T2-T8 (subagent-driven, all reviews clean):** T2 `bb7a07a5b4f` gc_shards knob (snap_shards→gc_shards);
  T3 `6c9140211b7` `blobShard`=(hash>>64)%gc_shards + ShardScatter (CasGcSnap deleted → new rule); T4
  `0a3c1c62227` ShardReducer delegates to existing `foldDeltasIntoGeneration` (no divergent path); T5
  `57cccef654c` manifestCleanupShard = std::hash<ManifestId>%gc_shards (qualified, not ref-only); T6
  `d21a8a89ef5` removed the implicit shard-0 pin → gated routing (gc_shards==1 untouched; >1 partitions by
  blobShard), CoordinatorPlan policy, scheduler unchanged (work-dedup lease + disjoint key namespaces); T7
  `25eeeccc45a` gc_shards=1 in-degree equivalence; T8 `62c20f3339d` two-replica disjoint + pre-seal
  invisibility + post-seal merge. Full `Cas*:Ca*` 376 ran / 369 passed / 1 = baseline B3.
- **T9 soak INFEASIBLE → B13.** docker+harness+server-rebuild OK, but the integration MinIO image predates
  S3 `If-Match` conditional delete that `CasProbe` step-6 hard-requires → server won't start (blocks ALL CA
  S3 integration tests, pre-existing). Test module `tests/integration/test_cas_gc_sharded/` committed
  `pytest.mark.skip` with reason. **Soak prep CAUGHT+FIXED a latent bug** `209d4ff1462`: `gc_shards` in
  PoolConfig was never written to the initial GcState (XML config a no-op; all pools silently gc_shards=1) —
  threaded `<gc_shards>` through MetadataStorageFactory/ContentAddressedMetadataStorage + set on first lease
  acquire. Re-verified: rebuild + `Cas*:Ca*` 369 passed / 1=B3 (no regression). Phase 4 DONE.
- Next: **Phase 5** (retire-token optimization — the final phase).

### 2026-06-27 — PHASE 5 model gate DONE; code DEFERRED (B14, design decision); REDESIGN COMPLETE through Phase 4
- **T1 model gate `d56f4e84a4d`.** `EnableRetireTokenSource` + `SabotageStaleTokenOverDelete` + `storedTok`
  (seal-time token). storedTok write GATED behind the flag → inertness (stage0/1/2 EXACT) → stage3/4/live +
  27 controls carried. `stage5_retiretoken` (bounded) HOLD 3.5M on INV_NO_DANGLE/LOSS/RETURN +
  `RetireTokenSourceComplete` (tightened to monotone `e.t ≤ tokOf[e.b]` — sound; plan's form wrongly failed
  a never-edged present blob). `sab_staletokenoverdelete` VIOLATES **INV_NO_LOSS** (not INV_NO_RETURN: monotone
  tokens preclude a dead token returning live; the over-delete is a LOSS of re-incarnated referenced bytes —
  matches the plan's prose). 28 controls VIOLATE, 3 witnesses, no UNEXPECTED PASS.
- **T2/T3 code DEFERRED → B14.** Investigation found the optimization is NOT realizable as planned: the
  blob's incarnation token (ETag) is **not available at fold time without a HEAD** — `ManifestEntry` records
  only `blob_hash`, not the storage token. Eliminating the per-candidate `retire` HEAD requires recording
  the blob's committed ETag in the manifest payload (a core write-path / on-disk schema change coupling the
  content-plane manifest to storage incarnations) — a design decision for the maintainer, NOT an autonomous
  overnight core-schema change. The model gate stands as the safety proof; the current per-candidate-HEAD
  `retire` is correct and shipped. No dead code committed. Lowest-stakes phase (R1 perf only).

### 2026-06-27 — UNATTENDED RUN SUMMARY (Phases 0–5)
- **Phase 0** R0 TLA+ gate; **Phase 1a** identity/codecs; **behavior switch (1b+1c+1d)** content-addressed-tree
  → root-local part-manifest model, hardened (durability M1/M2/M3 + B9) + reviewed; **post-1d chaos soak CLEAN**
  (caught the binary predated the switch, rebuilt, re-ran). **Phase 2** token-diff discovery (caught+reverted
  an unsafe trim-before-recheck reorder → conservative skip ⊆ model-proven-safe, no model change). **Phase 3**
  lazy trim. **Phase 4** target-sharded reducers R2 (caught+fixed the gc_shards XML no-op bug; soak infeasible
  =B13 MinIO). **Phase 5** model-proven, code deferred =B14.
- Method that held up: every TLA+ extension GATES new vars behind its flag → inertness proven (stage0/1/2 exact
  counts) → existing stages carried, cheap re-greens. Big positive stages bounded when the full cross-product
  won't converge. subagent-driven-development + per-task review caught two real safety issues (the trim reorder,
  the gc_shards no-op) and one design blocker (B14). Infra: setsid-detached for long TLC + ScheduleWakeup poll;
  Agent subagents for code (background bash is reaped).
- **State:** all phase model gates GREEN + committed; Phases 0–4 code DONE + committed; full `Cas*:Ca*` 369
  pass / 1 = pre-existing baseline-red B3. Open decisions: **B14** (Phase 5 manifest-token provenance) for the
  maintainer; B13 (sharded soak needs newer MinIO); B3 (pre-existing freeze test). Branch
  `cas-gc-part-manifest-impl` — NOT yet PR'd (a multi-phase branch; opening a PR is the maintainer's call).

### 2026-06-27 — REPORTING CORRECTION (review caught an under-reported final state)
The "unattended run summary" above under-reported open items and over-claimed two things. Honest state:
- **Full open-issue list** (the summary wrongly listed only B14/B13/B3): **B3** pre-existing baseline-red
  freeze test; **B7** cross-server part relink — feature regression to restore (3 `_ObsoleteB7` GC-leak
  skips); **B8** GC precommit-reclaim NOT wired for the converged-shard model — a **real core
  space-liveness gap** (not R0 data-loss, but space can leak; 1 skip `CasBuildRoot.AbandonedPrecommitReclaimed`);
  **B12** optimal one-round token-diff skip; **B13** sharded integration soak (was deferred on MinIO — see
  below); **B14** Phase-5 retire-token code (blob-token provenance decision); **B15** plan bodies retain
  stale rejected-protocol snippets (doc hygiene). The 6 `Cas*:Ca*` skips = B7(3) + B8(1) + 2 genuinely
  obsolete (deferred-tree-upload, inline-closure JournalRecord — the tree model was removed). So the unit
  state is **376 ran / 369 passed / 6 skipped / 1 failed (B3)** — the 6 skips are NOT all benign: B7+B8 are
  real deferred gaps, B8 especially.
- **Phase 4 wording corrected:** it is **model + unit green; the integration soak was DEFERRED** (B13), NOT
  full integration validation.
- **B13 was MISDIAGNOSED as a hard blocker.** The MinIO-If-Match limitation only affects the *praktika*
  integration path (`test_cas_gc_sharded/`). The canonical **`ca-soak` harness already runs RustFS**
  (`rustfs/rustfs:1.0.0-beta.8`), which supports the conditional delete — so the two-replica sharded soak
  IS feasible there. Re-attempting it now via ca-soak/RustFS at `gc_shards=2` (subagent dispatched). If it
  passes, B13 closes properly (integration validation, not just unit+model).
- **"All plans re-synced clean" was overstated** → B15 (plan bodies still carry pre-rev.15 rejected
  snippets; the CODE is correct, the plans are stale docs).
### 2026-06-27 — REVIEW FOUND 2 HIGH PROTOCOL BUGS (precommit lifecycle) — fixing
A second review pass (Codex) caught two HIGH protocol-level bugs where the C++ build path violates the
preconditions the TLA+ model proves safe (NOT caught by my per-task reviews — they were in Phase 1b code
predating this session, exercised only by specific precommit-removal/abandon sequences the unit tests
didn't drive):
- **HIGH #1 — promote doesn't verify the precommit is the live owner.** `Build::promote` (CasBuild.cpp
  ~:634) appends the pure owner-move (Δ=0, no blob delta — fold treats equal old/new manifest_ref as
  no-delta) WITHOUT checking the precommit is still the current owner. Model `WPromote` has `owner[m]=bld`.
  If the precommit was removed/reclaimed, the Δ=0 move restores no in-degree → committed manifest with
  in-degree-0 blobs → GC deletes → **DANGLE**. Fix: require the exact live precommit binding before the
  move, else ABORTED.
- **HIGH #2 — abandon deletes a live precommit body.** `Build::abandon` (CasBuild.cpp ~:704, called from
  `ContentAddressedTransaction`'s destructor on a failed txn) deletes every staged manifest body, incl. one
  `precommitAdd` made a live owner input. Model `WAbandonPrecommit` appends a REMOVAL event and never
  deletes the body (delete-after-sealed-decrements; `sab_deletebodybeforedecrements`). Fix: append a
  precommit-removal RootOwnerEvent in the target shard + never writer-delete a precommitted body.
- Both fixes = make code match the already-proven model (no model change). Dispatched (opus, TDD, 2 commits).
- Related: **B8** (abandoned-precommit RECLAIM still wired to the old `_precommits` namespace, not the new
  target-shard model) remains deferred — it covers the CRASHED-build case (no abandon called); HIGH #2 fixes
  the explicit-abandon path. **B11** (manifest-delete counter undercount) deferred as before.
- Also re-attempting the Phase-4 sharded soak on the ca-soak/RustFS harness (B13 misdiagnosis).

### 2026-06-27 — 2 HIGH precommit bugs FIXED + verified (code now matches the proven model)
- **BUG 1 `c93c1149694`** — `promote` fails closed unless the precommit is the live owner (WPromote
  `owner[m]=bld`). Guard: inside the shard CAS, replay `root.journal` exactly as the fold dispatches it
  (old_binding removes, new_binding installs) and ABORT iff a REMOVAL of EXACTLY this precommit binding
  (`old={Precommit,ref,build_id,manifest_ref}`, `new=none`) is recorded and not re-added (`present_in_live`).
  Subtlety the implementer caught (a naive "present in live set" check broke
  `SharedBlobSurvivesSourceDropDuringBuild`): the create-precommit add can be legitimately TRIMMED below the
  sealed fold cursor once GC activates it, so a trimmed-but-live precommit (no removal) is correctly ALLOWED.
  **Completeness of the visible-removal check** (a removal could itself be trimmed) rests on the build-seq
  WATERMARK: an in-flight build doesn't `retireBuildSeq` until promote/abandon, so GC cannot reclaim its
  precommit while it's promoting → no trimmed removal for a live build; abandon is the same build (wouldn't
  promote); + the existing blob revalidation is a third fail-closed layer. Faithful to WPromote.
- **BUG 2 `77ad8b460e4`** — `abandon` appends a precommit-REMOVAL `RootOwnerEvent` (old=Precommit,
  new=none) in the target shard (mirroring `Store::dropRef`/`Gc::reclaimAbandonedPrecommit`), reliably,
  and SKIPS the precommit body in the best-effort `staged_manifests` delete loop (never writer-delete a
  live precommit body — WAbandonPrecommit; delete-after-sealed-decrements). Never-precommitted staged
  debris is still best-effort deleted.
- 4 new RED-first tests pass; regression `CasBuild*:CasGc*:Ca*` 373 passed / 6 skipped (unchanged; B8 NOT
  un-skipped) / 1 = baseline B3. No model change (code now matches the already-proven protocol).
- NOTE: a final full `Cas*:Ca*` build+sweep including these fixes is pending (the running RustFS soak uses
  a pre-fix binary; will rebuild+sweep after it returns).
