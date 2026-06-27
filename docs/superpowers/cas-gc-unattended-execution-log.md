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
