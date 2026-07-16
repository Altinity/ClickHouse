# CAS Source Layout Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` into a readable, layered, self-describing tree — merge tightly-coupled micro-files, move the flat `Core/` into layered subdirectories, decompose the `CasStore`/`CasBuild` god objects along state/lock boundaries, and rename `Store`→`Pool` / `Build`→`PartWriteTxn` — with **zero behavior change**.

**Architecture:** Five phases, in strict order: (1) eight in-place file merges, (2) `git mv` into the target tree + include-path sweep + CMake update, (3) `CasStore` decomposition into four components, (4) `CasBuild` blob-uploader extraction, (5) renames. Phases 1–2 are script-driven (mechanical moves/merges). Phases 3–5 are hand surgery under a strict "no logic changes" invariant. Every phase is independently buildable, green under the full cas-gtest battery, and one-commit-per-step.

**Tech Stack:** C++ (ClickHouse `src/`), CMake `add_headers_and_sources` glob discovery, GoogleTest (`unit_tests_dbms`), `git mv`, `git grep`/`sed` scripted sweeps, praktika (final CA-default stateless + ca-soak gate).

**Authoritative design:** `docs/superpowers/specs/2026-07-15-cas-source-layout-refactoring-design.md` (committed `416c982b5d6`). Do NOT redesign — this plan executes it. The spec's merge table (§Merges) and decomposition method-groups (§Decomposition) are authoritative; where this plan enumerates members, the spec text wins on any conflict.

## Global Constraints

- **Branch:** `cas-gc-rebuild`. No rebase, no amend, no force-push, no push. One new commit per step (project rule: add commits, never rewrite).
- **Do NOT commit to `master`.** All work stays on `cas-gc-rebuild`.
- **Zero behavior change (operational definition, binding every step — spec §Invariants):**
  1. No mutex changes its covered state; no lock-acquisition order changes.
  2. Construction/destruction order preserved verbatim (the ordered `~Store` teardown: drain lanes → keeper terminate → … must be reproduced exactly when members move into components).
  3. No changes to persisted bytes, object keys, log/error/event texts, ProfileEvents, or metric names.
  4. Move commits (`git mv`) contain **no** content edits; content commits contain **no** moves.
  5. No drive-by improvements — anything worth improving goes to `docs/superpowers/cas/BACKLOG.md`, never into these diffs.
- **Rename boundary rule (spec §Renames):** only C++ identifiers and file names change. Persisted bytes, key layouts, log/error/event texts, ProfileEvents/metric names, and protocol-spec vocabulary do NOT change. The protocol term "build" (`build_seq`, `buildSeq`, `BuildPrefix`, upload stamps) SURVIVES — it is watermark-spec/durable-context vocabulary, not a class name.
- **Include direction rule (README-only, no CI):** `Primitives → Formats → Backend → Pool → Gc → Tools ≈ Parts → facade`. A file includes only its own layer and layers to its left. Documented exceptions: staging sweeper + `probeConditionalCopy` bypass `Backend` into `IObjectStorage`; `Backend` may read `Formats` traits.
- **`Cas` file-name prefix stays tree-wide** (grep-ability); namespace `DB::Cas` and the `ContentAddressed*`/`Part*` wiring convention are untouched until phase 5 renames (which touch only `Store`/`Build`).
- **Build discipline (project rules):** run `ninja` with no `-j`/`nproc`; redirect build output to `<build_dir>/build_srclayout.log`; use a subagent to analyze each build/test log and return only a concise summary. Redirect each gtest run to a uniquely-named log under the build dir.

---

## Shared Procedures

These three procedures are invoked by name from the task steps below. Every task that says "run the **Merge Gate**" / "**Move Gate**" / "**Decomp Gate**" means exactly the commands here.

### SP-1: Build + Battery Gate

The gate after **every** commit. Build target and gtest filter are fixed.

```bash
# From repo root. Build dir = build (RelWithDebInfo). Never pass -j.
cd /home/mfilimonov/workspace/ClickHouse/master
ninja -C build unit_tests_dbms > build/build_srclayout.log 2>&1; echo "NINJA_EXIT=$?" >> build/build_srclayout.log
```

- Analyze `build/build_srclayout.log` with a **subagent** (return only pass/fail + first error). Expected: `NINJA_EXIT=0`.

```bash
# Run the full CAS/CA gtest battery. Unique log name per gate invocation (replace <tag>).
./build/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > build/gtest_cas_<tag>.log 2>&1; echo "GTEST_EXIT=$?" >> build/gtest_cas_<tag>.log
```

- Analyze `build/gtest_cas_<tag>.log` with a **subagent**. Expected: `GTEST_EXIT=0`, `[  PASSED  ]` count equal to the pre-refactor baseline captured in Task 0 (no test lost). Zero `[  FAILED  ]`.
- The battery is the entire test cycle for this refactor — there are **no new tests**; the guarantee is "the same tests still pass".

### SP-2: File-Count / No-Loss Verification

Run before and after each file-changing commit.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
# Tracked file count under the CA tree (the number the task's expected delta applies to):
git ls-files 'src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/**' | wc -l
```

- **Merges:** count must drop by exactly the per-merge delta stated in the task.
- **Moves (`git mv`):** count must be **unchanged**; additionally confirm history is preserved on a moved file:
  ```bash
  git log --follow --oneline -- <new/path/to/moved/file> | head
  ```
  Expected: history predating the move is visible (rename detected).
- No file may appear in `git status` as both deleted-and-untracked (that means a non-`git-mv` move lost history — abort and redo with `git mv`).

### SP-3: Global Include-Path Sweep

Include paths are root-relative angle-bracket (`<Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/...>`), so one sweep fixes the CA tree, `src/Disks/tests/`, `programs/disks/`, `src/Storages/System/`, `src/Interpreters/`, and `MetadataStorageFactory.cpp` uniformly.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
OLD='Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/<OLD_SUBPATH>'
NEW='Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/<NEW_SUBPATH>'
git grep -l -- "$OLD" -- 'src' 'programs' | xargs -r sed -i "s#${OLD}#${NEW}#g"
```

- After every sweep, confirm no stale reference remains anywhere (code, docs, scripts, `.claude/`):
  ```bash
  git grep -n -- '<OLD_SUBPATH>' -- 'src' 'programs' 'docs' 'utils' '.claude' || echo "clean"
  ```
- `#pragma once` (all 60 headers) makes any duplicate include lines produced by a many→one merge sweep harmless. Deduping identical include lines is cosmetic and optional; it is NOT required for green and must never be bundled into a `git mv` commit.

---

## Task 0: Precondition Gate + Baseline Re-Verification

**Files:**
- Read-only inventory; produces the authoritative file map consumed by all later tasks. Writes nothing except an optional scratch note under `tmp/`.

**Interfaces:**
- Produces: the confirmed post-v3 file inventory (exact surviving names), the baseline `[  PASSED  ]` count, and the resolved placement of drift files (`CasSourceEdgeMarkers.h`, `CasPoolMeta.{h,cpp}`, `CasWireVocab`/`CasRefWireVocab`). Every later task's `git mv`/merge inputs are validated against this map.

> **WHY THIS TASK EXISTS (flag to reviewer):** This plan was written against the PRE-v3 tree (2026-07-16). The spec's precondition (`§Precondition: Codecs V3 Is Landed`) is **not yet met** — the current tree still contains `Core/CasCodecUtil.h`, `Core/Formats/CasWireVocab.{h,cpp}`, `Core/Formats/CasRefWireVocab.{h,cpp}`, `Core/CasSourceEdgeMarkers.h`, and lacks a `CasPoolMeta.h`. Codecs v3 (steps 1–8) is in flight in parallel. **Execution of this plan MUST NOT start until v3 is landed and green.** This task is the hard gate.

- [ ] **Step 1: Verify codecs v3 is landed**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
# v3 removes these; their absence is the landed signal:
git ls-files | grep -E 'ContentAddressed/Core/CasCodecUtil.h|Core/Proto|Formats/CasWireVocab|Formats/CasRefWireVocab' || echo "V3-LANDED-CANDIDATE"
# v3 creates the per-object Formats registry README and the CasPoolMeta logic/format split:
git ls-files | grep -E 'ContentAddressed/Formats/README.md|ContentAddressed/.*CasPoolMeta\.(h|cpp)'
```
Expected once v3 is landed: the first command prints `V3-LANDED-CANDIDATE`; the second shows the Formats README and a `CasPoolMeta.{h,cpp}` pair. **If v3 is not landed, STOP** and report to the team lead — do not proceed.

- [ ] **Step 2: Snapshot the authoritative post-v3 inventory**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git ls-files 'src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/**' | sort | tee tmp/cas_baseline_inventory.txt | wc -l
```
Record the file count and the full list. This is the baseline for SP-2.

- [ ] **Step 3: Capture the green gtest baseline**

Run SP-1 with `<tag>=baseline`. Record the `[  PASSED  ]` count from `build/gtest_cas_baseline.log`. Every later gate must match this count. Expected: `GTEST_EXIT=0`.

- [ ] **Step 4: Resolve drift-file placement against the spec target layout**

For each file present in the post-v3 tree but NOT explicitly named in the spec's Target Layout, decide its layer (record the decision in `tmp/cas_baseline_inventory.txt`):
  - `CasSourceEdgeMarkers.h` (a dependency-free POD header consumed by `CasRecordStreamFormat.h` and `CasBlobInDegree.h`) → **`Primitives/`** (zero outward deps; matches the Primitives layer definition). Confirm it still exists and its consumers before phase 2.
  - `CasPoolMeta.{h,cpp}` (post-v3 logic half) → **`Pool/`** per spec; confirm v3 produced a `.h` (pre-v3 had only `.cpp`).
  - `CasWireVocab`/`CasRefWireVocab` → confirm v3 either removed them or folded them into `Formats/`; if any survive, they belong in **`Formats/`** (persisted vocabulary). Record their exact surviving names — they feed the phase-2 move map.
  - `CasInspect.{h,cpp}` → per spec it is "gutted to a thin decompress-print or deleted" by v3; if it survives, it moves to **`Tools/`**. Record its fate.

- [ ] **Step 5: Confirm no in-flight conflicting work on the CA tree**

Run:
```bash
git log --oneline -15 -- 'src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed'
git status --short 'src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed'
```
Expected: a quiet point right after v3 landed, working tree clean under the CA path. If dirty, STOP and report.

- [ ] **Step 6: No commit** — this task produces only the `tmp/` scratch inventory. Proceed to Phase 1.

---

## Phase 1 — The Eight Merges (in place)

**Rule (spec §Merges):** merge *files*, not *classes* — type names and APIs do not change here (renames are phase 5). Composition order inside each merged file: types/enums on top, then codecs, then logic (reads top-down). Every merge stays within one layer; merges happen **at the files' current locations** (`Core/` or top-level) — moves to the target tree are entirely phase 2. Each merge is one content commit; no file moves in a merge commit.

**Merge procedure (applies to every Task 1.x):**
1. Pick the **result file** (an existing input keeps its identity; a brand-new result file is created with `git mv` from the largest input, then others appended — so the result carries history).
2. Append the other inputs' content into the result file in the order: types/enums → codecs → logic, stripping their `#pragma once` and now-internal includes.
3. `git rm` the absorbed input files.
4. Run **SP-3** mapping each absorbed header path → the result header path (consumers repoint; `#pragma once` dedupes).
5. Run **SP-2** (expect the stated negative delta) and the **Merge Gate** (SP-1, `<tag>=merge<N>`).
6. Commit.

Order chosen to minimize churn: smallest/leaf merges first (4, 5, 6, 1), then the medium (2, 3), then the two that also re-order internals (7, 8). No merge depends on another, so any green order is valid; this order surfaces breakage on the cheapest diffs first.

### Task 1.1: Merge #4 — `CasBackend.h` absorbs `CasBackendListing.h`

**Files:**
- Modify/result: `.../ContentAddressed/Core/CasBackend.h`
- Remove: `.../ContentAddressed/Core/CasBackendListing.h`
- Sweep consumers of `Core/CasBackendListing.h` (e.g. `src/Disks/tests/gtest_cas_backend_listing.cpp`).

**Interfaces:** Consumes nothing from prior tasks. Produces: `CasBackendListing.h` no longer exists; its listing-helper declarations live in `CasBackend.h` unchanged.

- [ ] **Step 1:** Append `CasBackendListing.h` body into `CasBackend.h` (seam helpers at the seam), drop its `#pragma once`/redundant includes, then `git rm .../Core/CasBackendListing.h`.
- [ ] **Step 2:** SP-3 with `OLD_SUBPATH=Core/CasBackendListing.h`, `NEW_SUBPATH=Core/CasBackend.h`.
- [ ] **Step 3:** SP-2 — expect delta **−1**. Merge Gate SP-1 `<tag>=merge4`. Expected `NINJA_EXIT=0`, `GTEST_EXIT=0`, PASSED == baseline.
- [ ] **Step 4: Commit**
```bash
git add -A && git commit -m "cas(refactor): merge CasBackendListing into CasBackend (files, not classes)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PeC1dg2dXBh55xkaZ63U1P"
```

### Task 1.2: Merge #5 — `CasGcShardPlan` absorbs `CasGcCursorKey.h`

**Files:**
- Result: `.../Core/CasGcShardPlan.h` (+ `.cpp` unchanged)
- Remove: `.../Core/CasGcCursorKey.h`
- Sweep: `src/Disks/tests/gtest_cas_gc_shard_plan.cpp` and any GC consumer of `CasGcCursorKey.h`.

**Interfaces:** Produces: `CasGcCursorKey` type declared inside `CasGcShardPlan.h`, same name/API.

- [ ] **Step 1:** Move `CasGcCursorKey.h`'s 39-line body into `CasGcShardPlan.h` (types on top), `git rm` the header.
- [ ] **Step 2:** SP-3 `OLD_SUBPATH=Core/CasGcCursorKey.h` → `NEW_SUBPATH=Core/CasGcShardPlan.h`.
- [ ] **Step 3:** SP-2 delta **−1**. Merge Gate `<tag>=merge5`.
- [ ] **Step 4: Commit** — message `cas(refactor): merge CasGcCursorKey into CasGcShardPlan` (+ standard trailers).

### Task 1.3: Merge #6 — `CasGcScheduler.h` absorbs `CasGcRoundLogRecord.h`

**Files:**
- Result: `.../ContentAddressed/CasGcScheduler.h` (top-level; `.cpp` unchanged)
- Remove: `.../ContentAddressed/CasGcRoundLogRecord.h`
- Sweep: any Interpreters/consumer of `CasGcRoundLogRecord.h`.

**Interfaces:** Produces: `GcRoundLogRecord` POD declared in `CasGcScheduler.h`, still dependency-free (the Interpreters decoupling survives — the record keeps zero dependencies).

- [ ] **Step 1:** Move the POD into `CasGcScheduler.h`. **Verify** the record still includes nothing beyond standard types (invariant: zero deps preserved). `git rm` the header.
- [ ] **Step 2:** SP-3 `OLD_SUBPATH=CasGcRoundLogRecord.h` → `NEW_SUBPATH=CasGcScheduler.h`.
- [ ] **Step 3:** SP-2 delta **−1**. Merge Gate `<tag>=merge6`.
- [ ] **Step 4: Commit** — `cas(refactor): merge CasGcRoundLogRecord into CasGcScheduler`.

### Task 1.4: Merge #1 — `Core/CasTypes.h` absorbs the six identity micro-headers

**Files:**
- Create (result, via `git mv` from the largest input to carry history): `.../Core/CasTypes.h`
- Remove/absorb: `Core/CasIds.h`, `Core/CasToken.h`, `Core/CasBlobDigest.h`, `Core/CasBlobRef.h`, `Core/CasManifestId.h`, `Core/CasRefIds.h`
- Sweep: all six paths across `src/` + `programs/` (heavily used — `gtest_cas_ids.cpp`, `gtest_cas_blob_digest.cpp`, `gtest_cas_blob_ref.cpp`, `gtest_cas_manifest_id.cpp`, and most CA cpp/h files).

**Interfaces:** Produces: `RootNamespace`, `Token`, `BlobDigest`, `BlobRef`, `ManifestId`, `RefTxnId` + hex helpers, all in `CasTypes.h`, names unchanged. (Hex helpers stay here — their pre-v3 home `CasCodecUtil` was removed by v3.)

- [ ] **Step 1:** `git mv Core/CasIds.h Core/CasTypes.h` (largest/most-central input keeps history), then append the other five headers' bodies in dependency order (digest → ref → manifestid → token → refids), dropping each `#pragma once` and now-internal cross-includes. `git rm` the other five.
- [ ] **Step 2:** SP-3 six times (one per absorbed header), each `OLD_SUBPATH=Core/<Name>.h` → `NEW_SUBPATH=Core/CasTypes.h`. The `CasIds.h` path is already gone via `git mv`; sweep it too so consumers repoint to `CasTypes.h`.
- [ ] **Step 3:** SP-2 delta **−5** (6 inputs → 1 result). Merge Gate `<tag>=merge1`.
- [ ] **Step 4: Commit** — `cas(refactor): merge identity micro-headers into CasTypes`.

### Task 1.5: Merge #2 — `ContentAddressedTransaction` absorbs `ContentAddressedWriteBuffers.{h,cpp}`

**Files:**
- Result: `.../ContentAddressed/ContentAddressedTransaction.{h,cpp}` (top-level, exists)
- Remove: `.../ContentAddressed/ContentAddressedWriteBuffers.{h,cpp}`
- Sweep: `src/Disks/tests/gtest_cascade_and_memory_write_buffer.cpp`, `gtest_ca_transaction.cpp`, any consumer of the write-buffers header.

**Interfaces:** Produces: the write-buffer classes declared/defined inside the transaction TU; buffers are created only by `writeFile` (9/9 buffer commits co-touch the transaction). Names/APIs unchanged.

- [ ] **Step 1:** Append `ContentAddressedWriteBuffers.h` into `ContentAddressedTransaction.h` and `...WriteBuffers.cpp` into `...Transaction.cpp`, dropping duplicate includes/`#pragma once`. `git rm` both write-buffer files.
- [ ] **Step 2:** SP-3 `OLD_SUBPATH=ContentAddressedWriteBuffers.h` → `NEW_SUBPATH=ContentAddressedTransaction.h`.
- [ ] **Step 3:** SP-2 delta **−2**. Merge Gate `<tag>=merge2`.
- [ ] **Step 4: Commit** — `cas(refactor): merge ContentAddressedWriteBuffers into ContentAddressedTransaction`.

### Task 1.6: Merge #3 — `Core/CasServerRoot` absorbs `CasSingleWriterSlot` + `CasStagingSweeper`

**Files:**
- Result: `.../Core/CasServerRoot.{h,cpp}` (exists)
- Remove: `Core/CasSingleWriterSlot.{h,cpp}`, `Core/CasStagingSweeper.{h,cpp}`
- Sweep: consumers of both headers.

**Interfaces:** Produces: `SingleWriterSlot` base (its one subclass `MountLeaseKeeper` lives in `CasServerRoot`) and the mount-scoped staging sweeper (~45 lines) inside `CasServerRoot`. Names/APIs unchanged. **Preserve the staging-sweeper's documented exception** (it bypasses `Backend` into `IObjectStorage`) — do not reroute any call.

- [ ] **Step 1:** Append `CasSingleWriterSlot.{h,cpp}` then `CasStagingSweeper.{h,cpp}` into `CasServerRoot.{h,cpp}`, drop guards/internal includes, `git rm` all four.
- [ ] **Step 2:** SP-3 twice: `Core/CasSingleWriterSlot.h`→`Core/CasServerRoot.h`, `Core/CasStagingSweeper.h`→`Core/CasServerRoot.h`.
- [ ] **Step 3:** SP-2 delta **−4**. Merge Gate `<tag>=merge3`.
- [ ] **Step 4: Commit** — `cas(refactor): merge SingleWriterSlot + StagingSweeper into CasServerRoot`.

### Task 1.7: Merge #7 — `PartFolderAccess` absorbs `PartRefKey` + `PartFolderView` + `CachedPartFolderAccess`

**Files:**
- Create (result): `.../ContentAddressed/PartFolderAccess.{h,cpp}` (top-level; ~900 lines total)
- Remove/absorb: `PartRefKey.h`, `PartFolderView.{h,cpp}`, `CachedPartFolderAccess.{h,cpp}` (and `PartFolderValidate`, which lives inside the cache header)
- Sweep: `gtest_cas_part_folder_access.cpp`, `gtest_cas_part_folder_view.cpp`, and part-path consumers.

**Interfaces:** Produces: `PartRefKey` + `Freshness` + `PartFolderValidate` + `PartFolderView` + `CachedPartFolderAccess` in one file reading top-down (key → value → cache of one mechanism). Names/APIs unchanged.

> **Flag (spec §Merges):** merges #7 and #8 also tidy internal declaration order — their diff is NOT purely line-mechanical. Keep the tidy strictly to declaration ordering; no logic edits. Reviewer: check the diff is reorder-only.

- [ ] **Step 1:** `git mv CachedPartFolderAccess.h PartFolderAccess.h` and `git mv CachedPartFolderAccess.cpp PartFolderAccess.cpp` (largest input carries history), then append `PartRefKey.h`, `PartFolderView.{h,cpp}` in reading order (key → view → validate → cached), reordering declarations top-down. `git rm` the three remaining absorbed files.
- [ ] **Step 2:** SP-3 for each absorbed path (`PartRefKey.h`, `PartFolderView.h`, `CachedPartFolderAccess.h`) → `PartFolderAccess.h`.
- [ ] **Step 3:** SP-2 delta **−3** (5 files → `PartFolderAccess.{h,cpp}`). Merge Gate `<tag>=merge7`.
- [ ] **Step 4: Commit** — `cas(refactor): merge PartRefKey + PartFolderView + CachedPartFolderAccess into PartFolderAccess`.

### Task 1.8: Merge #8 — `Core/CasRefProtocol` absorbs `CasRefStateMachine` + `CasRefIntake`

**Files:**
- Create (result): `.../Core/CasRefProtocol.{h,cpp}`
- Remove/absorb: `Core/CasRefStateMachine.{h,cpp}`, `Core/CasRefIntake.{h,cpp}`
- Sweep: `gtest_cas_ref_statemachine.cpp`, `gtest_cas_ref_intake.cpp`, ref-protocol consumers.

**Interfaces:** Produces: the two pure-logic halves of the ref protocol (replay + intake planning) in one file. The two *codecs* stay in `Formats/` (v3 physical rule — do NOT pull them in). Names/APIs unchanged.

> **Flag:** like #7, this merge tidies internal declaration order (not purely line-mechanical). Reorder only.

- [ ] **Step 1:** `git mv Core/CasRefStateMachine.h Core/CasRefProtocol.h` and `.cpp`, append `CasRefIntake.{h,cpp}` (replay on top, intake below), `git rm` the intake files.
- [ ] **Step 2:** SP-3: `Core/CasRefStateMachine.h`→`Core/CasRefProtocol.h`, `Core/CasRefIntake.h`→`Core/CasRefProtocol.h`.
- [ ] **Step 3:** SP-2 delta **−2**. Merge Gate `<tag>=merge8`.
- [ ] **Step 4: Commit** — `cas(refactor): merge CasRefStateMachine + CasRefIntake into CasRefProtocol`.

**Phase 1 exit:** total CA-tree delta ≈ **−19** files from the Task 0 baseline; battery green at every commit.

---

## Phase 2 — Move To The Target Tree

One phase, several commits. Move commits contain **no** content edits (SP-2 confirms history via `git log --follow`). The include sweep, the CMake edit, the `toEventKind` move, and the README are the only content edits, each in its own commit and clearly not a move.

**Ordering:** (2.1) create empty target dirs is unnecessary — `git mv` creates them. Move layer-by-layer bottom-up so the tree is coherent at each commit: Primitives → Formats → Backend → Pool → Gc → Tools → Parts → top-level facade stays put. Then the one CMake commit, then the `toEventKind` direction fix, then the README, then the stale-path grep.

### Task 2.1: Move `Primitives/`

**Files (git mv, one commit):**
- `Core/CasTypes.h` → `Primitives/CasTypes.h`
- `Core/CasBlobHasher.{h,cpp}` → `Primitives/CasBlobHasher.{h,cpp}`
- `Core/CasXXH3.h` → `Primitives/CasXXH3.h`
- `Core/CasEvent.{h,cpp}` → `Primitives/CasEvent.{h,cpp}`  *(the `toEventKind` direction fix is Task 2.9, a separate content commit — move the file as-is here)*
- `Core/CasSourceEdgeMarkers.h` → `Primitives/CasSourceEdgeMarkers.h`  *(placement resolved in Task 0 Step 4)*

- [ ] **Step 1:** `git mv` each pair above (NO content edits).
- [ ] **Step 2:** SP-2 — count **unchanged**; `git log --follow` on `Primitives/CasTypes.h` shows pre-move history.
- [ ] **Step 3: Commit** — `cas(refactor): move Primitives/ (git mv only, no content)`. Do NOT build yet (includes are stale until Task 2.8 sweep). *(If per-commit green is required, fold 2.1–2.8 into a single "move + sweep" mega-commit — see Task 2.8 note.)*

> **DESIGN NOTE on move-vs-sweep atomicity (flag to reviewer):** Invariant #4 forbids content edits in a move commit, but a pure `git mv` commit leaves includes stale → that single commit does not build. Two legal reconciliations: **(A)** one `git mv`-only commit for ALL of phase 2's moves, immediately followed by one sweep+CMake content commit — the *pair* is green, each commit is either all-moves or all-content (satisfies #4 literally; the intermediate move commit is a known non-building checkpoint, acceptable because history/bisect treats the pair as the unit). **(B)** per-layer move commits that are individually non-building, with a single trailing sweep. This plan uses **(A)**: Tasks 2.1–2.7 are `git mv` steps that accumulate into **one** move commit (commit only at Task 2.7), then Task 2.8 is the single content sweep commit. This keeps `git log --follow` clean AND gives one green checkpoint at 2.8. Adjust the per-task "Commit" steps accordingly: 2.1–2.6 end with `git add -A` (no commit); 2.7 commits the whole move; 2.8 commits the sweep+CMake.

- [ ] **Step 3 (revised per Note A):** `git add -A` only. No commit, no gate yet.

### Task 2.2: Move `Formats/`

**Files (git mv, accumulate):**
- `Core/Formats/*` → `Formats/*` (all v3 per-object format files + `README.md`)
- `Core/CasLayout.{h,cpp}` → `Formats/CasLayout.{h,cpp}` (keys are part of the persisted schema → joins Formats)
- Any surviving `CasWireVocab`/`CasRefWireVocab` (per Task 0 Step 4) → `Formats/`

- [ ] **Step 1:** `git mv Core/Formats/<each> Formats/<each>`; `git mv Core/CasLayout.{h,cpp} Formats/`.
- [ ] **Step 2:** `git add -A` only (part of the single move commit).

### Task 2.3: Move `Backend/`

**Files (git mv, accumulate):**
- `Core/CasBackend.h` → `Backend/CasBackend.h`
- `Core/CasObjectStorageBackend.{h,cpp}` → `Backend/CasObjectStorageBackend.{h,cpp}`
- `Core/CasInMemoryBackend.{h,cpp}` → `Backend/CasInMemoryBackend.{h,cpp}`
- `Core/CasInstrumentedBackend.{h,cpp}` → `Backend/CasInstrumentedBackend.{h,cpp}`
- `Core/CasRequestControl.{h,cpp}` → `Backend/CasRequestControl.{h,cpp}`
- `Core/CasProbe.{h,cpp}` → `Backend/CasProbe.{h,cpp}`

- [ ] **Step 1:** `git mv` each. **Step 2:** `git add -A` only.

### Task 2.4: Move `Pool/`

**Files (git mv, accumulate):**
- `Core/CasStore.{h,cpp}` → `Pool/CasStore.{h,cpp}` *(renamed to `CasPool` in phase 5, not here)*
- `Core/CasBuild.{h,cpp}` → `Pool/CasBuild.{h,cpp}` *(→ `CasPartWriteTxn` in phase 5)*
- `Core/CasRefProtocol.{h,cpp}` → `Pool/CasRefProtocol.{h,cpp}`
- `Core/CasServerRoot.{h,cpp}` → `Pool/CasServerRoot.{h,cpp}`
- `Core/CasPoolMeta.{h,cpp}` → `Pool/CasPoolMeta.{h,cpp}`
- `Core/CasBlobMeta.{h,cpp}` → `Pool/CasBlobMeta.{h,cpp}`

> The new `Pool/` component files (`CasRefLedger`, `CasMountRuntime`, `CasManifestReader`, `CasPlainObjects`, `CasBlobUploader`) do NOT exist yet — they are created in phases 3–4. Do not stub them here.

- [ ] **Step 1:** `git mv` each. **Step 2:** `git add -A` only.

### Task 2.5: Move `Gc/`

**Files (git mv, accumulate):**
- `CasGcScheduler.{h,cpp}` (top-level) → `Gc/CasGcScheduler.{h,cpp}`
- `Core/CasGc.{h,cpp}` → `Gc/CasGc.{h,cpp}`
- `Core/CasGcShardPlan.{h,cpp}` → `Gc/CasGcShardPlan.{h,cpp}`
- `Core/CasBlobInDegree.{h,cpp}` → `Gc/CasBlobInDegree.{h,cpp}`
- `Core/CasOrphanManifestSweep.{h,cpp}` → `Gc/CasOrphanManifestSweep.{h,cpp}`

- [ ] **Step 1:** `git mv` each. **Step 2:** `git add -A` only.

### Task 2.6: Move `Tools/`

**Files (git mv, accumulate):**
- `Core/CasFsck.{h,cpp}` → `Tools/CasFsck.{h,cpp}`
- `Core/CasDecommission.{h,cpp}` → `Tools/CasDecommission.{h,cpp}`
- `Core/CasInspect.{h,cpp}` → `Tools/CasInspect.{h,cpp}` *(only if it survived v3 — per Task 0 Step 4; if v3 deleted it, skip)*

- [ ] **Step 1:** `git mv` each surviving file. **Step 2:** `git add -A` only.

### Task 2.7: Move `Parts/` + commit the whole move

**Files (git mv):**
- `PartPathParser.{h,cpp}` (top-level) → `Parts/PartPathParser.{h,cpp}`
- `PartFolderAccess.{h,cpp}` (top-level, from merge #7) → `Parts/PartFolderAccess.{h,cpp}`

- [ ] **Step 1:** `git mv` each.
- [ ] **Step 2:** Confirm `Core/` is now empty: `git ls-files 'src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/**'` prints nothing.
- [ ] **Step 3:** SP-2 — total count **unchanged** vs Phase-1 exit; `git log --follow` on e.g. `Pool/CasStore.cpp` and `Parts/PartFolderAccess.h` shows full history.
- [ ] **Step 4: Commit the single move** (no content):
```bash
git commit -m "cas(refactor): move Core/ into layered tree (git mv only, no content edits)

Primitives/ Formats/ Backend/ Pool/ Gc/ Tools/ Parts/ per spec target layout.
Include paths and CMake are fixed in the following commit; this commit is
move-only so git log --follow tracks every file.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01PeC1dg2dXBh55xkaZ63U1P"
```

### Task 2.8: Include-path sweep + CMake update (single content commit → first green checkpoint)

**Files:**
- Modify: all `#include` sites across `src/`, `programs/` (scripted).
- Modify: `src/CMakeLists.txt:133-135` — replace the `Core` and `Core/Formats` directory lines with the seven new subdirs.

**Interfaces:** Produces the first buildable state of phase 2. Consumes the move commit's new paths.

- [ ] **Step 1: Sweep every old path prefix → new.** Run SP-3 once per moved-file path change. The mapping is mechanical: every `ContentAddressed/Core/Formats/X` → `ContentAddressed/Formats/X`; every `ContentAddressed/Core/CasLayout.` → `ContentAddressed/Formats/CasLayout.`; every `ContentAddressed/Core/<Backend-file>` → `.../Backend/<file>`; likewise Pool/Gc/Tools/Primitives; top-level `ContentAddressed/CasGcScheduler` → `ContentAddressed/Gc/CasGcScheduler`; `ContentAddressed/PartPathParser` and `ContentAddressed/PartFolderAccess` → `.../Parts/...`. Drive it from the Task 0 inventory diff (old path → new path table) so nothing is missed. A single bulk form:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
# Collapse the two most common prefixes first:
git grep -l -- 'ContentAddressed/Core/Formats/' -- src programs | xargs -r sed -i 's#ContentAddressed/Core/Formats/#ContentAddressed/Formats/#g'
# Then per-file for the remaining Core/* files, from a generated map (old<TAB>new):
while IFS=$'\t' read -r old new; do
  git grep -l -- "$old" -- src programs | xargs -r sed -i "s#${old}#${new}#g"
done < tmp/cas_move_map.tsv
```
Build `tmp/cas_move_map.tsv` from the Task 0 inventory (old subpath → new subpath, one per moved file).
- [ ] **Step 2: Update `src/CMakeLists.txt`.** Replace:
```cmake
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats)
```
with:
```cmake
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools)
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts)
```
Keep the existing top-level `.../ContentAddressed` line (facade files + `ContentAddressedTransaction` + `ContentAddressedExchange.h` stay there).
> **Flag:** this is a real (expected) build-file edit — the CA dir uses per-directory `add_headers_and_sources` (non-recursive glob), so new subdirs are NOT auto-discovered. gtests ARE auto-discovered (`GLOB_RECURSE gtest*.cpp`), so no test-CMake edit is needed.
- [ ] **Step 3: Verify no stale path anywhere:** `git grep -n 'ContentAddressed/Core' -- src programs || echo clean` → `clean`.
- [ ] **Step 4:** SP-2 (count unchanged) + **Move Gate** SP-1 `<tag>=phase2` — **first green checkpoint of phase 2**. Expected `NINJA_EXIT=0`, `GTEST_EXIT=0`, PASSED==baseline.
- [ ] **Step 5: Commit** — `cas(refactor): fix include paths + CMake for the layered tree` (+ trailers).

### Task 2.9: `toEventKind` direction fix

**Files:**
- Modify: `Primitives/CasEvent.h` (remove the envelope-header include + `toEventKind` body)
- Modify: `Formats/CasBlobEnvelopeFormat.h` (add `toEventKind`)
- Sweep: callers of `toEventKind`.

**Interfaces:** Produces: `CasEvent` becomes dependency-free (legal `Primitives` position); `toEventKind` lives in `Formats` (legal `Formats → Primitives` direction). This is the single post-v3 direction violation the spec says to fix here (spec §Include Direction Rule).

- [ ] **Step 1:** Cut `toEventKind` from `Primitives/CasEvent.h` and its envelope include; paste `toEventKind` into `Formats/CasBlobEnvelopeFormat.h`.
- [ ] **Step 2:** Repoint callers (`git grep -n 'toEventKind'`) to include `Formats/CasBlobEnvelopeFormat.h`.
- [ ] **Step 3:** Confirm `CasEvent.h` no longer includes anything from `Formats/` (`grep -n include Primitives/CasEvent.h`).
- [ ] **Step 4:** SP-2 (unchanged) + Move Gate `<tag>=eventkind`.
- [ ] **Step 5: Commit** — `cas(refactor): move toEventKind into Formats so CasEvent is dependency-free`.

### Task 2.10: Root `README.md` + docs/scripts path sweep

**Files:**
- Create: `.../ContentAddressed/README.md`
- Modify: live CAS doc set `docs/superpowers/cas/*.md` (only the current/live docs — NOT dated historical reports/plans/specs, which are frozen artifacts) and any script under `utils/`/`.claude/` referencing the old `Core` path.

**Interfaces:** Produces the layer map / include rule / named exceptions / reading order that IS the direction-rule enforcement (README-only, no CI).

- [ ] **Step 1:** Write `README.md` with: the layer map (`Primitives → Formats → Backend → Pool → Gc → Tools ≈ Parts → facade`), the include-direction rule, the named exceptions (staging sweeper + `probeConditionalCopy` bypass `Backend` into `IObjectStorage`; `Backend` may read `Formats` traits), and the reading order: `ContentAddressedMetadataStorage` → `route`/`PartRefKey` → `PartFolderAccess` → `CasPool` → `CasPartWriteTxn` → `CasGc`. (Use post-phase-5 names in the reading order, or note the pending rename — reviewer's choice; keep names consistent with the tree at README-commit time and update in phase 5.)
- [ ] **Step 2:** Sweep live `docs/superpowers/cas/*.md` + `utils`/`.claude` scripts for `ContentAddressed/Core` → new paths. Leave dated `docs/superpowers/reports/*`, `plans/*`, `specs/*` untouched (historical record).
- [ ] **Step 3:** `git grep -n 'ContentAddressed/Core' -- docs/superpowers/cas utils .claude || echo clean`.
- [ ] **Step 4: Commit** — `cas(refactor): add ContentAddressed/README.md layer map + doc path sweep`. (Docs-only; no gate needed, but run SP-1 `<tag>=phase2end` once to confirm the tree is still green before entering surgery.)

**Phase 2 exit:** `Core/` gone; layered tree in place; battery green; `git log --follow` intact for every file.

---

## Phase 3 — Decompose `CasStore` Into Four Components

> **THE REAL RISK (flag to reviewer, spec §Risks).** This is NOT a scripted move — it is hand C++ surgery: members migrate between classes, constructors gain environment parameters, `CasPool` grows thin delegates, friendships die, `PoolConfig` splits. The binding invariant: **every component owns its mutexes wholesale; no lock changes covered state; no acquisition order changes; construction/destruction order is preserved verbatim.** One component = one commit. An umbrella review runs on the phase-3 diffs before the final gate.

**Cutting principle (spec §Decomposition):** state/lock ownership boundaries. Environment reaches components via constructor parameters and callbacks — **never** a `Pool &` back-reference.

**Risk order (spec):** warm-up (`CasPlainObjects` + `CasManifestReader`) → the big one (`CasRefLedger`) → `CasMountRuntime`. Each extraction:
1. Create the new `Pool/<Component>.{h,cpp}`.
2. Move the member group + its mutex(es) wholesale from `CasStore` (now at `Pool/CasStore.{h,cpp}`), carrier types with them.
3. Wire environment via ctor params/callbacks; leave a thin delegating wrapper on `CasStore` so the external API is unchanged.
4. **Verify the invariant** (locks unchanged, order unchanged, teardown order reproduced).
5. SP-2 (count **+2** per component: new `.h`+`.cpp`; CMake auto-globs `Pool/` since Task 2.8 added the line) + **Decomp Gate** SP-1.
6. Commit.

> **Member-level inventory note:** the spec's method lists (below, quoted from §Decomposition) are authoritative. The exact field-level enumeration must be re-derived against the landed post-v3 `Pool/CasStore.{h,cpp}` at execution time (spec §Deferred To Plans). Do the enumeration as the first step of each extraction, cross-checking every named method/field is accounted for.

### Task 3.1: Extract `Pool/CasPlainObjects` (warm-up)

**Files:** Create `Pool/CasPlainObjects.{h,cpp}`; modify `Pool/CasStore.{h,cpp}`.

**Interfaces:** `CasPlainObjects` is **stateless over `Backend &` + `const Layout &`**. Takes: `casPutObject` / `casGetObject` / `casRemoveObject` and the namespace-file + mountpoint-object surfaces. Produces thin `CasStore` delegates with identical signatures.

- [ ] **Step 1:** Enumerate the three `cas*Object` methods + namespace-file/mountpoint surfaces on `CasStore`; confirm none touch `CasStore` mutexes (stateless).
- [ ] **Step 2:** Create `Pool/CasPlainObjects.{h,cpp}` (ctor: `Backend &`, `const Layout &`), move the method bodies verbatim.
- [ ] **Step 3:** Replace the `CasStore` methods with delegates to a `CasPlainObjects` member.
- [ ] **Step 4:** SP-2 (**+2**) + Decomp Gate `<tag>=plainobjects`.
- [ ] **Step 5: Commit** — `cas(refactor): extract CasPlainObjects from CasStore (stateless move)`.

### Task 3.2: Extract `Pool/CasManifestReader` (warm-up)

**Files:** Create `Pool/CasManifestReader.{h,cpp}`; modify `Pool/CasStore.{h,cpp}`.

**Interfaces:** Takes: `readManifest` / `readManifestShared`, the token-gated byte-weighted decode cache (`ManifestCacheKey`, weight functor, LRU) + its mutex wholesale, `locate`. Produces `CasStore` delegates.

- [ ] **Step 1:** Enumerate the manifest-read methods + the decode-cache members + their mutex; confirm the cache mutex is owned solely by this group.
- [ ] **Step 2:** Create `Pool/CasManifestReader.{h,cpp}`; move members + mutex + `ManifestCacheKey`/weight functor/LRU wholesale; ctor takes `Backend &`, `const Layout &`, token accessor.
- [ ] **Step 3:** `CasStore` delegates to a `CasManifestReader` member.
- [ ] **Step 4:** SP-2 (**+2**) + Decomp Gate `<tag>=manifestreader`.
- [ ] **Step 5: Commit** — `cas(refactor): extract CasManifestReader (read path + decode cache) from CasStore`.

### Task 3.3: Slice `PoolConfig` into owner sub-structs

**Files:** Modify the `PoolConfig` definition (in `Pool/CasStore.h` or its config header) + call sites.

**Interfaces (spec §PoolConfig Slices):**
- `RefLedgerConfig`: snapshot thresholds, both backoff pairs, `ref_table_cache_bytes`, admission budgets.
- `MountConfig`: lease TTL, renew period, `materialization_grace_ms`, `boot_ms_fn`, `wait_sleep_fn`.
- The rest stays on the pool. Public `PoolConfig` **remains as the aggregate of the slices** — external callers unchanged.

- [ ] **Step 1:** Introduce `RefLedgerConfig` and `MountConfig` structs; move the listed fields into them; make `PoolConfig` embed both (aggregate) so external field access still compiles or is trivially repointed.
- [ ] **Step 2:** Repoint internal readers of the moved fields to `config.ref_ledger.*` / `config.mount.*`.
- [ ] **Step 3:** SP-2 (count unchanged if structs are inline in an existing header; **+2** only if a new header is created — prefer inline to avoid a new file) + Decomp Gate `<tag>=poolconfig`.
- [ ] **Step 4: Commit** — `cas(refactor): slice PoolConfig into RefLedgerConfig + MountConfig (aggregate preserved)`.

### Task 3.4: Extract `Pool/CasRefLedger` (the big one)

**Files:** Create `Pool/CasRefLedger.{h,cpp}`; modify `Pool/CasStore.{h,cpp}`.

**Interfaces (spec §CasRefLedger — authoritative member list):** Takes the `ref_tables` map + `RefTableRuntime` wholesale, **both** mutexes (`ref_queue_mutex`, per-table `state_mutex`), `allocateRefTxnId` + `next_ref_sequence`, `ref_request_controller`; the append lane (`appendRefOps`, `runRefQueueLeader`, `flushRefBatch`, wedge semantics); recovery+seal (`ensureRefTableRecovered`); snapshot publication (`maybeScheduleSnapshotPublish`, `trySnapshotPublishOnce`, publish backoff pair); the stale-precommit sweep (all four methods + its backoff pair); cache-budget eviction (`enforceRefTableCacheBudget`); remount/shutdown coordination (`quiesceRefTablesForRemount`, `refLanesSettledForRemount`, `drainRefLanesForShutdown`); the read side (`resolveRef`, `listRefs`, `namespaceIsRemoved`, `observedNamespaceCleanupMarker`, `publishRemovedSnapshotNow`); the ref lifecycle entry points (`dropRef`, `updateRefPayload`, `dropNamespace`); `wedgedRefLaneCount` + the ~15 lane `*ForTest` seams. Carrier types move with it: `MutationScope`, `RootMutationOrigin`, `RootMutationKind`, `Resolved`, `RefPayloadUpdate`, `DropNamespaceStats`.

Environment via constructor — **never a `Pool &` back-reference**: `Backend &`, `const Layout &`, its `RefLedgerConfig` slice, and callbacks `live_epoch`, `fence_ok`, `emit_event`, `on_impossible_interference` (reaches up into remount), `boot_ms`, `wait_sleep`.

**Friendships die:** remove `friend class Build` and `friend class Gc` on `CasStore` — the transaction and GC reach the ledger through `CasRefLedger`'s public surface (`appendRefOps` already public; the friendship only covered internals that now move out).

- [ ] **Step 1:** Enumerate every listed method/field/carrier-type against the landed `Pool/CasStore.{h,cpp}`; build a checklist. Confirm `ref_queue_mutex` and per-table `state_mutex` are used ONLY by this group (grep every lock site) — if any lock site outside the group touches them, STOP and report (a shared lock means the cut boundary is wrong).
- [ ] **Step 2:** Create `Pool/CasRefLedger.{h,cpp}`; move the whole group + both mutexes + carrier types wholesale, **preserving lock-acquisition order verbatim**.
- [ ] **Step 3:** Add the constructor env params + callbacks; wire them from `CasStore`. `CasStore` holds a `CasRefLedger` member and delegates the public entry points.
- [ ] **Step 4:** Remove `friend class Build`/`friend class Gc`; repoint `Build`/`Gc` to `CasRefLedger`'s public surface.
- [ ] **Step 5: Verify teardown order** — the `~Store` sequence (drain lanes → keeper terminate → …) must be reproduced: the `CasRefLedger` member's destruction (drain lanes) must occur at the same point in `~CasStore` as before. Check member declaration order (destruction is reverse-declaration) matches the old teardown.
- [ ] **Step 6:** SP-2 (**+2**) + Decomp Gate `<tag>=refledger`.
- [ ] **Step 7: Commit** — `cas(refactor): extract CasRefLedger from CasStore (ref journal subsystem; friendships removed)`.

### Task 3.5: Extract `Pool/CasMountRuntime`

**Files:** Create `Pool/CasMountRuntime.{h,cpp}`; modify `Pool/CasStore.{h,cpp}`.

**Interfaces (spec §CasMountRuntime — authoritative):** Takes: `MountFence` + `mayMutate`/`tripMountLost`/`setMountDeadline`/`armMountFence`/`bootMs`/`bootMsNow`; ownership of `MountLeaseKeeper`; the self-remount machinery (`scheduleRemount` thread, its 5 atomics + 4 mutexes, `live_writer_epoch`, `unclean_epoch_boundary_seen_at`, `ownsAndSawUncleanBoundaryFor`); the watermark/build-seq surface (`process_epoch`, `next_build_seq`, `active_build_seqs`, `inflight_builds`, `minActive`, `renewWatermarkOnce`, `allocateBuildSeq`/`retireBuildSeq`). **Orchestration of `open`/`openForDecommission`/`tryRemountOnce` (claim → ledger quiesce → fence re-arm ordering) STAYS in `CasStore`; the mechanics live here.**

- [ ] **Step 1:** Enumerate the fence + self-remount + watermark members (5 atomics + 4 mutexes named); confirm they form a closed lock set used only here. Note `MountLeaseKeeper` moved into `CasServerRoot` (merge #3) — `CasMountRuntime` *owns an instance*, it does not redefine it.
- [ ] **Step 2:** Create `Pool/CasMountRuntime.{h,cpp}`; move members + mutexes + the `scheduleRemount` thread wholesale; ctor env params (backend, config `MountConfig` slice, ledger-quiesce callback for the orchestration seam).
- [ ] **Step 3:** Leave `open`/`openForDecommission`/`tryRemountOnce` orchestration in `CasStore`, calling into `CasMountRuntime` mechanics; preserve the claim→quiesce→re-arm ordering exactly.
- [ ] **Step 4: Verify teardown order** — the remount thread join / keeper terminate must occur at the same `~Store` point (member declaration order).
- [ ] **Step 5:** SP-2 (**+2**) + Decomp Gate `<tag>=mountruntime`.
- [ ] **Step 6: Commit** — `cas(refactor): extract CasMountRuntime (fence + self-remount + watermark) from CasStore`.

### Task 3.6: Confirm `CasStore` is the ~400-line composition root

**Files:** Read-only review of `Pool/CasStore.{h,cpp}`.

**Interfaces (spec §CasPool After):** what remains — the `open` protocol, ownership of backend/pool-meta/layout + the four components, the dedup cache and admitted-algos cache (~30 lines each, stay as members), the event sink, `startBuild`, `currentGcRound`, thin delegating wrappers keeping the external API unchanged.

- [ ] **Step 1:** Confirm nothing beyond the above remains inline; if a stray subsystem is still embedded, it belongs in a component — re-open the relevant extraction task. (No commit; this is a checkpoint.)
- [ ] **Step 2:** Dispatch the mandatory **umbrella review** (skill `ubrella-clickhose-review`) on the phase-3 commit range before the final gate. Address findings as new commits.

---

## Phase 4 — Extract `Pool/CasBlobUploader` From `CasBuild`

> Same "no logic change" invariant. `CasBuild` is at `Pool/CasBuild.{h,cpp}` (moved in phase 2; renamed in phase 5).

**Interfaces (spec §CasBlobUploader):** Takes the **byte-delivery engine**: the `putBlob` core, both `observeAndAdmit` overloads, `uploadFromSource`, HEAD-first, the S3-staging promote/resurrect paths. The **decision** logic (admit/adopt/resurrect choice, dep-set recording, the `promote` gate) STAYS in the transaction class — it is transactional state; **only the execution moves.**

### Task 4.1: Extract `Pool/CasBlobUploader`

**Files:** Create `Pool/CasBlobUploader.{h,cpp}`; modify `Pool/CasBuild.{h,cpp}`.

- [ ] **Step 1:** Enumerate the byte-delivery methods (`putBlob` core, both `observeAndAdmit`, `uploadFromSource`, HEAD-first, staging promote/resurrect). Draw the line at execution-vs-decision: the `promote` **gate** and admit/adopt/resurrect **choice** stay in `CasBuild`; the byte transfer moves.
- [ ] **Step 2:** Create `Pool/CasBlobUploader.{h,cpp}` (ctor: `Backend &`, `const Layout &`, event sink); move the execution bodies; `CasBuild` calls the uploader for byte delivery, keeps the decision state.
- [ ] **Step 3:** Confirm no mutex/lock moved (blob upload is transaction-scoped, not shared state); preserve the staging-sweeper documented exception if touched.
- [ ] **Step 4:** SP-2 (**+2**) + Decomp Gate `<tag>=blobuploader`.
- [ ] **Step 5: Commit** — `cas(refactor): extract CasBlobUploader (byte-delivery engine) from CasBuild`.

---

## Phase 5 — Renames

> **Boundary rule (spec §Renames):** only C++ identifiers + file names change. **No** persisted bytes, key layouts, log/error/event texts, ProfileEvents/metric names, or protocol vocab change. Protocol "build" (`build_seq`, `buildSeq`, `BuildPrefix`, upload stamps) SURVIVES — do not touch it. Namespace `DB::Cas` and the `ContentAddressed*`/`Part*` wiring convention are untouched.

Each rename is a scripted identifier sweep, but **scoped** so protocol-`build` terms are spared. Do renames as two commits (Store, then Build) so each is independently reviewable.

### Task 5.1: `Store` → `Pool`

**Files:** `Pool/CasStore.{h,cpp}` → `Pool/CasPool.{h,cpp}`; `gtest_cas_store.cpp` → `gtest_cas_pool.cpp`; all `Cas::Store`/`StorePtr` references.

**Rename map (identifiers):** `Cas::Store` → `Cas::Pool`, `StorePtr` → `PoolPtr`, class `Store` → `Pool`, file `CasStore` → `CasPool`, gtest `gtest_cas_store` → `gtest_cas_pool`.

- [ ] **Step 1:** `git mv Pool/CasStore.h Pool/CasPool.h`; `git mv Pool/CasStore.cpp Pool/CasPool.cpp`; `git mv src/Disks/tests/gtest_cas_store.cpp src/Disks/tests/gtest_cas_pool.cpp`.
- [ ] **Step 2:** Scripted identifier sweep (word-boundary anchored to avoid `build`/`Store`-substring collisions):
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
# class/type + smart-ptr + file-path tokens. Word boundaries prevent partial hits.
git grep -l -- 'CasStore\|Cas::Store\|StorePtr' -- src programs \
  | xargs -r sed -i -e 's#\bCasStore\b#CasPool#g' -e 's#Cas::Store\b#Cas::Pool#g' -e 's#\bStorePtr\b#PoolPtr#g'
# The bare class name `Store` inside namespace Cas — sed the qualified/decl forms explicitly,
# NOT a blanket s/Store/Pool/ (would corrupt unrelated identifiers). Inspect `git grep -nw Store`
# within the CA tree and repoint each decl/use by hand-verified pattern.
```
> **Flag:** a blanket `s/Store/Pool/` is UNSAFE (hits unrelated `Store`, `IMetadataStorage`, etc.). Anchor to `CasStore`, `Cas::Store`, `StorePtr`, and hand-verify the bare `Store` class-name occurrences (they are confined to the `Cas` namespace, easily enumerated with `git grep -nw Store -- 'src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed'`).
- [ ] **Step 3:** Update the `README.md` reading-order names (`CasStore`→`CasPool`) if written earlier with old names.
- [ ] **Step 4:** SP-2 (unchanged) + Decomp Gate `<tag>=rename-pool`. Confirm `git log --follow Pool/CasPool.cpp` history intact.
- [ ] **Step 5: Commit** — `cas(refactor): rename Store -> Pool (identifiers + files; no persisted/protocol change)`.

### Task 5.2: `Build` → `PartWriteTxn`

**Files:** `Pool/CasBuild.{h,cpp}` → `Pool/CasPartWriteTxn.{h,cpp}`; `gtest_cas_build.cpp` → `gtest_cas_part_write.cpp`; `gtest_cas_build_root_dangle.cpp` → `gtest_cas_part_write_root_dangle.cpp`; all `Cas::Build`/`BuildPtr`/`BuildInfo`/`Store::startBuild` references.

**Rename map:** `Cas::Build` → `Cas::PartWriteTxn`, `BuildPtr` → `PartWriteTxnPtr`, `BuildInfo` → `PartWriteInfo`, `Store::startBuild` → `Pool::beginPartWrite` (method rename), files `CasBuild` → `CasPartWriteTxn`.

> **CRITICAL (spec §Renames):** spare the protocol term `build`. Do NOT rename `build_seq`, `buildSeq`, `BuildPrefix`, `allocateBuildSeq`, `retireBuildSeq`, `active_build_seqs`, `next_build_seq`, `inflight_builds`, or upload-stamp `build` tokens. The rename touches ONLY the class name `Build`, its smart-ptr, `BuildInfo`, and `startBuild`.

- [ ] **Step 1:** `git mv Pool/CasBuild.h Pool/CasPartWriteTxn.h`; `.cpp` likewise; `git mv` the two gtest files.
- [ ] **Step 2:** Anchored identifier sweep sparing protocol-`build`:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git grep -l -- 'CasBuild\|Cas::Build\|BuildPtr\|BuildInfo\|startBuild' -- src programs \
  | xargs -r sed -i \
    -e 's#\bCasBuild\b#CasPartWriteTxn#g' \
    -e 's#Cas::Build\b#Cas::PartWriteTxn#g' \
    -e 's#\bBuildPtr\b#PartWriteTxnPtr#g' \
    -e 's#\bBuildInfo\b#PartWriteInfo#g' \
    -e 's#\bstartBuild\b#beginPartWrite#g'
# Bare class name `Build` in namespace Cas: enumerate + repoint by hand (git grep -nw Build within CA tree),
# NEVER a blanket s/Build/.../ (would hit build_seq/BuildPrefix/etc — the protocol vocab that MUST survive).
```
- [ ] **Step 3: Verify protocol vocab survived:** `git grep -nw 'build_seq\|buildSeq\|BuildPrefix\|allocateBuildSeq\|active_build_seqs' -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed` — counts must be **unchanged** from before Step 2.
- [ ] **Step 4:** Update `README.md` reading order (`CasPartWriteTxn`).
- [ ] **Step 5:** SP-2 (unchanged) + Decomp Gate `<tag>=rename-partwritetxn`. `git log --follow` intact.
- [ ] **Step 6: Commit** — `cas(refactor): rename Build -> PartWriteTxn (class only; protocol build_seq vocab preserved)`.

---

## Final Gate

> Exercises what gtests cannot: the ref lane under load, remount, GC concurrency.

- [ ] **Step 1: Umbrella review** of the full phase-3/4/5 diff range (if not already done in Task 3.6, or re-run to cover 4–5). Address findings as new commits + re-gate.
- [ ] **Step 2: CA-default stateless run.** Per the `cas-test-triage` / `clickhouse-praktika-tests` procedure, run the CA-default stateless job against the freshly built binary. Redirect to a log; subagent summarizes. Expected: no new failures vs the pre-refactor CA-default baseline (compare against the known CA-s3-lane ignore list).
- [ ] **Step 3: Short ca-soak.** Per `reference_ca_soak_duration_phase3` — a **time-driven phase-3 soak** (`--duration <N>m`, set N explicitly, e.g. `--duration 20m`; phase-1 `--ops` is op-driven and finishes ~10x faster, so it does NOT exercise the lane long enough). Fresh restart per `reference_ca_soak_fresh_restart`. Expected: GREEN — no wedged lane, no leak, no assertion, clean remount.
- [ ] **Step 4:** Report both green results to the team lead with the commit range.

---

## Self-Review (author checklist — completed)

**Spec coverage:**
- §Precondition (v3 landed) → Task 0. ✅
- §Target Layout (7 subdirs + facade) → Phase 2 (Tasks 2.1–2.7) + CMake 2.8. ✅
- §Include Direction Rule + `toEventKind` fix → Task 2.9; README rule → Task 2.10. ✅
- §Merges #1–#8 → Tasks 1.1–1.8 (mapped by number). ✅
- §Decomposition (`CasRefLedger`, `CasMountRuntime`, `CasManifestReader`, `CasPlainObjects`, `CasBlobUploader`, friendships, `PoolConfig` slices, `CasPool` after) → Tasks 3.1–3.6, 4.1. ✅
- §Renames (Store→Pool, Build→PartWriteTxn, 3 gtest renames, protocol-build spared) → Tasks 5.1–5.2. ✅
- §Migration Phases + Gates → phase headers + SP-1 gate + Final Gate. ✅
- §Invariants → Global Constraints + per-task teardown/lock verify steps. ✅
- §Risks (concurrency surgery, conflicts, history loss, stale paths, baseline drift) → Task 0, Note-A move/sweep split, SP-2 `--follow`, Task 2.8 Step 3 grep, Task 0 Step 4. ✅
- §Deferred To Plans (member inventories, sed mechanics, README content, backlog) → Task-3 enumeration steps, SP-3, Task 2.10, Global Constraint #5. ✅

**Placeholder scan:** no TBD/TODO; every content step shows the command/mapping; the one genuinely-non-mechanical area (phase 3/4 member enumeration) is explicitly flagged as hand-surgery with the authoritative spec member list inlined. ✅

**Type consistency:** `CasStore`→`CasPool`, `CasBuild`→`CasPartWriteTxn`, `startBuild`→`beginPartWrite`, `StorePtr`→`PoolPtr`, `BuildPtr`→`PartWriteTxnPtr`, `BuildInfo`→`PartWriteInfo` used consistently across phases 2 (old names, pre-rename) and 5 (new names). ✅

## Flags For The Team Lead (places the spec is NOT a pure scripted move)

1. **Precondition unmet at planning time:** current tree is PRE-v3. Execution is HARD-GATED on codecs v3 landing (Task 0). The plan is written against the spec's post-v3 target; Task 0 re-verifies exact surviving names + resolves drift (`CasSourceEdgeMarkers.h`→Primitives, `CasPoolMeta.h` existence, `CasWireVocab`/`CasRefWireVocab` fate, `CasInspect` fate).
2. **CMake is a real edit (not a move):** the CA dir uses per-directory `add_headers_and_sources` (non-recursive glob), so the seven new subdirs REQUIRE explicit `src/CMakeLists.txt` lines and removal of the `Core`/`Core/Formats` lines (Task 2.8). gtests are `GLOB_RECURSE`-discovered — no test-CMake edit needed.
3. **Phases 3–4 are hand C++ surgery, NOT scripted:** god-object decomposition moves members between classes, adds ctor env/callbacks, adds delegates, removes friendships, splits `PoolConfig`. This is the spec-acknowledged "more than a move" area. Mitigated by wholesale lock+state moves, per-component commits, teardown-order verification, and the mandatory umbrella review.
4. **Merges #7 and #8 tidy internal declaration order** — their diffs are not purely line-mechanical (reorder-only, no logic).
5. **Move-vs-sweep atomicity:** invariant #4 (moves and content never in one commit) means a pure `git mv` commit does not build; resolved via Note-A (one move commit for all of phase 2, then one sweep+CMake content commit = first green checkpoint at Task 2.8).
6. **Phase-5 renames need anchored seds, not blanket:** `s/Store/.../` and `s/Build/.../` are UNSAFE (hit `IMetadataStorage`, and the protocol `build_seq`/`BuildPrefix` vocab that MUST survive). Every rename sweep is word-boundary anchored to `CasStore`/`Cas::Store`/`StorePtr` / `CasBuild`/`Cas::Build`/`BuildPtr`/`BuildInfo`/`startBuild`, plus a hand-verified pass for the bare class names, plus a Step-3 grep proving protocol-`build` counts are unchanged.

No spot makes the spec infeasible — the scripted parts (merges, moves, include sweep, renames) are mechanical as required; the non-scripted parts (decomposition) are exactly the ones the spec pre-declared as surgery.
