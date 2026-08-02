---
description: 'Midpoint audit of CAS Stage B at baseline ce312f547c3: six read-only verification reports (Tasks 5/5b, 6/6b, 7/7a/7b, 8/9, 10, document consistency) checking every ledger/handoff completion claim against the tree. Grounds the superseding remaining-work plan. Evidence and provenance only — normative requirements live in the plan.'
sidebar_label: 'Stage B midpoint audit'
sidebar_position: 20260802
slug: /superpowers/cas/stage-b-midpoint-audit
title: 'CAS Stage B: midpoint audit'
doc_type: 'reference'
---

# CAS Stage B: midpoint audit (2026-08-02) {#stage-b-midpoint-audit}

**Baseline:** branch `cas-gc-rebuild`, HEAD `ce312f547c3`. Six read-only auditors verified every
completion claim of the 2026-07-28 catalog plan, its SDD ledger, and the session handoff against
the actual tree and git history. Nothing was edited, built, or rerun; numeric test-pass claims are
recorded as UNVERIFIABLE-WITHOUT-RERUN. **This document is evidence and provenance only** — the
normative requirements for the remaining work live in
`docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`.

**Headline verdicts:**

- Tasks 0–5b, 9, 10d/e/f/g completion claims: **CONFIRMED** (all 57 named commits exist and are
  ancestors of HEAD; every spot-checked code obligation present; every "what died" concept absent).
- Tasks 7a and 7b not-implemented claims: **CONFIRMED**.
- "Task 6 NOT STARTED": **CONTRADICTED** — the namespace-file production paths and two of the
  three required tests are landed; the ref side is genuinely unstarted.
- "Task 6b not closed, rename pending": **CONTRADICTED** — the rename landed at `9d92c84ee37`
  under Task 6b's own required commit subject; the ordering tests are the open remainder.
- Task 7's three "carried residues": **already fixed in code**; the closure evidence (records,
  lane run, inventory) is what remains. Task 7b's `PENDING` double-count: **already fixed**
  (`8e9b06c2a81`).
- The old plan's critical-path line omits Task 6 before Task 7b, contradicting its own dependency
  column; the corrected join is normative in the new plan.

## Historical-unrecoverable items {#historical-unrecoverable}

Recorded once, here, so no later gate re-opens the search (the new plan's T8 forbids archaeology):

- **Task-1 review "minors 1–8" verbatim enumeration** — LOST. It lived in a reviewer session
  ledger that was not preserved. Searches performed at plan-writing (2026-08-02/03, this
  worktree): `grep -rn "minor" .superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/*.md`,
  the same over `docs/superpowers/worklogs/CURRENT.md`, and
  `git log --all -S "minors 1-8" -- .superpowers/sdd/` — the only surviving content is the
  characterization in the placement-sweep record (row 4): stale comment; `"spells"` →
  `"decodes to"`; IWYU; a report/table count mismatch; a naming-collision clause; a noted
  tension; minor 2 = the `listNamespaces` DDL-path ruling (later re-opened and resolved by Task
  1c's record-and-continue reversal); minor 8 = the bump-B verification (re-homed to Task 4,
  closed with the foundation). That characterization is the final walkable list.
- **Task-1 re-review NITs C–F** — LOST. Named in the placement-sweep record (row 5) and nowhere
  enumerated. Searches: `grep -rln "NIT" .superpowers/sdd/` and over `docs/superpowers/` found
  only the mentions, no content. Closed as historical-unrecoverable; MINOR-B from the same
  adjudication was discharged in Task 1c (second guard in `recoverRefTable` with an equivalence
  argument) and is verified, not lost.

The six reports follow verbatim (headings demoted one level; anchors added).

## Report: Tasks 5 and 5b {#report-t5}

## Audit — "Task 5 COMPLETE" / "Task 5b COMPLETE" {#t5-audit-task-5-complete-task-5b-complete}

Read-only audit at `cas-gc-rebuild`, HEAD `ce312f547c3`. Nothing was edited, staged, built or run.

**Overall verdict: both completion claims are CONFIRMED.** Every named commit exists and its diff
matches its claim; no checkbox in the Task 5 or Task 5b plan sections is unchecked; every spot-checked
code obligation is present in the tree; every named dead concept is absent from production code; the
one acknowledged deferred test really is absent. One PROSE finding (IMPRECISE), no CODE or TEST
findings.

---

### 1. Named commits {#t5-1-named-commits}

All thirteen exist and their diffs are consistent with the claims. Summary of what each actually
touches (from `git show --stat` plus key hunks):

| commit | claim | verdict |
|---|---|---|
| `c863cdd7fa60` | model LIST-independent recovery frontier | CONFIRMED — rewrites `CaRefDeltaIntakeCore.tla` + cfgs, adds `CaWriterEpochBackfillCore.tla` with five cfgs and a RESULTS file; retires `_fix_ckptwitness`, `_witness_corruptgap`, `_sab_skipquietprobe` and adds `_sab_skip_catalog_target`, `_sab_skip_held_retry` exactly as the plan's Task 5b file list demands |
| `357cf7b963f4` | production baseline: exact checkpoint frontier, no hint-derived history | CONFIRMED — touches both named recovery entry points (`Pool/CasRefLedger.cpp`, `Pool/CasRefProtocol.cpp`, −391/+ in the latter), `CasRefCkptFormat.*`, `CasRefCkpt.*`, `Gc/CasGc.cpp`, `Gc/CasOrphanManifestSweep.cpp`, `Tools/CasFsck.cpp`, plus `gtest_cas_recovery_grounding.cpp` and the format goldens |
| `3747975bbbf` | validate checkpoint snapshot base context | CONFIRMED — adds validation in `CasRefProtocol.cpp` with tests in `gtest_cas_gc_frontier_gate.cpp` and `gtest_cas_recovery_grounding.cpp` |
| `8183a1af1800` | retain checkpoint predecessor seal proof | CONFIRMED — `CasRefProtocol.{h,cpp}` + `CasGc.cpp`, tests in `gtest_cas_recovery_grounding.cpp` and `gtest_cas_ref_gc.cpp` |
| `e48b476d90f` | reject terminal gaps in writer recovery | CONFIRMED — `CasRefLedger.cpp` only, plus one new test `RefWriterRecovery.TerminalGapBelowCheckpointFrontierIsCorruptionNotSameLifeRebirth` |
| `4ab9b452e660` | make checkpoint-base fsck failures hard | CONFIRMED — `Tools/CasFsck.{h,cpp}` + 165 lines of `gtest_cas_fsck.cpp` |
| `60cbec2bd274` | remove LIST-derived fsck snapshot oracle | CONFIRMED — deletes `FsckClass::SnapshotOracleMismatch`, `FsckReport::snapshot_oracle_mismatches`/`snapshot_oracle_checked`, its `kFsckHardFindings` row (`static_assert` 6 → 5), and the `CommandFsck` throw and label |
| `7ac127b650a` | remove stale fsck oracle soak class | CONFIRMED — `utils/ca-soak/soak/fsck.py` only |
| `613faf8166e` | preserve predecessor proof in orphan sweep fixture | CONFIRMED — test-only, `gtest_cas_orphan_manifest_sweep.cpp` |
| `ee9e84d855e` | docs: close Task 5b recovery grounding | CONFIRMED — BACKLOG + plan only |
| `765c50b7cb93` | report post-fold terminal cleanup leaks | CONFIRMED — new `CasGcNamespaceCleanupLeaks` ProfileEvent, `CasNamespaceJanitor.{h,cpp}`, `CasGc.cpp`, plus a substantive 98-line regression test |
| `2769d788463` | docs: close Task 5 removal lifecycle | CONFIRMED — plan only |
| `a600c2e433c` | reads/removals of a never-opened table no longer mint | CONFIRMED — `CasRefLedger.{h,cpp}` with tests in `gtest_cas_pool.cpp` and `gtest_cas_namespace_file_request_profile.cpp`; matches the plan's "Step 1 LANDED" attribution |

The merge commit `78cf06456d3` exists and its message itself records the 1930/1930 combined gate.

### 2. Plan checkboxes {#t5-2-plan-checkboxes}

`awk` over plan lines 1331–2040 (the whole `### Task 5` + `### Task 5b` span through the start of
`### Task 6`) finds **zero `- [ ]` entries**. Every box in both sections is `- [x]`. No checkbox
contradicts the completion claim. Both sections carry an explicit
`**Execution status, 2026-08-02: COMPLETE.**` paragraph naming the same commits as the handoff.

### 3. Code obligations at HEAD {#t5-3-code-obligations-at-head}

- **Perpetual dead-life janitor — PRESENT.** `NamespaceJanitor::runOnePage`
  (`Gc/CasNamespaceJanitor.h`), invoked from `Gc::runNamespaceJanitorPage`, which is called from two
  sites in `Gc::runRegularRound` — the deferred path with `suppress_destructive=true` and the normal
  path — i.e. every round, not only on removal. It is bounded (`page_budget`), fence-gated against
  `gc/state` lease owner+seq, and leak-only.
- **`Removing → absent` CAS deletion transition — PRESENT.**
  `CasRefCatalog::deleteCompletedRemoving` / `deleteCompletedRemovingAtSnapshot`. Both refuse unless
  the exact row is `Removing` with `removal_started_round`, the adopted parent seal's `ref_lives` row
  for that incarnation has `cleanup_evidence`, and `coverage.hold` is false — exactly the three
  conditions the plan states. Invoked only from `CatalogLifecycleReconciler::reconcile`, which is the
  pre-fold drain and loops to `DrainComplete` / `FencedOut`, throwing retry-later if the exact row
  survives.
- **LIST-derived fsck snapshot oracle — GONE.** `git grep` for `SnapshotOracleMismatch`,
  `snapshot_oracle_mismatch(es)`, `snapshot_oracle_checked`, `snapshot-oracle-mismatch` over the whole
  tree returns **only** historical documents (`docs/superpowers/cas/BACKLOG.md`, the Stage-A RESULTS,
  two dated to-do files, and the 2026-07-26 report artifacts). No hit in `src/`, `programs/`,
  `tests/` or `utils/`.
- **Recovery uses the exact checkpoint frontier, no hint-derived history — CONFIRMED.**
  `chooseRecoveryGrounding` exists in `Pool/CasRefCkpt.{h,cpp}` and is the single policy boundary used
  by all four consumers: `CasRefLedger.cpp` (mounted writer walk), `CasRefProtocol.cpp`
  (`recoverRefTableDetailed`), `CasGc.cpp`, and `Tools/CasFsck.cpp`. `hint_log_ids` has **zero**
  occurrences tree-wide. `committed_through` is present in the checkpoint format and threaded through
  `CasGc.cpp`, `CasOrphanManifestSweep.cpp`, `CasRefCatalog.cpp`, `CasRefCkpt.*`, `CasRefLedger.cpp`.
  Neither `CasRefProtocol.cpp` nor `CasRefLedger.cpp` contains any call to `Backend::list` — the only
  remaining `.list(` consumers under the CA subsystem are GC (`CasGc.cpp`), the janitor and the orphan
  manifest sweep (discovery), plus unrelated pool/root/probe code. `Tools/CasFsck.cpp` enumerates only
  `blobsPrefix` and manifest prefixes, never the ref stream.

### 4. "What died — do not re-add" (the old plan's `t5-died` list) {#t5-what-died}

Checked against production code under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`:

| dead concept | grep | verdict |
|---|---|---|
| `RemovalReady` | 0 hits tree-wide | absent |
| independent name-keyed cursors / cleanup maps (`per_ns_shard`, `ns_cleanup_items`) | 0 hits tree-wide | absent |
| fictitious ref shard zero (`"<namespace>/0"`, `shard_zero`, `shardZero`) | 0 hits | absent |
| permanent `Retired` catalog row | the only `Retired*` symbols are blob-GC `RetiredEntry`/`RetiredMergeResult` in `CasBlobInDegree.cpp`, an unrelated subsystem | absent |
| `_cleanup`-driven `Pending → Completed` handshake / one-shot removal cleanup pass | 0 hits for `_cleanup` key, `NsCleanup`, `cleanup_state` | absent; replaced by `RefLifeFoldState::cleanup_evidence` on the life row, as designed |
| Σ-index-set exactness / `nsc` cursor grammar | no residue | absent |

Prerequisite artifacts the Task 5 file list promised are present: `Gc/CatalogLifecycleReconciler.{h,cpp}`,
`Formats/CasGcMaintenanceStateFormat.{h,cpp}`, `FormatId::GcMaintenanceState = 25` in `CasFormat.h`,
`gtest_cas_gc_maintenance_state_format.cpp`, `gtest_cas_recovery_grounding.cpp`.

Note, not a finding: `CasRefCatalog::resolveLifeOrSentinel` still exists and is still used (production
`CasGc.cpp` comments plus tests). Deleting it is Task 6's stated obligation, and both the plan and the
handoff say so explicitly — it does not contradict Task 5/5b closure.

### 5. The acknowledged deferred item {#t5-5-the-acknowledged-deferred-item}

**CONFIRMED still absent — the debt record is accurate.** The exception branch in
`CasRefCatalog::deleteCompletedRemovingAtSnapshot` is the `attempt_failure` path: `backend.casPut` on
the catalog key throws, the mandatory resolution read follows, and the stored exception is rethrown
only if the exact old row survived (otherwise the outcome is `Deleted`/`EntryChanged`). No test injects
a throwing `casPut` on `refCatalogKey`. The only two files calling `deleteCompletedRemoving` are
`gtest_cas_ref_catalog.cpp` and `gtest_cas_ns_file_read_contract.cpp`, and neither uses a
fault-injecting backend for that key — the only `casPut`-throwing helper in `cas_test_helpers.h`
throws exclusively on keys ending in `.meta`.

What does exist, and is adjacent but not the same branch:
`CasRefCatalogRemoval.NonFenceAuthorityExceptionPropagatesBeforeEraseCas` and
`...PropagatesAfterEraseResolution` cover the *fence-check* callback throwing before and after the
erase — the pre-CAS one asserts zero `casPut`s, the post-CAS one asserts the erase became durable and
the original error still wins. So the branch's *sibling* is covered symmetrically; the CAS-throw
branch itself is not. That is precisely what the ledger says.

### 6. Findings {#t5-6-findings}

**F1 — PROSE, IMPRECISE.** The Task 5 **Files** list promises
`Create: src/Disks/tests/gtest_cas_ns_removal_lifecycle.cpp`. That file does not exist at HEAD and
`git log --all --diff-filter=A` shows it was **never** created on any branch. The removal-lifecycle
tests landed instead in `gtest_cas_ref_catalog.cpp` (the `CasRefCatalogRemoval` suite) and
`gtest_cas_gc_frontier_gate.cpp`. The file list is not a checkbox, so this does not contradict
"COMPLETE" — the obligation was met at a different address — but a reader using the Files list as an
inventory will look for a file that never existed. Batch into
`docs/superpowers/cas/deferred-docs-fixes.md`.

No CODE findings. No TEST findings.

### 7. Test-quality checks performed {#t5-7-test-quality-checks-performed}

- Sanitizer sweep `grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"` over the
  substantively-touched test files (`gtest_cas_recovery_grounding.cpp`, `gtest_cas_ref_writer.cpp`,
  `gtest_cas_gc_frontier_gate.cpp`, `gtest_cas_ref_gc.cpp`, `gtest_cas_orphan_manifest_sweep.cpp`,
  `gtest_cas_pool.cpp`, `gtest_cas_fsck.cpp`, `gtest_cas_ref_catalog.cpp`,
  `gtest_cas_namespace_file_request_profile.cpp`). Every bare `EXPECT_THROW`/`EXPECT_ANY_THROW` added
  by these commits resolves to a `CORRUPTED_DATA` throw site, not `LOGICAL_ERROR`:
  `readCheckpointSnapshotBase` throws `CORRUPTED_DATA` at all eight of its sites, and
  `sweepManifestCursorPage` likewise. The `LOGICAL_ERROR` uses in `gtest_cas_ref_catalog.cpp` are the
  pre-existing encoder-grammar block, which already carries its own death-test split and an explicit
  comment saying so. No new abort-in-sanitizer hazard.
- "Would it fail if the behaviour regressed?" applied to `765c50b7cb93`'s new test
  `CasGcFrontierGate.PostFoldUnreadableTerminalIsCountedWithoutSuppressingProgress`: yes. It asserts
  the phase metric `leaked == 1`, a ProfileEvent delta of exactly 1, that the captured `CasGc` warning
  contains the unreadable key and the word `leak`, that the catalog row was still removed, that an
  unrelated manifest was still deleted (`manifests_deleted == 1`), and that the later dead residue was
  still deleted despite the injected `head` fault. Deleting the per-key leak handling breaks at least
  three of those. Not vacuous.
- The claimed model evidence exists in the tree and matches: `CaWriterEpochBackfillCore_RESULTS.md`
  records all five expectations met with per-cfg state counts, and
  `CaRefDeltaIntakeCore_RESULTS.md` records "Exit 0, all 15 expectations met".

### 8. UNVERIFIABLE-WITHOUT-RERUN {#t5-8-unverifiable-without-rerun}

The numeric gate claims are recorded in the plan, the ledger and (for 1930/1930) the merge commit
message, but cannot be checked by inspection: branch gate 1929/1929 with 2 disabled; post-merge
1930/1930; CA-S3 selectors 3/3; focused residual gate 21/21. Not rerun, per the read-only dispatch.

## Report: Tasks 6 and 6b {#report-t6}

## Stage B baseline audit — Task 6 and Task 6b {#t6-stage-b-baseline-audit-task-6-and-task-6b}

Read-only audit. Repo `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`,
HEAD `ce312f547c3`. Nothing was edited, staged, built or run. All citations are by symbol.

**Headline.** "Task 6 NOT STARTED" is only true of its *ref* half. The *namespace-file* half is
substantially implemented and committed already: the delayed-writer defect the brief tells Task 6 to fix
is already fixed, `namespaceFilesReadable` is already deleted, every namespace-file API already takes a
`NamespaceLifeId`, and two of the three required namespace-file tests already exist under different
names. Three of the handoff's eight verifiable claims are CONTRADICTED by the tree.

---

### 1. `CasRefCatalog::resolveLifeOrSentinel` — CONFIRMED present, with a material qualification {#t6-1-casrefcatalog-resolvelifeorsentinel-confirmed-present-with}

**CODE.** The function exists and still has the sentinel fallback. Declaration in
`CasRefCatalog.h` (`static NamespaceLifeId resolveLifeOrSentinel(Backend &, const Layout &, const RootNamespace &)`);
definition in `CasRefCatalog.cpp`, whose whole body is:

```cpp
if (auto cataloged = lifeIfCataloged(backend, layout, ns))
    return *cataloged;
return NamespaceLifeId::stageATransition(ns);
```

**Qualification that changes the size of the task: it has ZERO production callers.**
17 call sites, every one of them under `src/Disks/tests/`:

| File | call sites |
|---|---|
| `src/Disks/tests/gtest_cas_gc_frontier_gate.cpp` | 6 |
| `src/Disks/tests/gtest_cas_ref_writer.cpp` | 3 |
| `src/Disks/tests/cas_test_helpers.h` | 2 (+1 in a comment) |
| `src/Disks/tests/gtest_cas_gc_fold.cpp` | 1 |
| `src/Disks/tests/gtest_cas_ref_chunked_flush.cpp` | 1 |
| `src/Disks/tests/gtest_cas_holey_list_detector.cpp` | 1 |
| `src/Disks/tests/gtest_cas_ns_file_incarnation.cpp` | 1 |
| `src/Disks/tests/gtest_cas_ref_contiguous_alloc.cpp` | 1 |

The only two non-test mentions outside `CasRefCatalog` itself are *comments*: one in `CasGc.h`
(inside the `FoldResult` catalog-cut documentation) and one in `Gc::foldRefTables`' region of
`CasGc.cpp`, both warning that a re-resolve here would see a different snapshot.

**Consequence for the plan.** The plan's Task-6 bullet says to "return `std::optional`, delete the
fallback, and let the compiler enumerate the sites." The compiler will enumerate **only test sites**.
The plan's stated motivation — that `resolveLifeOrSentinel` "calls `CasRefCatalog::read` … on every
call, at 24 sites, several inside per-namespace loops" — no longer describes production and should
not be quoted as live justification. Grade: **PROSE, IMPRECISE** (was true when written; the
production sites were migrated by Task 4-C/5 work).

**PROSE finding.** `CasRefCatalog.h`'s doc comment on `resolveLifeOrSentinel` still describes it as
serving "non-production discovery-path readers — `recoverRefTableDetailed`, fsck's exact stream walk,
`CasOrphanManifestSweep`'s active-key set". None of those three call it any more. Grade: **FALSE** as
a statement about current callers.

---

### 2. `stageATransition` — CONFIRMED present; the production surface is one line {#t6-2-stageatransition-confirmed-present-the-production-surface}

**CODE.** 295 hits across 42 files under `src/`. Classification:

- **Production call sites: 1.** `CasRefCatalog.cpp`, the fallback `return` inside
  `resolveLifeOrSentinel` quoted above — itself unreachable from production because nothing in
  production calls that function.
- **Production declaration: 1.** `NamespaceLifeId::stageATransition` in `CasNamespaceLifeId.h`.
- **Production comments: 2.** `CasNamespaceLifeId.h` ("`Task 6` DELETES it, gated on a tree-wide grep
  for `stageATransition`") and `CasRefCatalog.h` ("the deterministic Stage-A fixture identity").
- **Test hits: 291**, across 39 files. Largest concentrations: `gtest_cas_ref_writer.cpp` (38),
  `gtest_cas_ref_wedge_every_attempt.cpp` (24), `gtest_cas_pool.cpp` (22), `gtest_cas_ref_ckpt.cpp`
  (20), `gtest_cas_ref_recovery_cas_walk.cpp` (16), `gtest_cas_gc_hold_grammar.cpp` (15),
  `gtest_cas_ref_install_safety.cpp` (13), `gtest_cas_layout.cpp` (12),
  `gtest_cas_recovery_streaming.cpp` (11), `gtest_cas_fence_generation.cpp` (11),
  `gtest_cas_inspect.cpp` (10). `cas_test_helpers.h` holds 5.

So the "eliminate production `stageATransition`" obligation is one deleted line plus the
declaration; the real cost is the ~291 test-side migration onto the named fixture seam, which is the
plan's third bullet. Task 6 planning that budgets these as one item will misprice it.

---

### 3. `ContentAddressedMetadataStorage::namespaceFilesReadable` — **CONTRADICTED: it does not exist** {#t6-3-contentaddressedmetadatastorage-namespacefilesreadable-con}

**CODE.** `grep -rn "namespaceFilesReadable" src/` returns **zero** hits. It was deleted in
`827bc0a9189` ("ca: ref — namespace files keyed by incarnation; rebirth waits for no file").

What stands in its place is already the shape Task 6 asks for — an *optional life*, not a boolean gate:

- `Pool::namespaceFilesLifeIfReadable(const RootNamespace &) -> std::optional<NamespaceLifeId>`
  (`CasPool.h`/`CasPool.cpp`), forwarding to `CasRefLedger::namespaceFilesLifeIfReadable`.
- `ContentAddressedMetadataStorage::readableNamespaceFilesLife(const Cas::RootNamespace &) const
  -> std::optional<Cas::NamespaceLifeId>` (`ContentAddressedMetadataStorage.h`/`.cpp`), a thin wrapper.

Its callers are **eleven**, not five — six in `ContentAddressedMetadataStorage.cpp` (in `existsFile`,
`existsDirectory`, the two `listDirectory` arms, `tryGetInManifestBytes`, and one more) and five in
`ContentAddressedTransaction.cpp` (the readable-resolution seam, the rename/move `from_ns` and
`src_ns`/`dst_ns` arms, and the removal path).

**Verdict: CONTRADICTED.** The handoff's §5 instruction "Delete
`ContentAddressedMetadataStorage::namespaceFilesReadable` and rewire its five current callers" is
already done and must be struck from Task 6's scope; re-dispatching it will send an implementer
hunting a symbol that is not there.

---

### 4. Test files {#t6-4-test-files}

**TEST, CONFIRMED:** `src/Disks/tests/gtest_cas_ref_read_contract.cpp` is **absent**.

**TEST, CONFIRMED present:** `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp` exists (committed;
`git status` clean; landed via `3b952c6cbde`, touched by `4048163f0dd` and `224aacd8eb9`). It contains
exactly two tests, both in suite `CasNamespaceFileReadContract`:

1. `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`
2. `DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes`

Mapping onto the three required Task-6 namespace-file tests:

| Required name | Status |
|---|---|
| `StaleReaderAfterRebirthNeverSeesNewIncarnation` | **Present in substance, different name** — `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`. Same relative name written in both lives with different bytes; asserts the held read is never `"life-2\n"`, is `"life-1\n"` or absent, and that `getNamespaceFile(life1, …)` still returns life 1's bytes. |
| `DelayedWriterFinalizedAfterRebirthWritesOnlyItsOwnIncarnation` | **Present in substance, different name** — `DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes`. Opens a real `CaInlineWriteBuffer` under life 1, admits life 2, finalizes; asserts life 2's head token and body are byte-identical after, and that a successful finalize landed under life 1. Non-vacuity is handled: it accepts a typed stale failure but then asserts the life-1 bytes only in the non-failure branch. |
| `NamespaceFileHotPathsIssueZeroCatalogRequests` | **MISSING from this file.** Partially covered elsewhere — see §6. |

Both existing tests use production lifecycle machinery (`CasRefCatalog::casUpdate` →
`Removing`, `CasRefCatalog::deleteCompletedRemoving` with cleanup evidence and a held fence, then
`casAdmitEntry` for life 2), so they are not sentinel-fixture artefacts.

**Naming decision needed, not a defect:** the plan and handoff name three tests that do not exist by
those spellings while two equivalents do. Task 6 must either rename or explicitly record the
equivalence; silently leaving both spellings in circulation is how a later gate "finds" the tests
missing and re-writes them.

---

### 5. The delayed `CaInlineWriteBuffer` callback — **CONTRADICTED: already captures the life** {#t6-5-the-delayed-cainlinewritebuffer-callback-contradicted-alre}

**CODE.** In `ContentAddressedTransaction::writeFile`, the non-part / table-file branch:

```cpp
/// The LIFE is resolved once, here at buffer-open time, and captured by value below, so a
/// finalize that runs later writes to the incarnation this open was admitted under -- never
/// into whatever life the namespace name happens to denote when the callback fires
/// (directive §namespace-file-requirements).
const Cas::NamespaceLifeId life
    = metadata_storage.store()->namespaceLife(metadata_storage.liveNamespace(tf->table_uuid));
const std::string name = tf->tail;
std::string prefix_bytes;
if (mode == WriteMode::Append)
    if (auto existing = metadata_storage.store()->getNamespaceFile(life, name))
        prefix_bytes = std::move(*existing);
return std::make_unique<Cas::CaInlineWriteBuffer>(
    [this, life, name, carried = std::move(prefix_bytes)](std::string bytes)
    {
        metadata_storage.store()->putNamespaceFile(life, name, carried + bytes);
    });
```

The lambda captures `life` (a `Cas::NamespaceLifeId`), by value, and calls
`putNamespaceFile(life, …)`. It does **not** capture a bare `RootNamespace`, and it does not
re-resolve at finalize. The plan's description of the defect ("captures `[this, ns, name, carried = …]`
… and calls `putNamespaceFile(ns, name, carried + bytes)` LATER") is stale.

The sibling mountpoint branch captures a precomputed `key` string, which is likewise life-free by
construction.

**Verdict: CONTRADICTED.** Handoff §6 and the plan's corresponding bullet describe work already
done, and the test that would catch a regression
(`DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes`) is already in the tree. Grade for the plan
text: **PROSE, FALSE.**

---

### 6. Namespace-file APIs and hot-path catalog GETs {#t6-6-namespace-file-apis-and-hot-path-catalog-gets}

**CODE, CONFIRMED (Task 1c/4b surface complete).** Every namespace-file API takes the life explicitly,
at all three layers:

- `CasPlainObjects::putNamespaceFile / getNamespaceFile / listNamespaceFiles / removeNamespaceFile`,
  each `(const NamespaceLifeId & life, …)`; the list path derives its prefix from
  `layout.namespaceFilesPrefix(life)`.
- `Pool::putNamespaceFile / getNamespaceFile / listNamespaceFiles / removeNamespaceFile`, same shape.
- `Layout::namespaceFileKey(life, file_name)` / `namespaceFilesPrefix(life)` — there is no
  namespace-only key helper left to misuse.

**TEST — hot-path zero-catalog is pinned, with one gap.** `gtest_cas_namespace_file_request_profile.cpp`
holds `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey`,
which resets a recording object storage *after* table open and then asserts nothing touches any of
four forbidden key families — `layout.refCatalogKey()` ("no catalog request"),
`layout.casRefsPrefix()`, `layout.blobsPrefix()`, `layout.casManifestsPrefix()`. It carries a positive
control (`writtenContaining("/_files/")` must be non-empty), so the four empty answers are not a dead
recorder. Its companion `TheLifeResolutionIsPaidOncePerTableOpen` proves the resolution is real and
paid once, closing the "the zeros come from a fixture where no resolution happened" hole.

**Gap (TEST).** The operations exercised are rewrite, append (read-modify-rewrite), read, a second
segment create, and `unlinkFile`. **`listDirectory` / `listNamespaceFiles` is not among them.** The
required test spelling is "read, write, remove, **and list** through a held life"; list is the one arm
whose zero-catalog property is currently unasserted. This is the concrete missing coverage for
required test 3, and it is a real gap rather than a naming quibble.

**Ref side — genuinely not started.** There is no life-handle type: `NamespaceLifeId::fromLiveHandle`,
which `CasNamespaceLifeId.h` itself says "Stage B Task 6 adds", does not exist anywhere in the tree.
`CasRefLedger.cpp` still performs `CasRefCatalog::read` at ten sites (birth/reconcile/publish-admission/
recovery/janitor paths). No test asserts a *ref* read issues zero catalog requests, and there is no
ref-side rebirth-alias reader test. Required ref/read tests 1 and 2 are **missing**.

**Ref-side destructive cleanup — already done.** Required tests 3 and 4 exist, in
`gtest_cas_ref_gc.cpp`, suite `CasRefGcCleanupAuthority`:
`CatalogTokenMoveBeforeFirstDeleteRefusesEveryRefObjectDelete`,
`CatalogTokenMoveBetweenKeysAllowsFirstAndRefusesSecondDelete`, plus the GC-fence twins
`GcFenceMoveBeforeFirstDeleteRefusesEveryRefObjectDelete` and
`GcFenceMoveBetweenKeysAllowsFirstAndRefusesSecondDelete`. Their harness
(`RefCleanupAuthorityRaceBackend`) moves one authority at a precise delete boundary while leaving the
target object's own token untouched, so only an authority check can refuse — the fault genuinely fires.
`Gc::cleanupRefObjects` correspondingly reads its entry from `folded.catalog_cut` rather than
re-resolving.

---

### 7. Task 6b baseline {#t6-7-task-6b-baseline}

**CODE, CONFIRMED — the target spelling is already in the tree**, at four places:

- `CasRefLedger.h`: `bool tryPublishSnapshotAndAdvanceCheckpointOnce(const RootNamespace & ns);`
- `CasRefLedger.cpp`: the definition, plus the runtime-scoped
  `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` it delegates to, called from
  `dispatchSnapshotPublisher`.
- `CasPool.h` / `CasPool.cpp`: the public forwarder.

The **rename was not split**: the two durable effects stay in one retry unit, which is the constraint
the plan attached to the decision. That satisfies the plan's "RENAME, not a split … unless the
implementer finds a split that keeps both effects in ONE retry unit".

**PROSE, IMPRECISE.** The plan's Task 6b section heading is still `### Task 6b: trySnapshotPublishOnce
says what it does`, and its **Files** block cites line numbers (`CasRefLedger.h:190`,
`CasRefLedger.cpp:3440`, `:3242`, `CasPool.h:540`, `CasPool.cpp:1564`) that no longer point at the
symbol. Reading the section top-down leaves an implementer expecting the old name.

#### Existing ordering coverage — inventory {#t6-existing-ordering-coverage-inventory}

All in `gtest_cas_ref_ckpt.cpp` unless noted.

| Test | What it actually pins |
|---|---|
| `ACommittedSnapshotPublishAdvancesTheCheckpoint` | *Post-hoc* state: the `_ckpt`'s `checkpoint_snapshot_id` equals the published id, `life_epoch` is preserved by the merge, and the named body key exists. No order is observed. |
| `CleanupPlannedBetweenTheBodyPutAndTheCkptCasCannotDeleteTheNewSnapshot` | The *consequence* of body-before-ckpt: a stale checkpoint can only under-clean. But it constructs the stale reading from two **sequential completed publishes**, not from inside the body-PUT/`_ckpt` window, so it does not observe the two writes' relative order. |
| `TheCheckpointIsWrittenOncePerPublicationAndNotOnIdleAttempts` | Write *count* on `_ckpt`, not order. |
| `APublishFencedOutMidAttemptDoesNotAdvanceTheCheckpoint` | Neither `_ckpt` advance nor in-memory adoption when the incarnation is replaced mid-attempt — the closest existing thing to an adoption assertion, but it is the negative case only. |
| `NeedsRecoveryReplaysBeforeCheckpointAdvance` | Recovery replay precedes the advance. Ordering, but of a different pair. |
| `SnapshotPublisherRefusesEpochSealCandidateWithoutAnyWrite` | Refusal, not ordering. |
| `SnapshotsAreDeletableStrictlyBelowTheCheckpoint` | The GC gate's arithmetic. |

Public-surface callers of the method also appear in `gtest_cas_ref_contiguous_alloc.cpp`,
`gtest_cas_ref_writer.cpp` (including two concurrency tests with racing publisher threads) and
`gtest_cas_writer_duties.cpp` — so a rename would not have dropped coverage, but none of these assert
the three-effect order either.

#### Missing Task-6b coverage {#t6-missing-task-6b-coverage}

`src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp` does not exist. Of the four tests the
plan names:

1. `SnapshotBodyIsDurableBeforeCheckpointAdvances` — **MISSING.** Nothing asserts the backend op-journal
   order of the body `PUT` versus the `_ckpt` CAS. The nearest test reasons *about* that order without
   observing it.
2. `AdoptionHappensLastAndOnlyAfterBothDurableEffects` — **MISSING.** No positive assertion that
   in-memory adoption (`newest_snapshot_id` / `base_snapshot_bytes` under `state_mutex`) follows both
   durable effects; only the fenced-out negative exists.
3. `PoisonedRefusesPublicationAndTriggersReRecovery` — **MISSING, and the whole predicate is untested.**
   `grep -rn "Poisoned" src/Disks/tests/` yields nothing relevant; the only two `Poison*` test names in
   the tree (`CasRefStateMachine.E3TrustedReplayPoisonOnBadTailIsInternal`,
   `CasRefCatalogLifeIndex.DuplicatePhysicalIdsAreAmbiguousWithoutPoisoningUniquePointResolution`) are
   about other mechanisms. Constraint 17's publication block is a bare uncovered refusal.
4. `RetryBackoffUnchangedAcrossRename` — **MISSING.** `admitSnapshotPublishUnderStateLock`,
   `advancePublishBackoff` and `resetPublishBackoff` have **zero** references anywhere under
   `src/Disks/tests/`. Note the trap: the `CasRequestControllerBackoff` family in
   `gtest_cas_request_control.cpp` is the *request controller's* retry backoff, an unrelated mechanism —
   it must not be mistaken for coverage of the publish backoff.

Because the rename has already landed, test 4's literal premise ("capture the before-values as
literals … identical before and after") is retired: there is no pre-rename tree to capture from. The
durable obligation behind it — that the admission gate's decisions for a fixed clock sequence are
pinned at all — is unmet and should be written as a plain characterization test rather than a
differential one.

---

### 8. The scratch artifact {#t6-8-the-scratch-artifact}

`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task6-ns-file-contract-report.md`

**It does contradict "Task 6 not started", for the namespace-file half specifically.** It documents a
completed red-first cycle: two temporary mutations (re-resolving the namespace on every read in
`readableNamespaceFilesLife`; capturing the name and calling `resolveLifeOrSentinel` at finalize),
both tests failing non-vacuously under those mutations with the exact wrong bytes named
(`life-2\n`, and life 2's token changing), the mutations reverted against recorded blob hashes, and a
GREEN ASan run. Its claims match the tree: the two tests exist and are committed, and the production
code has the non-mutated shape.

Two prose defects in it:

- **PROSE, FALSE.** It names the first test
  `StaleReaderAfterSameNameRebirthNeverSeesSuccessorBytes`; the committed test is
  `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`. A reviewer grepping the reported name finds
  nothing.
- **PROSE, IMPRECISE.** "Added `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp`" — `git log` on
  that path shows it landed in `3b952c6cbde` and was subsequently modified by `4048163f0dd` and
  `224aacd8eb9`, so the file's provenance is broader than the report implies.

The handoff's instruction that this artifact "does not override the ledger and is not evidence that
Task 6 started or completed" is right about the *ledger*, but wrong as a reason to ignore it: the
tree independently confirms the work it describes. The ledger is the stale document here, not the
report.

---

### Additional finding, outside the eight claims {#t6-additional-finding-outside-the-eight-claims}

**CODE (comment policy).** Production CA sources carry 76 comment references to internal artefacts
(`Task N`, `Review C3`, `finding N`) — e.g. `Gc::cleanupRefObjects` opens with "Review C3: look up the
SAME complete cut…", `NamespaceLifeId` carries "Stage B Task 6 adds the second permanent factory" and
"`Task 6` DELETES it", `CasRefCatalog::resolveLifeOrSentinel`'s doc opens "Stage B (Task 4-C)". These
point at documents that get deleted from the branch. Task 14 nominally owns this sweep; recording it
here because Task 6 will rewrite several of the worst offenders anyway and the reason should be kept
while the citation is dropped (e.g. "look up the same cut the round's walk resolved; a fresh read
could delete a successor's objects using predecessor bounds" needs no "Review C3").

---

### Verdict table {#t6-verdict-table}

| # | Claim | Verdict |
|---|---|---|
| 1 | `resolveLifeOrSentinel` exists with sentinel fallback | **CONFIRMED** — but zero production callers; all 17 sites are tests |
| 2 | `stageATransition` tree-wide state | **CONFIRMED** — 295 hits / 42 files; production = 1 call + 1 decl + 2 comments; 291 test hits |
| 3 | `namespaceFilesReadable` exists with five callers | **CONTRADICTED** — deleted in `827bc0a9189`; replaced by optional-returning `namespaceFilesLifeIfReadable` / `readableNamespaceFilesLife` with 11 callers |
| 4a | `gtest_cas_ref_read_contract.cpp` absent | **CONFIRMED** |
| 4b | `gtest_cas_ns_file_read_contract.cpp` exists | **CONFIRMED** — 2 tests; required 1 and 2 present under other names; required 3 missing |
| 5 | Delayed callback captures `RootNamespace` and re-resolves | **CONTRADICTED** — captures `NamespaceLifeId life` by value |
| 6a | Namespace-file APIs accept `NamespaceLifeId` | **CONFIRMED** at all three layers |
| 6b | Namespace-file hot paths issue catalog GETs | **CONTRADICTED** — pinned at zero, with a positive control; gap is `list` only |
| 6c | Ref hot paths hold a life handle | **CONFIRMED NOT STARTED** — no `fromLiveHandle`; 10 `CasRefCatalog::read` sites in `CasRefLedger.cpp` |
| 7 | `tryPublishSnapshotAndAdvanceCheckpointOnce` spelling present | **CONFIRMED**, 4 sites, not split; plan heading and line numbers stale (PROSE) |
| 8 | Scratch artifact contradicts "not started" | **CONTRADICTED (the handoff)** — it does contradict it, and the tree corroborates it |

### Missing-coverage inventory {#t6-missing-coverage-inventory}

**Task 6:**
1. `NamespaceFileHotPathsIssueZeroCatalogRequests` — **list arm only**; read/write/remove/append/rotation
   already pinned by `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey`.
2. Ref-side rebirth-alias reader test (required ref test 1) — missing entirely.
3. Ref-side zero-catalog-GET hot-read test (required ref test 2) — missing entirely.
4. Required ref tests 3 and 4 (token move before first delete / between two keys) — **already covered**
   by `CasRefGcCleanupAuthority`, non-vacuously; do not rewrite.
5. `NamespaceLifeId::fromLiveHandle` and the ref life-handle plumbing — absent; this is the real
   remaining body of Task 6.
6. Test-side `stageATransition` migration onto one named fixture seam — 291 hits, 39 files.

**Task 6b:** all four named tests missing; the highest-value two are
`PoisonedRefusesPublicationAndTriggersReRecovery` (an entirely untested refusal predicate) and a
publish-backoff characterization test (`admitSnapshotPublishUnderStateLock` /
`advancePublishBackoff` / `resetPublishBackoff` have zero test references, and the similarly-named
`CasRequestControllerBackoff` family is a different mechanism).

## Report: Tasks 7, 7a, 7b {#report-t7}

## Stage B audit — Tasks 7, 7a, 7b {#t7-stage-b-audit-tasks-7-7a-7b}

Read-only audit. Repo `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`,
HEAD `ce312f547c3`. Nothing was edited, staged, committed, built or run. All citations are by
SYMBOL; line numbers appear only where a comment/marker has no symbol to name.

Labels: **CODE** / **TEST** findings open a fix round; **PROSE** findings (plan text, comments,
ledger prose) are batched into `docs/superpowers/cas/deferred-docs-fixes.md` and do NOT.

---

### Headline verdicts {#t7-headline-verdicts}

| Claim under audit | Verdict |
|---|---|
| Task 7 implementation present in `224aacd8eb9`, closure evidence open | **CONFIRMED** |
| Task 7 named residues (assertion + two parser residues) still open | **CONTRADICTED** — all three are discharged in code; only the *recorded inventory* is missing |
| Task 7a NOT IMPLEMENTED | **CONFIRMED** — every artefact still live |
| Task 7b NOT IMPLEMENTED (`kDefault` non-authoritative) | **CONFIRMED** |
| Task 7b still owes the `PENDING` double-count fix | **CONTRADICTED** — fixed 2026-07-30 in `8e9b06c2a81`; BACKLOG marks it RESOLVED |
| Task 7b has "five Stage-A return items" / "three `broken_tests.yaml` entries" | **CONTRADICTED** — 8 marker files, 4 yaml entries, plus a whole second marker class the exit grep cannot see |

New findings this pass: 1 TEST, 1 CODE(comment), 5 PROSE. Details below.

---

### Task 7 — R5 decommission duties {#t7-task-7-r5-decommission-duties}

#### Commit exists and contains the implementation — CONFIRMED {#t7-commit-exists-and-contains-the-implementation-confirmed}

`224aacd8eb96f6da6e0b2457dad1b09ac563d3d5`, "ca: close namespace removal and decommission duties",
2026-08-02, 26 files, +857/−243. It contains, in `Tools/CasDecommission.cpp`:

- the `_ckpt`-presence gate on a `Removing` row (`head(layout().refCkptKey(life))`, throws
  `CORRUPTED_DATA` when absent);
- the resumption call `admin->dropNamespace(life)` on a `Removing` row, with the comment stating
  catalog deletion stays GC's job;
- the `request_gc_round` callback parameter plus a `SCOPE_EXIT` that fires it whenever
  `gc_round_needed` was set — so partial progress still wakes GC on a fail-closed exit;
- the retirement-tail ownership check (`retirement_catalog_cut` + `victim_still_owned`) and a second
  token/value comparison against `fresh_retirement_catalog`.

And `src/Disks/tests/gtest_cas_decommission_catalog_duties.cpp` (new, 263 lines, 6 tests):
`RemovingWithoutCheckpointIsCorruptionAndKeepsSlot`,
`RemovingWithCheckpointResumesTerminalAndKeepsSlotForGc`,
`PartialRemovalProgressStillWakesGcWhenLaterNamespaceFails`,
`FinalCatalogFenceKeepsSlotWhenVictimEntryAppearsDuringDrain`,
`FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound`,
`OpaqueLifeDebrisWithoutCatalogOwnershipDoesNotBlockRetirement`.

Caveat worth recording: the commit is **not** a Task-7-scoped commit. Its subject is not the
plan-mandated `ca: decommission — catalog-exact duties; retirement fenced on owned entries`, and it
mixes in the Task 5 `RefTableSnapshot` Live-only DTO conversion across `CasRefSnapshotFormat`,
`CasRefProtocol`, `CasRefLedger`, `CasInspect` and eight test files. That is why the closure
evidence is hard to recover: there is no Task-7 diff to point at.

#### Plan checkboxes — CONFIRMED unchecked {#t7-plan-checkboxes-confirmed-unchecked}

All three Task 7 steps (`Step 1: Failing tests`, `Step 2/3/4`, `Step 5: Commit`) and both carried
residue bullets are `- [ ]` in `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md`
under `### Task 7`. The ledger
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/progress.md` reads
"Task 7: IMPLEMENTATION PRESENT, CLOSURE EVIDENCE OPEN."

#### Red-first evidence — GENUINELY MISSING {#t7-red-first-evidence-genuinely-missing}

Searched `.superpowers/sdd/` (both the Stage-B dir and the parent), `docs/superpowers/`, and `tmp/`.
The only artefacts naming `224aacd8eb9` are:
`tmp/step9_c_224aacd.review.patch` and `tmp/step9_c_head_after_foreign.txt` (Task 5 step-9c review
scaffolding, not a Task-7 RED record), plus the plan/ledger status prose itself. There is
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task7-9-ready-audit.md` — a *pre*-
implementation readiness audit taken at HEAD `7129d0d3f43` — which prescribes the batch but is not
evidence that it was demonstrated red first. **No red-first record exists.** Claim CONFIRMED.

#### `test_content_addressed_drop_pool_member` lane — test exists, post-`224aacd8eb9` result does not {#t7-testcontentaddresseddroppoolmember-lane-test-exists-post-224}

`tests/integration/test_content_addressed_drop_pool_member/test.py` exists with two tests,
`test_drop_dead_pool_member_heals_the_pool` and `test_drop_pool_member_rejected_on_readonly_disk`.
Its last touching commit is `c7acc572b13` ("ca: four CA lanes assert the Stage-A suppression
contract, with evidence"), which `git merge-base --is-ancestor` confirms **predates**
`224aacd8eb9`. So the handoff's statement that the Stage-A 2/2 result is not closure evidence is
CONFIRMED, and no newer lane result is recorded anywhere searched.

#### Named residues — re-audited, all three DISCHARGED in code {#t7-named-residues-re-audited-all-three-discharged-in-code}

1. **The decommission test's assertion.** The plan names
   `CasDecommission.LifelessKeyRefusesTheWholeCommandFailClose`; that symbol no longer exists. It was
   renamed and rewritten in `6a3dd6a9245` (Task 4d) to
   `CasDecommission.LifelessPhysicalKeyCannotRedirectCatalogOwnedDecommission`, which now asserts
   `report.namespaces_removed == 1` — exactly the alternative the residue bullet permitted — plus
   that the planted lifeless key survives. It does **not** scan for `victim` text inside opaque-id
   paths. Residue discharged.
2. **`Pool::listNamespaces`'s vestigial empty-namespace guard.** Gone. `Pool::listNamespaces`
   (`Pool/CasPool.cpp`) now projects logical names purely from `CasRefCatalog::read` + `life_index.resolve`;
   there is no path parsing and no empty-name guard left to be vestigial.
3. **The loose pre-flight LIST base that matched `victim2`.** Gone. `decommissionPoolMember` selects
   owned lives by iterating `catalog_cut.catalog.entries` and matching
   `entry.ns.string() == victim_srid || entry.ns.string().starts_with(victim_namespace_prefix)`,
   where `victim_namespace_prefix = victim_srid + "/"` — a catalog-name canonical-path-component
   match, not a LIST. `deleteListedPrefix` is the only `list` consumer left in the file and it
   targets `staging/<srid>/` and `serverRootDataPrefix(<srid>)`, i.e. loose non-namespace debris.
   `CasDecommission.VictimNameMatchesOneCanonicalPathComponent` pins the `victim2` case and asserts
   through `neighbor->listRefs(neighbor_ns)`. Residue discharged.

   **What is still owed is the RECORD, not the fix**: the plan gates the task on "a source/test
   inventory showing decommission selects the victim exclusively by exact catalog-name ownership and
   has no life-key prefix fallback". That inventory is what the above three paragraphs constitute;
   it needs to be written into the task report, not re-implemented.

#### NEW — TEST finding: the second retirement fence has no test {#t7-new-test-finding-the-second-retirement-fence-has-no-test}

`decommissionPoolMember`'s retirement tail contains two distinct refusals:

- **(a)** `victim_still_owned` over `retirement_catalog_cut` → warning "catalog still owns victim
  namespaces in Removing/Creating state; GC completion is required before slot retirement";
- **(b)** `fresh_retirement_catalog.token != retirement_catalog_cut->token ||
  fresh_retirement_catalog.catalog != retirement_catalog_cut->catalog` → warning "catalog changed
  after the victim ownership check; refusing slot retirement against a stale cut".

`grep` for either warning string across `src/Disks/tests/` and `tests/` returns **nothing**. Arm
(b) — the plan's "token-changed-at-final-check race → retirement refused" duty — has no test at all.

The test that carries the name is
`CasDecommissionCatalogDuties.FinalCatalogFenceKeepsSlotWhenVictimEntryAppearsDuringDrain`, and it
does **not** exercise (b). Its `AddVictimEntryDuringRootDrainBackend` injects the late catalog entry
from inside `list(prefix == "p/roots/victim/", cursor == "")`, i.e. during the
`deleteListedPrefix(serverRootDataPrefix(...))` drain — which happens strictly **before**
`retirement_catalog_cut` is read. So the late entry is already present in the *first* cut and is
caught by arm (a). Delete the entire `fresh_retirement_catalog` block and this test still passes.
Its name asserts a fence it never reaches.

Compounding it: nothing at all happens between the two reads (only two local copies,
`const Layout layout` and `const BackendPtr pool_backend`), so arm (b) can only fire on a genuinely
concurrent external mutation and requires an injecting backend to test — which is what the fixture
class was clearly built for and then wired to the wrong seam. Also `!retirement_catalog_cut` in (b)'s
condition is dead: the optional is set under the identical `report.warnings.empty()` guard, so
reaching (b) implies it is engaged.

Recommended closure work: either rewire the injection to fire between the two reads (e.g. on the
catalog GET rather than the roots LIST) and rename the test to what it then proves, or add a second
test for (b) and rename the existing one to `…VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot`.

#### Sanitizer sweep on the touched test file {#t7-sanitizer-sweep-on-the-touched-test-file}

`grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"` on
`gtest_cas_decommission_catalog_duties.cpp`: two `expectThrowsCode(ErrorCodes::CORRUPTED_DATA, …)`
hits, both matching their throw sites in `decommissionPoolMember` (the missing-`_ckpt` throw and the
same throw reached via the second namespace). No `LOGICAL_ERROR` expectations, no bare
`EXPECT_THROW`. **Clean.**

#### Task 7 verdict {#t7-task-7-verdict}

**CONFIRMED as claimed, with one downgrade and one new TEST finding.** The implementation and its
focused tests are present and the three named residues are already fixed in code — so the remaining
work is narrower than the handoff row implies: it is (i) the missing red-first record, (ii) a
post-`224aacd8eb9` `test_content_addressed_drop_pool_member` lane run, (iii) writing the ownership
inventory down, (iv) the un-tested retirement fence (b) above, and (v) review + a closure commit.

---

### Task 7a — delete probe A {#t7-task-7a-delete-probe-a}

**CONFIRMED NOT IMPLEMENTED.** Everything the plan lists is live. Deletion inventory, by symbol:

**Production C++**
- `Gc::sampleRefListQuality` — declaration `Gc/CasGc.h` (member decl block ~`:806`, contract comment
  `:795-805`), definition `Gc/CasGc.cpp` `:3739`, single call site `Gc/CasGc.cpp` `:700` under the
  comment `PHASE 4/19 ref_list_probe` (`:696`).
- `GcPhaseTimer t(phase_sink, "ref_list_probe")` — `Gc/CasGc.cpp` `:3767`.
- Setting read `store->poolConfig().gc_probe_a_period` — `Gc/CasGc.cpp` `:3768`.
- `PoolConfig::gc_probe_a_period` (default 16) — `Pool/CasPool.h:124`, doc block `:115`.
- Five `ProfileEvents::increment` sites — `Gc/CasGc.cpp` `:3780`, `:3794`, `:3861-3862`, `:3920`,
  `:3923`; the six `extern const Event` declarations at `Gc/CasGc.cpp` `:48`, `:53-57`.
- Six event definitions in `src/Common/ProfileEvents.cpp` `:888-893`: `CasGcRefScanDisagreements`,
  `CasGcProbeADue`, `CasGcProbeAPerformed`, `CasGcProbeASkipped`, `CasGcProbeAHolePresent`,
  `CasGcProbeAHoleAbsent`.
- `ContentAddressedGarbageCollectionLog.cpp` `:60` (execution-order phase list) and `:64`
  (`due`/`performed`/`skipped`/`holes` example).

**Sole-consumer plumbing.** `Gc::enumerateRefPrefix` (`Gc/CasGc.cpp:3671`) — see the caller-count
finding below.

**Stale comments** — `Gc/CasGc.h:215` (`RefScanSummary` doc), `Gc/CasGc.cpp:1603` (intake "does not
need a second opinion"), `Gc/CasGc.cpp:1850` (the B1 doc — **only the probe-A half of the sentence
goes**), `src/Disks/tests/cas_test_helpers.h` (`HintHoleBackend` doc),
`src/Disks/tests/gtest_cas_gc_arithmetic_intake.cpp`.

**Tests**
- `src/Disks/tests/gtest_cas_holey_list_detector.cpp` — whole file, 3 `TEST`s.
- `gtest_cas_retirement_sweep.cpp`: DELETE
  `CasRetirementSweep.ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway` (`:249`) and
  `CasRetirementSweep.TheDetectorsCadenceIsOnEveryFoldingRoundsRow` (`:441` region, the
  `gc_probe_a_period = 2` quiet/sampled pair at `:447`/`:458`/`:466`); CONVERT
  `CasRetirementSweep.TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` (`:384` region,
  the `gc_probe_a_period = period` loop at `:412` and the `= 0` disable assertion at `:435`).
- `gtest_cas_gc_log.cpp`: phase-order expectation list `:383` and `metricsOf(rows, 0, "ref_list_probe")`
  `:413-419`.

**Docs** — `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md:76`
(user-facing phase row) plus the `docs/superpowers/` sweep the plan enumerates.

**Soak/Python** — `utils/ca-soak/soak/signals.py` `:53-85` and `:303`/`:315`/`:326`.

#### PROSE finding (plan, IMPRECISE): `signals.py` is not the only non-C++ consumer {#t7-prose-finding-plan-imprecise-signals-py-is-not-the-only-non}

The plan states signals.py "is the one non-C++ code dependency of the deletion". Four more live
Python consumers exist: `utils/ca-soak/soak/metrics.py` `:28`/`:93` (`probe_a_holes` in the column
set and the row projection), `utils/ca-soak/soak/run.py:876`, and three soak unit tests —
`utils/ca-soak/tests/test_checkpoint_signal_capture.py` (`:23`,`:25`,`:28`,`:103`,`:108`),
`utils/ca-soak/tests/test_metrics_signal_columns.py` (`:57`,`:66`),
`utils/ca-soak/tests/test_signals.py` (`:172`,`:197`,`:227`,`:232`,`:238`). Left alone the last three
go red on deletion, and the first two keep projecting a column nothing emits.

#### PROSE finding (plan, FALSE): `enumerateRefPrefix` has three callers, not two {#t7-prose-finding-plan-false-enumeraterefprefix-has-three-caller}

The plan asserts twice ("exactly two callers tree-wide") that this is what makes the one-LIST
criterion verifiable. Measured at HEAD: `Gc::enumerateRefPrefix` (`Gc/CasGc.cpp:3671`) is called from
`Gc::listRefPrefix` (`:3719`), `Gc::sampleRefListQuality` (`:3788`) and the **rebuild** path
(`:4109`, `rebuild_ref_scan`). Step 3 explicitly says "if a third caller has appeared, the criterion
needs re-derivation, not a pass" — that condition has fired. The criterion survives (the rebuild
call is not on the regular folding-round path), but Step 2's "collapse it into `listRefPrefix`"
option is now wrong and the helper must be kept. Already flagged by the earlier
`task7-9-ready-audit.md`; still uncorrected in the plan.

#### CODE finding (comment): the `PHASE N/19` numbering is ALREADY broken at HEAD {#t7-code-finding-comment-the-phase-n-19-numbering-is-already-bro}

The plan instructs renumbering 19 phases down to 18. That premise is stale in both directions.
`grep -oE "PHASE [0-9]+/[0-9]+"` over `Gc/CasGc.cpp` yields 17 markers with **9 and 17 missing and 15
duplicated** — the sequence is already corrupt before Task 7a touches it. Separately,
`gtest_cas_gc_log.cpp:383`'s phase-order expectation now lists **19** phase names including
`pre_fold_ref_drain` (added by Task 5), which the plan's 19-item phase list does not contain. So the
post-7a count is 18 emitted phases, not 18-from-19-by-one, and whoever executes Step 1 must
re-derive the numbering from the `GcPhaseTimer` sites rather than apply the plan's arithmetic. This
is exactly the "stale `N/19` survives for a year" hazard the plan warns about, already realised.

#### Not named by the plan's Files list but caught by its grep: a compile-time pin {#t7-not-named-by-the-plan-s-files-list-but-caught-by-its-grep-a}

`src/Disks/tests/gtest_cas_ref_catalog.cpp` defines `DB::Cas::tests::GcRoundPlanSignatureAccess` with
`using ProbeSignature = decltype(&Gc::sampleRefListQuality);` and
`static_assert(std::is_same_v<ProbeSignature, ExpectedProbeSignature>)`. Deleting the member breaks
this translation unit at compile time (and the corresponding `friend` in `Gc/CasGc.h` becomes dead).
The Step-1 grep pattern does find it; the Files list does not mention it.

#### KEEP list re-verified as distinct {#t7-keep-list-re-verified-as-distinct}

- **B1/B2**: `logs_accounted`/`logs_applied` on the `fold_ref_intake` phase (`Gc/CasGc.cpp` `:2610`
  the single cursor-advance site, `:2827`/`:2830`/`:2834`) and the `produced=false` ordinals accounting
  (`:1904`). No dependence on the detector.
- **Mount capability probe (#23)**: `Backend/CasProbe.h` and `Backend/CasSentinelProbe.h` both exist
  and are separate files with no reference to `sampleRefListQuality`, `gc_probe_a_period` or any
  `CasGcProbeA*` event.
- **False positives confirmed**: `gtest_cas_upload_fanout.cpp:914/955/962/969` `probe_acquired`
  (upload permit), `gtest_cas_gc_shard_plan.cpp:236/240` local `probe_a` (a `ManifestId`),
  `base/poco/NetSSL_OpenSSL/src/Context.cpp` `poco_ssl_probe_and_set_default_ca_location` (unrelated).

---

### Task 7b — destruction enablement {#t7-task-7b-destruction-enablement}

#### `UniversePolicy` — CONFIRMED non-authoritative {#t7-universepolicy-confirmed-non-authoritative}

`enum class UniversePolicy : uint8_t` lives in `Gc/CasGc.h` (`:55-72`) with enumerators
`StageA_Suppressed = 0` and `AuthoritativeForTest = 1`, and `kDefault = StageA_Suppressed`. The
test-only enumerator awaiting the mechanical rename to `Authoritative` is
**`UniversePolicy::AuthoritativeForTest`**; its only production read is
`const bool universe_authoritative = policy == UniversePolicy::AuthoritativeForTest;` in `Gc::fold`
(`Gc/CasGc.cpp:3020`, under the comment "Term 3, the universe seam").

#### The destructive suppression formula — terms as they ACTUALLY stand {#t7-the-destructive-suppression-formula-terms-as-they-actually-s}

In `Gc::fold`, `Gc/CasGc.cpp` `:3029-3035`:

```
result.frontier_complete = universe_authoritative
    && result.frontier_namespaces > 0
    && result.frontier_proven == result.frontier_namespaces;
const bool frontier_incomplete = !result.frontier_complete;

result.suppress_destructive =
    !report.anomalies.empty() || !carried_holds.empty() || frontier_incomplete;
```

Independently-suppressing terms, enumerated: **(1)** a non-empty `report.anomalies`; **(2)** a
non-empty `carried_holds`; **(3a)** `!universe_authoritative`; **(3b)** `frontier_namespaces == 0`
(the R11 empty-universe vacuity floor); **(3c)** `frontier_proven != frontier_namespaces`. The
consumed value is read once as `const bool suppress_destructive = folded.suppress_destructive;`
at `Gc/CasGc.cpp:738` under "THE ROUND'S DESTRUCTIVE GATE".

#### PROSE finding (plan, IMPRECISE→FALSE): the quoted `frontier_complete` expression is stale {#t7-prose-finding-plan-imprecise-false-the-quoted-frontiercomple}

The plan's "Exact anchors, so 'unchanged' is checkable rather than asserted" paragraph quotes
`frontier_complete = universe_authoritative && result.frontier_proven == result.frontier_namespaces`
and the anchors `:2708-2709`, `:2704`, `:2703`, `Gc/CasGc.h:53-63`, `:62`, `:325`. Every anchor has
moved, and more importantly the quoted expression is **missing the `frontier_namespaces > 0`
conjunct** that R11 added. That matters for two reasons: the paragraph's own rule is "if the flip
requires touching any term of that expression, the flip is wrong", and Step 1b's per-term assertion
duty ("each of the three terms independently still suppresses") enumerates three terms where the
code has five independently-suppressing conditions. **The empty-universe floor (3b) is a real
suppressor with no Step-1b assertion duty attached to it** — a healthy round on a *fresh* pool
suppresses for a reason the plan's checklist never names. Worth promoting to an explicit assertion in
Step 1b when 7b is executed; as written it is a plan-prose defect, hence PROSE.

The reconciliation prose (three terms, four forbidders, budget exhaustion entering through
`frontier_complete`) is otherwise accurate: `Gc::fold`'s budget path leaves rows unproven, which is
term (3c).

#### `PENDING` double count — CONTRADICTED, already fixed {#t7-pending-double-count-contradicted-already-fixed}

`04290_content_addressed_no_leftovers.sh:102` and `04295_content_addressed_mutation_no_leftovers.sh:103`
both now read `{ print $col["pending_condemned"] }` with the trailing comment "already
candidates+retired per its doc in Gc/CasGc.h; summing all three double-counts". No
`pending_candidates + pending_condemned + pending_retired` sum survives anywhere in `tests/` (the one
remaining mention, `05020_content_addressed_fsck.sh:40`, is a `grep -c` column-presence check, not a
gauge). `docs/superpowers/cas/BACKLOG.md {#stateless-pending-double-count}` is titled
`RESOLVED …` and records "RESOLVED 2026-07-30: both tests now read `pending_condemned` alone
(`8e9b06c2a81`)". **Plan Step 5 and the handoff's 7b row are stale on this point.** (PROSE)

#### The Stage-A return items — five is wrong twice over {#t7-the-stage-a-return-items-five-is-wrong-twice-over}

`git grep -n "STAGE-A RETURN ITEM"` at HEAD returns **8 non-doc files** (3 further hits are inside
the plan itself):

1. `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h` — the `kDefault`
   comment that names the others.
2. `src/Disks/tests/gtest_ca_wiring.cpp` — the displacement test; currently
   `EXPECT_GT(after.unreachable, 0u)`, to be restored to `EXPECT_EQ(…, 0u)`. **Still weakened.**
3. `src/Disks/tests/gtest_cas_gc_log.cpp` — `CasGcLog.EmitsStartFinishWithCounts`; asserts
   `objects_deleted` stays 0 on every Finish. **Still weakened.**
4. `tests/broken_tests.yaml`.
5. `tests/queries/0_stateless/04290_content_addressed_no_leftovers.sh`
6. `tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh`
7. `tests/queries/0_stateless/05008_ca_gc_snap_prune.sh`
8. `tests/queries/0_stateless/05010_content_addressed_mounts_gc_health.sh`

**`broken_tests.yaml` has FOUR entries, not three**: `05008_ca_gc_snap_prune` (`:329`),
`04290_content_addressed_no_leftovers` (`:338`), `05010_content_addressed_mounts_gc_health`
(`:347`), `04295_content_addressed_mutation_no_leftovers` (`:355`). Step 4's removal bullet, Step 6,
and the `CasGc.h` `kDefault` comment all omit `05010`. The plan's own MEASURED WARNING paragraph
*does* cite 05010's pool, so the omission is internal inconsistency, not ignorance. (PROSE for the
plan; the `CasGc.h` comment omission is a CODE-comment defect, since that comment is the in-tree
index the next reader greps.)

#### CODE/PROSE finding: the exit-condition grep is not a partition of the marker set {#t7-code-prose-finding-the-exit-condition-grep-is-not-a-partitio}

Step 4/6's exit condition is `grep -n "STAGE-A RETURN ITEM"` returns NOTHING. A **second, disjoint
marker class** exists that this grep cannot see — the banner
`###  STAGE-A CONTRACT.  RESTORE … AT STAGE B TASK 7b.  ###` — in seven more files, all of which
assert the *suppression* contract and therefore go RED the moment `kDefault` flips:

- `tests/integration/test_content_addressed_drop_pool_member/test.py:203` (asserts "Stage A must
  reclaim NOTHING, but the pool shrank" — i.e. it fails *if destruction works*)
- `tests/integration/test_content_addressed_gc_s3/test.py:74`
- `tests/integration/test_content_addressed_ref_snaplog/test.py:126`, `:198`
- `tests/integration/test_content_addressed_shared_pool/test.py:177`, `:334`
- `tests/integration/test_cas_replicated_relink/test.py:877`
- `utils/ca-soak/scenarios/cards/s28_s33_corner.py:406`
- `utils/ca-soak/scenarios/framework/assertions.py:232`, `:260`, `:310`, `:363`, `:376`
  (plus `utils/ca-soak/scenarios/tests/test_leftovers_stage_a.py`, which exists and pins the
  Stage-A-allowance behaviour)

The complete detector is a case-insensitive `Task 7b` grep over `src/ tests/ utils/`, which returns
15 non-doc files. **Executing Task 7b against the plan's stated exit condition would declare the
task closed while leaving the entire integration and soak surface asserting the opposite of the new
behaviour** — and note `test_content_addressed_drop_pool_member`, the Task-7 closure lane, is one of
them. Label: PROSE for the plan's wording, but the consequence is a live CODE/TEST breakage set that
must be in Task 7b's inventory.

#### Prerequisite: "explain the unproven namespaces" {#t7-prerequisite-explain-the-unproven-namespaces}

Recorded in `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task7b-frontier-prereq-audit.md`,
status **BLOCKED**. It classifies every non-anomalous path to an unproven row (post-LIST append above
the frozen tail — its best-fit hypothesis and the one with *no* dedicated metric; probe-budget
exhaustion, already observable via `frontier_unprobed_budget`; no arithmetic genesis; effective hold;
genuine gap; undecodable `_ckpt`; whole-fold abort) and concludes the production warning does not
carry enough information to attribute the measured `3155/3157` and `11358/11369` deficits. It demands
one instrumented reproduction. That audit also lists Task 5b as a blocker; **that blocker is now
discharged** — `docs/superpowers/cas/BACKLOG.md {#recover-ref-table-list-residual-closed}` records
`[RECOVER-REF-TABLE-LIST-RESIDUAL] CLOSED — Task 5b`, naming `c863cdd7fa60` / `357cf7b963f4` and the
seven-commit closing chain. So the plan's `[Amendment note]` gate ("verify the BACKLOG entry reads
CLOSED before flipping") is **satisfied**; the frontier-attribution prerequisite is not.

#### Task 7b verdict {#t7-task-7b-verdict}

**CONFIRMED NOT IMPLEMENTED.** `kDefault` is `StageA_Suppressed`; `AuthoritativeForTest` awaits its
rename; the gate formula is intact and must stay so. Two of the plan's own Step-4/5 obligations are
stale (`PENDING` already fixed; "five sites"/"three entries" undercount), and its exit-condition grep
misses a second marker class covering seven integration/soak files including the Task-7 closure lane.

---

### Findings register {#t7-findings-register}

| # | Label | Grade | Where | What |
|---|---|---|---|---|
| 1 | **TEST** | — | `CasDecommissionCatalogDuties.FinalCatalogFenceKeepsSlotWhenVictimEntryAppearsDuringDrain` | Injects the late entry during the `serverRootDataPrefix` LIST, i.e. before `retirement_catalog_cut`; exercises the `victim_still_owned` arm, not the `fresh_retirement_catalog` token fence it is named for. Deleting the token fence leaves it green. No test asserts either retirement warning string. |
| 2 | **CODE** (comment) | — | `Gc/CasGc.cpp` `PHASE N/19` markers | Sequence already corrupt at HEAD (9 and 17 missing, 15 duplicated) and the emitted phase list is now 19 names including `pre_fold_ref_drain`. Task 7a's renumbering must re-derive from the `GcPhaseTimer` sites. |
| 3 | **CODE** (comment) | — | `Gc/CasGc.h`, `UniversePolicy::kDefault` block | Names three stateless return items and omits `05010_content_addressed_mounts_gc_health`, though it is registered in `broken_tests.yaml` and carries the marker. This comment is the in-tree index; an incomplete index is worse than none. |
| 4 | PROSE | FALSE | plan `### Task 7a`, Steps 2 and 3 | "`enumerateRefPrefix` has exactly two callers tree-wide" — there are three (`listRefPrefix`, the detector, rebuild). |
| 5 | PROSE | IMPRECISE | plan `### Task 7a`, Files | "`signals.py` … the one non-C++ code dependency" — `metrics.py`, `run.py` and three soak unit tests also consume `probe_a_holes` / the `CasGcProbeA*` names. Also the `PHASE …/19` → `/18` renumbering instruction rests on a count that no longer holds. |
| 6 | PROSE | FALSE | plan `### Task 7b`, Step 5 | The `PENDING` double-count fix is already landed (`8e9b06c2a81`, BACKLOG RESOLVED); the step describes work that no longer exists. |
| 7 | PROSE | FALSE | plan `### Task 7b`, "Exact anchors" | The quoted `frontier_complete` expression omits the `frontier_namespaces > 0` conjunct, so the "five independently-suppressing conditions" are presented as three and Step 1b's per-term assertion duty has a hole (the empty-universe floor). |
| 8 | PROSE | FALSE | plan `### Task 7b`, Steps 4 and 6 | "five sites" / "three `broken_tests.yaml` entries" — 8 marker files and 4 yaml entries; and the `STAGE-A RETURN ITEM` exit grep is not a partition of the Task-7b marker set (the `STAGE-A CONTRACT … AT TASK 7b` class adds 7 files, including the Task-7 closure lane `test_content_addressed_drop_pool_member`). |

## Report: Tasks 8 and 9 {#report-t8}

## Stage B audit — Task 8 and Task 9 {#t8-stage-b-audit-task-8-and-task-9}

Read-only audit. Repo `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`,
HEAD `ce312f547c3`. No edits, no builds, no test runs. Citations are by symbol, not line number.

Every finding is labelled **CODE**, **TEST** or **PROSE**. Per the campaign rule, PROSE findings are
batched into `docs/superpowers/cas/deferred-docs-fixes.md` and do NOT open a fix round; CODE and TEST
findings do. Prose findings are additionally graded FALSE or IMPRECISE.

---

### 1. Task 8 {#t8-1-task-8}

#### 1.1 Commit existence and content — CONFIRMED {#t8-1-1-commit-existence-and-content-confirmed}

All three named commits exist and are ancestors of HEAD.

| commit | subject | position | content |
|---|---|---|---|
| `d34aa06d89f` | `ca: model Task 8A ownership duties` | 58 behind HEAD | 20 files: `CaRefWriterCleanupCore.tla` + `CaRefFoldClampRecoveryCore.tla`, 7 new/edited `.cfg`, both runners, `README.md`, and `docs/superpowers/models/2026-08-02-stage-b-task8a-RESULTS.md` |
| `c3cc24c8152` | `Defer uncertain CAS writer cleanup duties` | 44 behind HEAD | `CasPartWriteTxn.cpp` (dtor), `CasPool.cpp` / `CasPool.h` (duty queue), new `src/Disks/tests/gtest_cas_writer_duties.cpp` (218 lines) |
| `8f14bc119fe` | `Retire CAS orphan manifest source edges` | 41 behind HEAD | `CasBlobInDegree.{cpp,h}`, `CasGc.{cpp,h}`, `CasOrphanManifestSweep.{cpp,h}`, new `src/Disks/tests/gtest_cas_orphan_nomination.cpp` (268 lines) |

The model gate is genuinely recorded: `2026-08-02-stage-b-task8a-RESULTS.md` tabulates both required
R3 sabotages (`_sab_deletebeforeadoption`, `_sab_nominationcontaminates`), the R2 sabotage
`_sab_retireuncertain`, both duty witnesses, and the two green controls, each with an exact expected
property name, observed result, distinct-state count and depth, plus the pinned TLC build hash and a
reproduction command. It also records that the sabotage-first run failed closed before the property
names existed. This is the strongest evidence artefact in either task.

#### 1.2 Implementations in current code — CONFIRMED {#t8-1-2-implementations-in-current-code-confirmed}

**Writer duty queue (R2).**
- `PartWriteTxn::~PartWriteTxn` (`Pool/CasPartWriteTxn.cpp`) now branches: on
  `PrecommitState::Uncertain` or `PrecommitState::Durable` it calls `Pool::enqueueWriterCleanupDuty`
  and returns *without* `retireBuildSeq`; otherwise it retires as before.
- `Pool::enqueueWriterCleanupDuty` (`Pool/CasPool.cpp`), declared `noexcept`, allocates the duty,
  pushes it under `writer_cleanup_mutex` and notifies. Its `catch (...)` sets the sticky
  `writer_cleanup_queue_failed` atomic and swallows a nested logging throw.
- `Pool::writerCleanupDutiesPending` returns true if the sticky bit is set OR any queue is non-empty,
  and is consulted at both mount-teardown "clean farewell" sites in `CasPool.cpp`. The fail-close
  direction is correct: a lost duty pins the floor rather than certifying a clean death.
- `Pool::drainWriterCleanupDuties` serialises per namespace via `draining` + condition variable,
  appends an exact `OwnerTransition` removal only when the precommit is still present (absence is a
  conclusive settlement, not an error), then calls `retireBuildSeq` *after* that same state
  observation, and restores `draining` on the exception path before rethrowing.
- The admission seam is the private template `Pool::mutateRefsAfterWriterCleanup` (`CasPool.h`),
  which drains then forwards.

I checked the seam for completeness rather than trusting it. Every `ref_ledger.` call in
`CasPool.cpp` was enumerated; the six *mutating* forwards — `dropRef`, `updateRefPublishedAt`, both
`dropNamespace` overloads, `appendRefOps`, `tryPublishSnapshotAndAdvanceCheckpointOnce` — all go
through `mutateRefsAfterWriterCleanup`. Every other `ref_ledger.` call is a read, a `staging*`
operation, a shutdown/quiesce path, or a `*ForTest` accessor. `ref_ledger` is private, so no
external caller can bypass the seam. The drain itself calls `ref_ledger.appendRefOps` directly,
avoiding re-entry. **The seam is a genuine partition of the durable-mutation surface.**

**Orphan nomination (R3).**
- `planManifestCursorPage` (`Gc/CasOrphanManifestSweep.cpp`) exact-GETs the captured body, decodes it
  via `decodePartManifest`, validates identity with `refMatchesBody` + `manifestNamespaceMatches`
  (throwing `CORRUPTED_DATA` on mismatch), and only then derives `BlobSourceRetirement` entries per
  `EntryPlacement::Blob` entry into `ManifestSweepResult::Nomination`. It deletes nothing.
- `Gc::…` fold (`Gc/CasGc.cpp`) collects `orphan_source_retirements` from the nominations, buckets
  them by `blobShard` on the sharded path, and passes them as the trailing
  `std::vector<BlobSourceRetirement>` argument of `foldDeltasIntoGeneration` — a parameter distinct
  from the `BlobDelta` stream, which is what makes the input accounting-neutral.
- Deletion happens only in the post-CAS `orphan_sweep` phase tail, after the round's `gc/state` CAS.
  The ordering claim in the code comment matches the code.
- Planning is gated by `if (!suppress_destructive && manifest_sweep_list_budget_keys > 0)`, so a
  suppressed pass produces no nominations and therefore reaches no delete. I checked this
  specifically because the delete loop itself has no `suppress_destructive` test — the gate is
  upstream and it holds.

#### 1.3 Direct tests — present, named below {#t8-1-3-direct-tests-present-named-below}

`src/Disks/tests/gtest_cas_writer_duties.cpp` (7 tests, suite `CasWriterDuties`):
`UncertainAdoptedGrantStaysActiveUntilTheNextMutationRemovesIt`,
`ProvenAbsentGrantDrainsAsNoOpBeforeTheNextMutation`,
`DropRefServicesPendingDutyBeforeRemovingTheRef`,
`UpdateRefPublishedAtServicesPendingDutyBeforeUpdatingTheRef`,
`DropNamespaceOverloadsServicePendingDutyBeforeRemoval`,
`SnapshotAttemptServicesPendingDutyBeforePublishingLedgerState`,
`PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant`.

`src/Disks/tests/gtest_cas_orphan_nomination.cpp` (4 tests, suite `CasOrphanNomination`):
`RetiresExactManifestSourcesBeforeDelete`, `CorruptManifestIsRetainedAndSurfaced`,
`TokenAbaIsRetainedAndSurfaced`, `SourceRetirementIsAccountingNeutral`.

Both suite names are present in the CA gate filter (verified in the recorded filter string of
`build/task5_step11_final_ca_rerun2.log`), so neither is orphaned from the gate.

**Would these fail if the behaviour regressed?** Yes, for the ones I traced:
- `leaveRejectedCleanupDuty` asserts `minActive() == rejected_seq` before the mutation under test, and
  each drain test then asserts `minActive() == peekNextBuildSeq()`. The two values differ in the
  pre-state, so the post-assertion is not vacuous.
- `RetiresExactManifestSourcesBeforeDelete` sets `source_absent_when_delete_started` inside an
  overridden `deleteExact` keyed on the watched manifest. If no nomination happened, that flag stays
  false *and* `manifestExists` stays true — two independent failures.
- Its in-degree oracle is a real partition: blobs 0–3 keep in-degree 1 (a second source), blobs 4–5
  drop to 0, and `condemnedCount == 2`. A touch-only or all-zero predicate fails it.
- `SourceRetirementIsAccountingNeutral` retires one present and one absent edge and asserts
  `retired.unmatched_removes == 0` and the `applied` ordinal vector byte-stable. Implementing
  retirement as `BlobDelta{remove = true}` would break both.

**Sanitizer sweep** (`grep -nE "EXPECT_(ANY_)?THROW|expectThrowsCode\(.*LOGICAL_ERROR"` on both new
files): two hits, both in `gtest_cas_orphan_nomination.cpp`
(`CorruptManifestIsRetainedAndSurfaced`, `TokenAbaIsRetainedAndSurfaced`). I checked each against its
actual throw site. The corrupt-manifest path throws `ErrorCodes::CORRUPTED_DATA` from
`CasOrphanManifestSweep.cpp`; the ABA path throws `ErrorCodes::CORRUPTED_DATA` from the `orphan_sweep`
phase in `CasGc.cpp`. **Neither is `LOGICAL_ERROR`, so there is no death-test/abort hazard.** See
finding T-1 for the separate precision problem.

#### 1.4 Plan checkbox state and Step-2 scenario coverage {#t8-1-4-plan-checkbox-state-and-step-2-scenario-coverage}

Plan section `### Task 8` (`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md`):
Step 1 is `[x]`; Step 2 is `[ ]`; the combined Step 3/4/5 line is `[ ]`. The Step-5 commit subject
`ca: writer/gc — duty queue, uncertain-grant retirement guard, neutral orphan nomination (R2+R3)`
does not exist in the log. The SDD ledger agrees (`Task 8: PARTIAL`). **The claimed state is
CONFIRMED.**

Step 2 enumerates seven scenarios. Coverage:

| # | Step-2 scenario | Covered by | Verdict |
|---|---|---|---|
| 1 | build retired while grant wedged → refused, queue holds it | `UncertainAdoptedGrantStaysActiveUntilTheNextMutationRemovesIt` | covered |
| 2 | duty queue drains on wedge resolution **both ways (adopt/reject)** | adopt: `UncertainAdoptedGrant…`; reject: see T-2 | **adopt only** |
| 3 | crash remnants cleaned via the successor-seal path | `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant` | covered |
| 4 | sweep nominates a swept manifest's blobs; B2 ordinals and unmatched-remove byte-stable | in-degree in `RetiresExactManifestSourcesBeforeDelete`; accounting in `SourceRetirementIsAccountingNeutral` | covered, but split across two fixtures (see T-3) |
| 5 | nomination adopted in `gc/state` before any delete (op order) | `RetiresExactManifestSourcesBeforeDelete` via `source_absent_when_delete_started` | covered |
| 6 | ABA token at manifest key → retain + surface | `TokenAbaIsRetainedAndSurfaced` | covered, imprecise fence (T-1) |
| 7 | the S42 regression | `RetiresExactManifestSourcesBeforeDelete` (its comment names S42 and the fixture seeds the recorded shape) | covered |

Four of the six duty tests additionally cover Pool-delegate seam completeness, which the plan did not
enumerate — that is coverage above the bar, not a gap.

#### 1.5 Recorded RED evidence — NOT FOUND (for C++) {#t8-1-5-recorded-red-evidence-not-found-for-c}

- **Model RED: present and exact.** The RESULTS file records every sabotage going red against its
  named property, and records the sabotage-first failure before the properties existed.
- **C++ RED: no artefact found.** I searched `tmp/`, `build/*.log` and the SDD directory for
  `CasWriterDuties` / `CasOrphanNomination`. The only hits are Task-5 gate logs
  (`build/task5_step11_final_ca*.log`, `build/task5_merge_full_ca.log` and the suite inventories),
  which show all 11 tests **passing** — green, never a recorded red.
- Two `tmp/` artefacts are decoys the closer must not mistake for evidence: `tmp/t8_report.md`
  (dated 2026-07-12) is a *different* Task 8 about `CasLayout` ref-object keys at commit
  `d9ad4977a2b`; `tmp/review_t8_verify.log` (2026-07-24) is a `RefWriterChunkedFlush`/`CasRefCodec`
  run. `tmp/task8_defer_test.diff` and `tmp/task8_janitor_test.diff` (2026-08-02) are janitor/defer
  test diffs, not writer-duty or nomination reds.
- Each new test does carry an in-file comment naming the mutation that would break it (e.g. "Removing
  `mutateRefsAfterWriterCleanup` from the `dropRef` delegate leaves the rejected build at
  `minActive`"). That is a well-written falsification hypothesis, but it is a claim, not a run.

The handoff's "recover or redemonstrate the load-bearing REDs" is therefore **still open, and for the
C++ slices it is a demonstrate, not a recover** — I found nothing to recover.

#### 1.6 Task 8 findings {#t8-1-6-task-8-findings}

**T-1 — TEST, real.** `CorruptManifestIsRetainedAndSurfaced` and `TokenAbaIsRetainedAndSurfaced` both
assert `EXPECT_THROW(runRegularRoundReclaiming(*f.gc), DB::Exception)`. `runRegularRoundReclaiming`
runs a whole GC round, which can throw `DB::Exception` from dozens of unrelated sites; either test
would pass if the round started failing for a reason having nothing to do with manifest decode or
ABA. Both throw sites are `ErrorCodes::CORRUPTED_DATA`, and the project helper
`DB::Cas::tests::expectThrowsCode` is already used by the sibling `gtest_cas_writer_duties.cpp` in
the same commit pair. Tighten both to `expectThrowsCode(ErrorCodes::CORRUPTED_DATA, …)`. This is the
"a fence trusted for more than it checks" case: the retention assertion
(`head(...).exists`) is sound, the *surfacing* assertion is not.

**T-2 — TEST, real (Step-2 gap).** Step 2 requires the duty queue to drain "on wedge resolution both
ways (adopt/reject)", and the model gate has both `_witness_duty_adopt` and `_witness_duty_reject`.
On the C++ side only the adopt arm resolves an actual wedge:
`UncertainAdoptedGrantStaysActiveUntilTheNextMutationRemovesIt` asserts
`ASSERT_TRUE(store->refLaneWedgedForTest(ns))` then `EXPECT_FALSE(...)` after the drain. The
reject-side test, `ProvenAbsentGrantDrainsAsNoOpBeforeTheNextMutation`, asserts the opposite
precondition — `ASSERT_FALSE(store->refLaneWedgedForTest(ns))` — because it is a controller
pre-attempt refusal, not a wedged lane. So "the wedge resolves as *reject* and the duty drains as a
no-op" has model coverage but no C++ coverage. The behaviour may well be correct; the point is that
nothing would catch its regression.

**T-3 — TEST, minor.** Step 2 asks for B2 ordinals and unmatched-remove accounting to be asserted
byte-stable *on the nominating round*. `SourceRetirementIsAccountingNeutral` asserts that on a
synthetic `foldDeltasIntoGeneration` call with a bare `InMemoryBackend`, not on the real round driven
by `RetiresExactManifestSourcesBeforeDelete`. The neutrality proof and the end-to-end proof never
meet. Adding the two accounting assertions to the real-round test would close it cheaply.

**C-1 — CODE, moderate (footgun, not a live bug).** `sweepManifestCursorPage`
(`Gc/CasOrphanManifestSweep.cpp`) still plans a page and then **exact-deletes each nomination
immediately, with no source-edge retirement and no `gc/state` adoption** — that is precisely the S42
defect this task fixed on the GC path. I enumerated its callers across all of `src/`: every one is a
test (`gtest_cas_orphan_manifest_sweep.cpp`, `gtest_cas_sweep_deletion_premise.cpp`). So there is no
live exposure today. But it is a non-static, header-declared function whose doc comment in
`CasOrphanManifestSweep.h` reads as an ordinary production API and says nothing about the missing
retirement, while the sibling `planManifestCursorPage` comment explicitly promises retirement. The
next production caller to reach for the obvious-looking name reintroduces S42. Either fold it into
the test translation units, or state in its comment that it deletes without retiring and is not a
production path. (Note the contrast with `sweepNamespace`, which *does* delete directly but carries
an explicit discriminator — "emits NO blob deltas (a pre-precommit body never contributed `+1`)" —
that justifies it.)

**C-2 — CODE, minor (comment policy).** Two production comments cite internal documents, which the
comment policy forbids because those artefacts are deleted from the branch:
`ContentAddressedTransaction::writeFile` ends its life-capture rationale with
"(directive §namespace-file-requirements)", and `NamespaceLifeId::stageATransition`
(`Primitives/CasNamespaceLifeId.h`) says "Task 6 DELETES it, gated on a tree-wide grep". Both
rationales are good and should stay; only the citations should go. Neither was introduced by the
Task-8 commits, so this belongs to whoever does the Task-14 sweep rather than to Task 8's closure.

**C-3 — CODE/TEST, cross-task note.** `gtest_cas_writer_duties.cpp` introduces a **new** use of
`NamespaceLifeId::stageATransition` (in
`UncertainAdoptedGrantStaysActiveUntilTheNextMutationRemovesIt`, to build a fault prefix). Task 6's
exit criterion is a tree-wide zero-hit grep for `stageATransition` in build inputs. This is not a
Task-8 defect, but Task 6's migration list must include it or that gate will not close.

**P-1 — PROSE, IMPRECISE.** The plan's Files list for Task 8 names
`Create: src/Disks/tests/gtest_cas_writer_duties_nomination.cpp` (one file) and describes the work as
"one coherent change". The implementation is two files and two commits. The split is arguably better
than the plan (R2 and R3 are independently revertable), but the plan text now describes something
that does not exist and the closer should reconcile it rather than hunt for a missing file.

**P-2 — PROSE, IMPRECISE.** The plan cites the S42 recorded shape as
`reports/2026-07-26-s42-stale-edge-repro/`. No `reports/` directory exists at the repo root; the
tracked artefact is `docs/superpowers/reports/2026-07-26-s42-stale-edge-repro/`. The evidence is
real, the path is wrong.

**Q-1 — open question for the closer, not a finding.** The plan's Files list names
`CasPartWriteTxn.cpp` "staged-body cleanup `:1438`" as a modify target. `c3cc24c8152` changed only
the destructor; `PartWriteTxn::cleanupStagedManifestDebrisBestEffort` is untouched and is still
invoked only from `abandon` and `promote`. Under the new destructor, an `Uncertain`/`Durable`
transaction returns before any staged-debris cleanup, so that debris now falls entirely to the orphan
sweep. That may be exactly the intended division of labour (R3 gives the sweep a safe retire-then-
delete path), but the plan asked for a change here and none was made. Someone with the design context
should say "intended" or "gap" explicitly rather than leaving it inferred.

**Q-2 — observation, low severity.** On the ABA path in `CasGc.cpp`'s `orphan_sweep` phase, the
source edges of the nominated manifest are already retired and durable in the round's `gc/state` CAS
by the time the `TokenMismatch` throw fires. The manifest body is retained (correct), but its edges
are gone. Since a token change at an immutable manifest key is illegal by construction and the round
fails loudly, and since the sweep cursor advanced inside the same CAS so the next round resumes past
the object rather than wedging on it, I do not think this is a defect. Recording it because the
sequencing is not obvious from the test, which asserts only that the body survives.

#### 1.7 Task 8 verdicts {#t8-1-7-task-8-verdicts}

| Claim | Verdict |
|---|---|
| Model complete in `d34aa06d89f` | **CONFIRMED** — model + configs + runners + a genuinely exact RESULTS artefact |
| Production slices `c3cc24c8152`, `8f14bc119fe` exist with direct tests | **CONFIRMED** |
| Do not duplicate the duty queue or nomination implementation | **CONFIRMED** — both are complete and coherent in current code |
| Task-level closure open | **CONFIRMED** — Step 2 and Step 3/4/5 unchecked, no closure commit, ledger agrees |
| Load-bearing REDs need recovery | **CONFIRMED, and stronger than claimed** — model RED is recorded and exact; C++ RED does not exist in any artefact I can find and must be demonstrated, not recovered |
| Every Step-2 scenario audited | **6 of 7 covered**; the reject arm of wedge resolution (T-2) has model coverage only |

---

### 2. Task 9 {#t8-2-task-9}

#### 2.1 Commit and placement — CONFIRMED {#t8-2-1-commit-and-placement-confirmed}

`ca07cbf87fd` (`ca: docs — R1 verbatim-file aliasing closed by the namespace-life re-key`, 52 behind
HEAD) creates `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md` (113 lines) and
rewrites R1's entry in `docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md` from
"SUPERSEDED IN PART" to "CLOSED, 2026-08-02" with a link to the note. **The closure note lives at
`docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md` in the current tree.** It
carries the required frontmatter and every heading has a `{#kebab-case-anchor}`.

All four Step-1 sub-hazards (a)–(d) are addressed, and Step 2's loose-mountpoint question is answered
negatively with a code-derived argument. Plan checkboxes for Steps 1–3 are all `[x]`; the ledger says
`Task 9: COMPLETE in ca07cbf87fd`. **Consistent.**

#### 2.2 The substantive claims — verified against code, all CONFIRMED {#t8-2-2-the-substantive-claims-verified-against-code-all-confirm}

I did not take the note's word for any of these.

- "The obsolete `namespaceFilesReadable` TOCTOU gate and `namespaceIsRemoved` surface are absent from
  the current tree." A tree-wide grep over `src/**/*.{cpp,h}` returns **zero hits** for
  `namespaceFilesReadable`, `namespaceIsRemoved`, `namespacePhysicallyEmpty` and
  `runNamespaceCleanupPasses`. **CONFIRMED.**
- "Once a reader or `CaInlineWriteBuffer` has obtained a life, it deliberately retains that exact
  `NamespaceLifeId`." In `ContentAddressedTransaction::writeFile`, the table-file branch resolves
  `life` once at buffer-open time and captures it **by value** into the callback, which calls
  `putNamespaceFile(life, name, …)`. **CONFIRMED in code.**
- "A same-name namespace rebirth … cannot retarget a loose mountpoint key."
  `Layout::namespaceFileKey` composes `namespaceFilesPrefix(life) + file_name`, i.e. keyed by the
  opaque life; `Layout::mountpointObjectKey` returns `prefix + "/roots/" + key` with no namespace,
  catalog or life input. **CONFIRMED** (with the caveat in P-5 below).
- Every one of the nine cited commits resolves and its subject matches the role the note assigns it
  (`827bc0a9189`, `6a3dd6a9245`, `2b8475fc6f6`, `21ce9e99f4d`, `4048163f0dd`, `3b952c6cbde`,
  `bf396ffa50d`, `d278d130024`, `111bb12a407`). **CONFIRMED.**

**Additional confirmation that strengthens the note beyond what it claims:** I enumerated the callers
of `CasRefCatalog::resolveLifeOrSentinel`, the one function that can still return the name-derived
sentinel `NamespaceLifeId::stageATransition(ns)`. It has **zero production callers** — every caller
is under `src/Disks/tests/`. This was the sharpest way R1 could still be alive (a name-derived life
id is identical across rebirths, so a namespace-file path reaching it would alias by construction),
and it is not reachable from production. The note's disposition holds under the adversarial reading.

#### 2.3 The "after Task 6" wording {#t8-2-3-the-after-task-6-wording}

**CONFIRMED, with a precision the handoff's phrasing loses.** The "after Task 6" text is in the
**plan**, not in the closure note. Plan `### Task 9` says: *"Scheduling: after Tasks 4b and 6, so the
note records what landed and not what was intended"*, and its Step 1(b) instructs the author to
*"record Task 6's final verdict"*. The closure note itself contains no scheduling prose and never
mentions Task 6. So the note is not internally inconsistent; the *plan* asserts a dependency on a
task the ledger records as NOT STARTED, and the note was written and closed anyway on evidence that
landed in the 4b/4d/5 preparatory slices.

#### 2.4 Which claims might need a post-Task-6 correction {#t8-2-4-which-claims-might-need-a-post-task-6-correction}

The handoff asks for exactly this list. Ranked by likelihood of needing a change.

1. **The zero-catalog-GET claim — most likely to move.** The note says
   `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` "proves
   that steady namespace-file operations add no hot-path catalog request". Task 6's own required test
   `NamespaceFileHotPathsIssueZeroCatalogRequests` — read, write, remove *and* list through a held
   handle, with the operation journal asserting zero catalog GETs — does not exist yet
   (`gtest_cas_ns_file_read_contract.cpp` currently holds only two tests). The note's claim rests on
   a disk-profile test with a different framing. If Task 6's four-verb journal test finds a residual
   catalog GET on any verb, this sentence needs correcting. **This is the one claim I would actively
   re-check after Task 6.**
2. **Step 1(b)'s attribution.** The plan wanted Task 6's verdict recorded for the
   `namespaceFilesReadable` TOCTOU. The gate is genuinely gone (verified above), but it was removed
   by preparatory work, not by Task 6. If Task 6 changes anything about how a held handle behaves on
   `Removing`/absent, the "stale-or-`NotFound`" sentence needs re-reading. Low risk — the contract is
   already implemented and tested.
3. **Nothing else.** Task 6's remaining scope per the handoff is the ref-side read contract, the
   `resolveLifeOrSentinel` optional-return migration, the one fixture seam, and sentinel retirement.
   The closure note makes no claim about ref readers or about the sentinel, so those cannot falsify
   it. Removing the sentinel can only *strengthen* §2.2's finding.

#### 2.5 Task 9 findings {#t8-2-5-task-9-findings}

**P-3 — PROSE, FALSE.** The note cites
`CasNsFileIncarnation.OldFileHiddenByListIsInvisibleAfterRebirth`. **No such test exists.** The four
tests in `gtest_cas_ns_file_incarnation.cpp` are `ColdReaderUsesCatalogCutWhileOldFileSurvivesRemoval`,
`FreshReaderAssignsOnlyLiveCatalogLifeWithoutMutation`, `RebirthDoesNotWaitForFilesToBeEmpty`,
`LegacyUnqualifiedFileKeyIsRefusedAtOpen`. I read the first of those and it *does* perform exactly
what the note describes — it hides the life-1 key from `LIST` while keeping it physically durable,
admits life 2 under the same name, and asserts the life-1 handle is stale-or-`NotFound` and never
successor bytes. So the underlying fact is true and the citation is simply wrong. Correct the name to
`ColdReaderUsesCatalogCutWhileOldFileSurvivesRemoval`.

**P-4 — PROSE, FALSE.** The note cites
`CasNamespaceFileReadContract.StaleReaderAfterSameNameRebirthNeverSeesSuccessorBytes`. The test is
named `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`. The stale name is the one used in the
scratch artefact `.superpowers/sdd/…/task6-ns-file-contract-report.md`; the test was renamed later
(the file's history is `4048163f0dd` → `3b952c6cbde` → `224aacd8eb9`) and the closure note copied the
pre-rename name from the scratch report. The described behaviour matches the current test.

P-3 and P-4 matter more than ordinary typos because the plan's Step 1 explicitly demands "the commit
and the test that proves it — no prose-only claims". A citation that does not resolve is a prose-only
claim wearing a test's name, and it defeats the mechanism the step was built around. Four of the six
cited test names are correct; two are not.

**P-5 — PROSE, IMPRECISE.** The note says "`Layout::mountpointObjectKey` maps the loose branch to
`roots/<server_root_id>/<path>`". `Layout::mountpointObjectKey` actually returns
`prefix + "/roots/" + key` and knows nothing about server roots — the `<server_root_id>/` qualifier
is prepended by the *caller* (`ContentAddressedTransaction::writeFile` composes
`metadata_storage.serverRootId() + "/" + path`). The audit's conclusion is unaffected, since neither
the Layout nor the caller consults a namespace or catalog. But the note attributes a safety property
to the Layout that the Layout does not enforce, and a future caller that omits the prefix would put a
bare `<path>` directly under `roots/`. Worth one accurate sentence.

**P-6 — PROSE, IMPRECISE (provenance).** The note's RED-evidence sentence — "The suite's controlled
RED mutations re-resolved the catalog name on read and on buffer finalize; both tests failed for the
expected successor-alias behavior" — is sourced to
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task6-ns-file-contract-report.md`. The
handoff explicitly disclaims that file: "A scratch artifact named …task6-ns-file-contract-report.md
does not override the ledger and is not evidence that Task 6 started or completed." The report does
contain a detailed and plausible RED record (the exact mutations, the observed `life-2\n` alias, the
restored-file object hashes, and named ASan build/test logs under `build_asan/`), and I did not
attempt to re-run it. But a closure note that stakes its central read/write claim on a document the
handoff calls non-evidence should either promote that record into a durable location or cite the
underlying logs. This also explains P-4: the note inherited the scratch report's stale test name.

**Also worth surfacing, though not a Task-9 finding: the handoff's own Task-6 scope is stale.** Two
items listed as remaining Task-6 work are already done in the current tree — "Delete
`ContentAddressedMetadataStorage::namespaceFilesReadable` and rewire its five current callers" (zero
hits tree-wide) and the delayed-writer requirement that the callback "must not capture only
`RootNamespace` and resolve it again when `finalizeImpl` runs" (it captures `life` by value). And
"Delete the production fallback that silently substitutes `NamespaceLifeId::stageATransition`" is
mostly done: `resolveLifeOrSentinel` has no production callers left, so what remains is deleting the
function and migrating ~20 test call sites. Whoever picks up Task 6 should re-measure its scope
before planning it — this is the "evidence expires when placement moves" case.

#### 2.6 Task 9 verdicts {#t8-2-6-task-9-verdicts}

| Claim | Verdict |
|---|---|
| Complete in `ca07cbf87fd` | **CONFIRMED** — commit exists, both files as described, all plan checkboxes and the ledger agree |
| Contains the R1 closure note on verbatim-file rebirth aliasing | **CONFIRMED** — all four sub-hazards plus the loose-mountpoint answer |
| Closure note location in the tree | `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md`; R1's register entry at `docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md#r1-verbatim-alias` now reads "CLOSED, 2026-08-02" and links to it |
| Its substantive technical claims | **CONFIRMED against code**, and strengthened: `resolveLifeOrSentinel` has zero production callers, so the name-derived sentinel is unreachable from production |
| Prose says "after Task 6" while evidence landed early | **CONFIRMED with a correction** — that wording is in the *plan*'s Task 9 section, not in the closure note; the note contains no scheduling prose |
| Claims needing post-Task-6 correction | One likely (the zero-catalog-GET claim, §2.4 item 1), one to re-read (§2.4 item 2), the rest immune |
| Do not redo | **AGREED** — the two test-name citations (P-3, P-4) are prose fixes for the batch file, not a reopening |

---

### 3. Disposition summary {#t8-3-disposition-summary}

**Opens a fix round (CODE/TEST): T-1, T-2, T-3, C-1.** C-2 and C-3 are code-touching but belong to
other tasks (Task 14's comment sweep and Task 6's migration list respectively) — flagged so they are
not lost, not to be fixed under Task 8.

**Batch into `docs/superpowers/cas/deferred-docs-fixes.md` (PROSE): P-1, P-2 (plan text, Task 8),
P-3, P-4, P-5, P-6 (closure note, Task 9).** Two graded FALSE (P-3, P-4), four IMPRECISE.

**Needs a human decision, not a fix: Q-1** (was the untouched `cleanupStagedManifestDebrisBestEffort`
intended or a gap?).

**Q-2** is recorded for the closer's awareness only.

Test-pass counts are UNVERIFIABLE-WITHOUT-RERUN and were not rerun; the green observations quoted in
§1.5 are read out of `build/task5_step11_final_ca_rerun2.log`, a **Task-5** gate log, not a
Task-8-labelled run.

## Report: Task 10 {#report-t10}

## Task 10 audit — model debt state {#t10-task-10-audit-model-debt-state}

Read-only audit. Repo `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`,
HEAD `ce312f547c3`. Nothing was edited, staged, built or run. No TLC invocation was made, so every
model *result* below is either read out of a committed RESULTS artifact (cited) or marked
UNVERIFIABLE-WITHOUT-RERUN.

Findings are labelled CODE / TEST (here: model + runner artifacts, which are the executable
surface of this task) or PROSE. Per the standing directive, PROSE findings are batched for
`docs/superpowers/cas/deferred-docs-fixes.md` and do not open a fix round.

---

### Per-unit verdicts {#t10-per-unit-verdicts}

| Unit | Plan claim | Verdict |
|---|---|---|
| 10a | OPEN, unaudited `listedTok` premise | **CONFIRMED** |
| 10b | PARTIAL 8/9, `run_gc_partmanifest.sh` is the dirty ninth | **CONFIRMED** |
| 10c | PARTIAL: 4 runners committed, results artifact absent, stale prose | **CONFIRMED** (stale prose is in **two** places, not one) |
| 10d | COMPLETE | **CONFIRMED** with a wording correction (7 runners, not 5) |
| 10e | COMPLETE | **CONFIRMED** — both debts really discharged; witness fires on the install |
| 10f | COMPLETE | **CONFIRMED with a coverage gap** — one term of the gate formula is inert in the model and the gap is undisclosed |
| 10g | COMPLETE | **CONFIRMED** — best-evidenced unit in the set |

---

### 10a — `listedTok` semantic audit {#t10-10a-listedtok-semantic-audit}

**Verdict: CONFIRMED OPEN.**

**The premise, from the code.** `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`:

- Variable declaration (`listedTok`): *"live root-shard token discovery observes **from LIST**; any
  owner transition advances it (discovery MAY set it)"*.
- `BumpListed(n)` advances it on every owner transition when `EnableTokenDiff` is TRUE; it is inert
  (pinned at its zero init) when that constant is FALSE.
- `CanSkipShard(n)` requires `listedTok[n] = foldedTok[n] \/ SabotageSkipChangedShard`, plus the
  dead-live-precommit conjunct added later.
- `GDiscoverSkip(n)` additionally requires `TokenObservable`, then jumps the durable fold cursor to
  `Len(journal[n])` while emitting no source edges and not re-sealing `foldedTok`.

So the encoded premise is: **LIST surfaces a live root-shard token, and that LIST-derived token is
the thing compared against persisted fold coverage to decide that a shard needs no body read.** The
skip is a durable-coverage claim gated on what a listing returned. That is exactly the shape Stage B
demoted — universe-from-catalog (Task 4) and LIST-independent recovery (Task 5b), after which LIST
may only offer a newer candidate, diagnostics, or garbage nominations.

**No verdict is recorded anywhere.** Searched every `.md` under `docs/superpowers/` for `listedTok`.
Four classes of hit, none of them a verdict:

- `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` § `{#unaudited-residual}` — *names* the
  residual and explicitly says *"Whether v9's arithmetic frontier changes that model's premise (it
  very likely does …) is a real question this audit did **not** answer."*
- `docs/superpowers/worklogs/CURRENT.md` — records it as "named unaudited residual", again not a
  verdict.
- The two Codex review-finding files — they only record the *split* of Task 10 into units.
- The plan itself.

**There is no `CaGcRootLocalPartManifestCore_RESULTS.md` in the tree at all.** Fifteen `*_RESULTS.md`
files exist under `docs/superpowers/models/`; the largest battery in the tree (47 configs) is not one
of them. So the plan's instruction "Verdict into the model's RESULTS file" has no target file yet —
10a's deliverable includes creating it.

---

### 10b — driver expectations, by model family {#t10-10b-driver-expectations-by-model-family}

**Verdict: CONFIRMED PARTIAL, 8/9.**

The nine single-config drivers are enumerated in `2026-07-28-v9-phase-RESULTS.md` §
`{#fix-runners}`: `run_ackfloor`, `run_ackfloor_zombie`, `run_condemnmarker`, `run_ebo`,
`run_gc_partmanifest`, `run_relinkconfirm`, `run_retiredinrun`, `run_tlc`, `run_foldabort_witness`.

All eight claimed commits exist, are ancestors of HEAD, and each touches exactly one runner plus the
phase RESULTS file — one family per commit, as the plan required:

| commit | runner | RESULTS lines added |
|---|---|---|
| `ba8fdc6ba45` | `run_condemnmarker.sh` | 25 |
| `71ab79daa4b` | `run_ackfloor_zombie.sh` | 20 |
| `e4b34c3d6fb` | `run_ebo.sh` | 20 |
| `3d982731a8f` | `run_retiredinrun.sh` | 20 |
| `5696c2d1135` | `run_foldabort_witness.sh` | 20 |
| `43f0b878fbd` | `run_ackfloor.sh` | 31 |
| `e354def4eb0` | `run_relinkconfirm.sh` | 26 |
| `f2c1a861027` | `run_tlc.sh` (incarnation core) | 60 |

**The eight are a true partition of nine-minus-`gc_partmanifest`** — checked name by name, no
duplicate, no omission. Each converted runner now carries an expectation table and calls
`check_tlc_pin`:

```
run_ackfloor.sh              rows=19 pin=1      run_relinkconfirm.sh      rows=13 pin=1
run_ackfloor_zombie.sh       rows=4  pin=1      run_retiredinrun.sh       rows=5  pin=1
run_condemnmarker.sh         rows=2  pin=1      run_tlc.sh                rows=24 pin=1
run_ebo.sh                   rows=5  pin=1      run_foldabort_witness.sh  rows=7  pin=1
```

The pasted runner output the plan demanded is present — e.g. `43f0b878fbd` pastes the full 19-row
ack-floor tail into RESULTS, each row naming the exact invariant broken.

**The ninth family is the unstaged `run_gc_partmanifest.sh` (`git diff`, +189/-12).** It rewrites the
bare `./run_gc_partmanifest.sh <cfg>` driver into an asserted whole-suite runner for
`CaGcRootLocalPartManifestCore`:

- 43 fast rows plus a `SLOW=1` block of 5 (`stage2`, `stage3`, `stage4`, `stage5_lazytrim`, `live`),
  sabotages first, exact expected-name per row;
- five expectation kinds: `green`, `violation:<inv>`, `temporal:<prop>`, `known-model-error`,
  `incomplete`;
- sources `tlc_temporal_gate.sh`, calls `check_tlc_pin` then `check_tlc_temporal_expectations`
  (three rows are `temporal`: `sab_deletebodybeforedecrements`/`NoLeakForever`,
  `sab_noorphansweep`/`OrphanManifestDebrisDrains`,
  `sab_skipparksdeadprecommit`/`LiveDeadPrecommitReclaimed`);
- fail-closed selection: an unknown selector or a SLOW-only selector without `SLOW=1` sets
  `overall=1`;
- private per-config metadir and log keyed on a `$$`-and-nanosecond run id, so concurrent runs
  cannot collide;
- an `incomplete` row that comes back green is reported as `green (tighten expectation)` with
  verdict `KNOWN`, so an improvement cannot masquerade as a regression *or* as a silent pass.

Two things in the diff a reviewer of the eventual commit should look at hard, flagged here but not
adjudicated (running the battery is out of my read-only remit):

1. **TEST — three rows expect `known-model-error UnchangedCompositeVars`**
   (`sab_crosssharddisplacement`, `sab_reducerownsfence`, `stage5_sharding`). Those three sharding
   configs do not check the model — they pin a *recorded defect in the model itself* (`origVars`
   composite-`UNCHANGED` in `GReduceShard`). The runner is careful about it: it greps the module for
   the two provenance line numbers and refuses to accept the error unless TLC's message cites both,
   so an unrelated parse error cannot pass as this known one. But the net effect is that
   `CaGcRootLocalPartManifestCore`'s entire sharding arm (Phase 4, R2 target-sharded reducers) is
   currently unproven, and the runner records that as `KNOWN` rather than as a failure. That is the
   honest accounting, but it is a standing model debt that the plan does not name anywhere, and it
   should not disappear into a `KNOWN` column.
2. **TEST — the `temporal` classifier requires the property name to be declared in the cfg**
   (`grep -Eq "^PROPERTY[[:space:]]+${want}$" "$cfg"` alongside the TLC message). Good: the label
   cannot go stale behind a property nobody updated. Worth confirming at review time that all three
   temporal cfgs use `PROPERTY` and not `PROPERTIES`, or those rows fall through to `error`.

**No RESULTS record for the ninth family exists.** `2026-07-28-v9-phase-RESULTS.md` has a per-family
`{#…-whole-suite}` section for the converted families; `grep` for `gc_partmanifest` in it returns
only the pre-conversion mentions at the `{#fix-runners}` scope-boundary paragraph. Confirmed absent.

---

### 10c — runnerless models {#t10-10c-runnerless-models}

**Verdict: CONFIRMED PARTIAL.**

- `5cbd1b7c4c8` "ca: tla — runners for the four models that had none" exists, is an ancestor of HEAD,
  and adds exactly four files: `run_b140danglemerge.sh`, `run_gclease.sh`, `run_gcrounddefer.sh`,
  `run_gcshardincarnation.sh` (+216 lines, no deletions).
- `5a5e32618af` exists and is the merge that brought the TLA lane in
  (parents `9318862bd5d` and `6fccf5a4db2`).
- All four runners are present in the working tree and are asserted suites (named expectation per
  row, sabotage first). `run_b140danglemerge.sh` explicitly enumerates the `m_*.cfg` names with the
  reason stated in a comment — *"a conventional model-prefixed glob silently misses this whole
  proof"* — which is the right shape.

**The required before/after results artifact is absent.** No `*_RESULTS.md` records a run of these
four runners. `CaGcRoundDeferCore_RESULTS.md` and `CaGcShardIncarnationCore_RESULTS.md` predate the
runners (`fe995fa8678`, `c5f50a7ab6a`/`2c8f03a6996`) and record inline TLC invocations; neither
mentions its runner. `CaGcLeaseCore` and `CaB140DangleMerge` have no RESULTS file at all. The only
recorded execution of any of the four is a single 10g row for
`run_gcrounddefer.sh` / `sab_unbounded_defer` in the empty-set survey.

**PROSE (FALSE) — the stale "no runners" prose is in TWO places, not the one the plan names.**

1. The one the plan means: `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md`, §
   `{#fix-runners}`, closing paragraph:

   > "Three further models have no runner at all (`CaGcLeaseCore`, `CaGcRoundDeferCore`,
   > `CaGcShardIncarnationCore`) plus `CaB140DangleMerge`, whose four configs use an `m_*.cfg` prefix
   > instead of the usual `<Model>_*.cfg` one (so a conventional glob misses them). Recorded as the
   > follow-up, not silently skipped."

2. **Not named by the plan:** `docs/superpowers/models/README.md` § `{#summary-table}` still carries
   `(inline TLC)` in the **Runner** column for all four rows — `CaGcLeaseCore.tla`,
   `CaGcShardIncarnationCore.tla`, `CaGcRoundDeferCore.tla`, and
   `` `CaB140DangleMerge.tla` (+ `m_*.cfg`) ``. This is the table a reader consults to find a
   runner, so it is the more consequential of the two.

**PROSE (FALSE) — a third stale site, from 10b/10g rather than 10c:**
`docs/superpowers/cas/06-tla-models.md` § `{#running-models}` says the TLC jar is *"(v2.19)"* — the
build 10g rejected — and says *"The rest (`run_tlc.sh`, `run_gc_partmanifest.sh`, `run_ackfloor.sh`,
…) are single-config drivers: they take a cfg as `$1`, run it, and assert nothing."* Two of those
three are now asserted suites (`f2c1a861027`, `43f0b878fbd`); its suite-runner list names none of the
eight converted families nor any of the four 10c runners; and the hand-rolled shell loops that follow
are the very expectations 10b was meant to replace. `{#fix-runners}` itself points at this doc —
*"that doc is where the work starts"* — and it was never updated.

**TEST (minor) — three of the four 10c runners accept any jar.** `run_gclease.sh`,
`run_gcshardincarnation.sh` and `run_b140danglemerge.sh` hardcode `JAR=../../../tmp/tla2tools.jar`
and only test `[[ -f "$JAR" ]]`; they neither source `tlc_temporal_gate.sh` nor call
`check_tlc_pin`. (`run_gcrounddefer.sh` does both — 10g reached it.) None of the three has a
`temporal` row, so the *temporal* defect cannot bite them, but the README's claim that "a safety-only
table cannot keep using the former jar" is scoped to the five Task 10g runners only, and these three
are outside it. Cheap to close at the same time as 10c's results artifact.

---

### 10d — phase-runner classifier {#t10-10d-phase-runner-classifier}

**Verdict: CONFIRMED COMPLETE**, with one wording correction.

`0382d5737b0` "ca: tla — the runner classifier can name a digit-bearing invariant" exists, is an
ancestor of HEAD, and is a pure one-line-per-file change to **seven** runners: `run_deltaintake.sh`,
`run_mount.sh`, `run_nscleanup_staleleader.sh`, `run_refcatalog.sh`, `run_reflane.sh`,
`run_refsnaplog.sh`, `run_relinklane.sh` (7 files, +7/-7).

**PROSE (IMPRECISE) — the plan body says "the five phase runners"; the commit fixes seven.** The
execution-table row is the accurate one ("all affected runners"). Not a defect in the work, only in
the plan text.

**Sweep confirms the fix is exhaustive:** `grep -l 'A-Za-z_\]+' docs/superpowers/models/*.sh` returns
nothing — no runner in the tree still carries the digit-blind pattern. That is the check that matters,
and it passes.

The re-run claim ("all five were re-run afterwards so the recorded output comes from the scripts as
committed") is recorded in `2026-07-28-v9-phase-RESULTS.md` § `{#regex-digits}` and is
UNVERIFIABLE-WITHOUT-RERUN beyond that prose; the RESULTS master table is the artifact standing
behind it.

---

### 10e — the lane battery's two witness debts {#t10-10e-the-lane-battery-s-two-witness-debts}

**Verdict: CONFIRMED COMPLETE.** Both debts are genuinely discharged, and the second one passes the
"does it check what its name claims" test.

**Debt (1), `0665d3fd6fb`** deletes the over-claiming phrase rather than rewriting it, exactly as the
plan preferred: `CaRefLaneCore_RESULTS.md` loses "retry-created adoption" from the witness list. The
flag `saw_retry_created` is indeed set in the durable-observation action
(`saw_retry_created' = (saw_retry_created \/ (durable_id = resolver_attempt.id - 1))`), not at any
install, so the removed claim was correctly identified as unsupported.

**Debt (2), `76ba4adb5a9`** adds `saw_durable_adoption`, `W_DurableAdoption == ~saw_durable_adoption`,
the config `CaRefLaneCore_witness_durableadoption.cfg`, and the runner row
`"witness_durableadoption violation W_DurableAdoption"` in `run_reflane.sh`.

**Traced the setter to confirm the witness names what it proves.** `saw_durable_adoption' = TRUE`
appears exactly once in `CaRefLaneCore.tla`, inside `ApplyResolution`, in the branch
`SameResolution /\ observation = "Durable" /\ (CurrentRuntime \/ SabotageNoFence)` — the same
conjunction that performs `lane' = "Ready"`, `cache_id' = resolver_attempt.id`,
`cache_binding' = resolver_attempt.binding`. `SameResolution` requires `lane = "Wedged"` together
with exact attempt and generation identity. So the flag fires on the **`Wedged → Ready` adoption
install itself**, not on durability observation — which is precisely the arm debt (1) showed was
previously unwitnessed. The two fixes are complementary, not a rename of the same thing.

The re-run of `run_reflane.sh` (15/15 → 16 rows) is UNVERIFIABLE-WITHOUT-RERUN; the runner row is
committed and the config is well-formed (`W_DurableAdoption` is in its `INVARIANTS` line, so a
"violation" is the reachability report the witness convention expects).

---

### 10f — destructive gate + empty-set blind spot {#t10-10f-destructive-gate-empty-set-blind-spot}

**Verdict: CONFIRMED COMPLETE, with one coverage gap that is not disclosed.**

Both commits exist and are ancestors of HEAD. `fd4959e29b8` adds `CaGcDestructiveGateCore.tla`,
`CaGcDestructiveGateCore_RESULTS.md`, `run_destructive_gate.sh` and 13 configs; `fef19e98723`
tightens the correspondence text across the RESULTS, README and empty-set survey.

**Model files:** `docs/superpowers/models/CaGcDestructiveGateCore.tla` plus its 13
`CaGcDestructiveGateCore_*.cfg` configs, driven by
`docs/superpowers/models/run_destructive_gate.sh`. The empty-universe blind-spot half of 10f is
recorded in `docs/superpowers/models/2026-07-30-empty-set-survey.md`.

**Recorded result** — `CaGcDestructiveGateCore_RESULTS.md` § `{#exact-runner-tail}` pastes a full
13-row tail, all PASS, under the pinned official jar with `TEMPORAL SMOKE PASS`:
5 sabotages red by exact property name, 5 honest configs green, 3 non-vacuity witnesses red as
required. Checker identity is recorded (TLC `2026.07.18.145032`, rev `30cc360`, SHA-256
`cc4803…e516b3`, 1 worker, BFS). This is a properly evidenced unit.

**It answers the vacuous-comparison question correctly, which is the point of the unit.** Production
(`Gc/CasGc.cpp`, `Gc::fold`) computes

```
result.frontier_complete = universe_authoritative
    && result.frontier_namespaces > 0
    && result.frontier_proven == result.frontier_namespaces;
```

and the model separates the empty-universe floor from the equality on purpose —
`ComputedPhysicalGateOpen` has `(CatalogUniverse # {} \/ SabotageGateAcceptsEmptyUniverse)` as its
own conjunct with its own control, with the comment *"The empty-set control is intentionally distinct
from frontier equality: `{}` equals `{}` while a destructive round must still fail closed."* The
sabotage `sab_gate_accepts_empty_universe` drops that floor and goes red on
`PhysicalDeleteOnlyWhenGateOpen`. This is exactly the "does the comparison hold vacuously when both
counts are zero" case, and it is modelled, controlled and recorded.

**TEST — the gap: `UniverseAuthoritative` is a constant `TRUE` with no sabotage.**
`CaGcDestructiveGateCore.tla` defines `UniverseAuthoritative == TRUE` and conjoins it into both
`FrontierComplete` and `ComputedPhysicalGateOpen`. It is inert — removing it would not change a
single reachable state, and there is no `sab_gate_omits_authoritative`. So of the four terms of the
production `frontier_complete` formula, **three are proven load-bearing by a control and one is
not exercised at all.** In the current code that term is `policy == UniversePolicy::AuthoritativeForTest`,
i.e. *false in production* — the source comment says so outright ("anything else — which in
production is the only possibility — refuses"). The model therefore describes the **post-Task-7b**
posture, which is defensible given the stated `Task 5 → 10f → 7b` ordering, but it means 10f does not
gate today's code path and cannot catch a regression in the universe seam.

**PROSE (IMPRECISE) — the gap is nowhere stated.** The RESULTS § correspondence section quotes the
four-term production formula, then says *"`FrontierComplete` and `PhysicalGateOpen` mirror these
later-fold formulas using finite sets"* and maps only `frontier_namespaces > 0` and the equality. It
never says `universe_authoritative` is pinned TRUE, has no control, and is currently false in
production. The `.tla` line carries no comment either. The verdict section's *"The four physical gate
conjunct controls … all five honest safety configurations are green"* is literally true (there are
four controls) but reads as term-exhaustive when the term set is five. Recommended fix: one sentence
in the RESULTS correspondence section and one short comment at `UniverseAuthoritative`, stating that
the term is pinned TRUE because the model gates the posture Task 7b creates, and naming Task 7b's own
independent-assertion requirement as where that term gets its coverage — worded as the reason, without
citing the plan.

---

### 10g — the broken TLC jar and the three temporal rows {#t10-10g-the-broken-tlc-jar-and-the-three-temporal-rows}

**Verdict: CONFIRMED COMPLETE.** The strongest-evidenced unit in the set.

`6fccf5a4db2` adds `TlcTemporalSmoke.tla`/`.cfg`, `tlc_temporal_gate.sh`, and wires the gate into the
three runners that carry a `temporal` expectation. `4e1ae9b64de` adds `check_tlc_pin` (SHA-256 pin,
not a version banner), `test_tlc_temporal_gate.sh`, and updates README plus the survey.

All four plan checkboxes verified against artifacts:

- **Re-validation of the three temporal rows** — `2026-07-30-empty-set-survey.md` records a
  both-jars table for `CaBuildRootPrecommit/lazyleak` → `INV_NO_LEAK`,
  `CaDiskLifecycle/sab_nogcselfexit` → `GcExitsAfterVanished`,
  `CaGcRoundDeferCore/sab_unbounded_defer` → `EventuallyFolded`, each violating under both jars, plus
  a focused revalidation table with state counts, depth and exit codes. So none of the three was
  green because the checker violates everything. Recorded, not merely asserted.
- **Smoke adopted as a gate, not a note** — `check_tlc_temporal_gate` refuses a verdict if
  `<> TRUE` violates *or* if the smoke does not complete (both directions, so a crashed smoke cannot
  pass as silence). `check_tlc_temporal_expectations` scans the runner's own expectation table and
  fires the smoke only if a `temporal` row exists, which makes the gate self-arming: adding a future
  temporal row activates it with no runner edit. Nine runners call it, including the two that
  currently have no temporal row (`run_foldclamp.sh`, `run_refwcleanup.sh`) — the structurally right
  choice, and the survey states the reasoning.
- **No `ASSUME Builds # {}` hack** — no such `ASSUME` was added; `grep` finds the empty-set configs
  instead.
- **Jar decision recorded where runners can see it** — one jar for both safety and temporal, pinned
  by digest in `tlc_temporal_gate.sh` and documented in `models/README.md`, with the reason for
  rejecting 2.19 spelled out. Eighteen runners call `check_tlc_pin`.

**Environment note, not a repo defect, but it blocks the remaining 10b/10c work in this workspace:**
`tmp/tla2tools.jar` is still a symlink to the TLAToolbox 1.7.4 jar, SHA-256
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88` — not the pinned
`cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`. Every runner that calls
`check_tlc_pin` will exit 3 here until the official jar is placed at that path. Whoever runs the
ninth 10b family or 10c's before/after battery must fix the symlink first, or the run will look like
a harness failure. (10f and 10g both worked around this with an overlaid
`tmp/tla2tools-official.jar`, which is recorded in their artifacts.)

---

### Summary of findings {#t10-summary-of-findings}

**TEST / model (would open a fix round):**

1. 10f — `UniverseAuthoritative == TRUE` is inert in `CaGcDestructiveGateCore.tla`; one of the four
   production `frontier_complete` terms has no control. Coverage gap, not an error.
2. 10c — `run_gclease.sh`, `run_gcshardincarnation.sh`, `run_b140danglemerge.sh` do not call
   `check_tlc_pin`; they will run under any jar.
3. 10b (pending ninth family) — three `known-model-error UnchangedCompositeVars` rows mean
   `CaGcRootLocalPartManifestCore`'s whole sharding arm is currently unproven and booked as `KNOWN`.
   The provenance grep makes the acceptance honest, but the debt is unnamed in the plan.

**PROSE (batch to `deferred-docs-fixes.md`, no fix round):**

1. FALSE — `models/2026-07-28-v9-phase-RESULTS.md` § `{#fix-runners}`: "Three further models have no
   runner at all … plus `CaB140DangleMerge`". All four have runners since `5cbd1b7c4c8`.
2. FALSE — `models/README.md` § `{#summary-table}`: `(inline TLC)` in the Runner column for
   `CaGcLeaseCore`, `CaGcShardIncarnationCore`, `CaGcRoundDeferCore`, `CaB140DangleMerge`.
3. FALSE — `cas/06-tla-models.md` § `{#running-models}`: jar described as "(v2.19)" (rejected by
   10g); `run_tlc.sh` and `run_ackfloor.sh` described as asserting nothing (both converted by 10b);
   suite-runner list omits every 10b and 10c runner; the hand-rolled expectation loops that follow
   are what 10b replaced.
4. IMPRECISE — `CaGcDestructiveGateCore_RESULTS.md` § correspondence: claims the model "mirrors" a
   four-term production formula without disclosing that one term is pinned TRUE, uncontrolled, and
   false in production today.
5. IMPRECISE — the plan's Task 10d body says "the five phase runners"; the commit correctly fixes
   seven. The execution-table row is already accurate.

**Also noted, outside Task 10's scope but inside the standing comment policy:** the `frontier_complete`
comment in `Gc/CasGc.cpp` cites an internal document by path and anchor
(`docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md {#r11-empty-universe-vacuous}`).
That is a dangling reference the moment the branch drops that file. The *reason* the comment gives —
`0 == 0` is vacuously true, so a nonzero floor is needed to fail closed — is excellent and should
stay verbatim; only the citation should go.

## Report: Document consistency and foundation {#report-docs}

## Stage B document-consistency audit + foundation spot-check {#docs-stage-b-document-consistency-audit-foundation-spot-check}

Read-only audit. Repo `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`,
HEAD `ce312f547c3` (verified at audit time; working tree unchanged by this audit).

Subjects:
- `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` (3064 lines, read in full)
- `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/progress.md` (the ledger)
- `tmp/task6_handoff_context.md` (the handoff)

Findings are labelled **CODE/TEST** or **PROSE**, per the review contract. PROSE findings are graded
FALSE or IMPRECISE and belong in `docs/superpowers/cas/deferred-docs-fixes.md`; they do not open a fix
round. Note that in this dispatch almost everything found is PROSE **by construction** — the subject is
three documents — but three of the PROSE findings describe work a future executor would REDO or SKIP,
which is a materially different cost from a wording nit, and those are called out as such.

---

### 1. Three-way consistency table, Tasks 0–14 {#docs-1-three-way-consistency-table-tasks-0-14}

Per the ledger's own note, unchecked historical checkboxes on completed tasks are expected and are not
flagged. Only contradictions and material silences are listed.

| Task | Plan says | Ledger says | Handoff says | Verdict |
|---|---|---|---|---|
| 0 | no status marker; 3 unchecked steps; Step 1 note records the Stage-A verdict flipped to PASS at `3f7b35c7ce1` | "foundation Tasks 0–4d … through `d278d130024`" | "foundation Tasks 0–4d: complete" | consistent |
| 1 | **EXECUTED** 2026-07-29 (`34d607e72ae` + `67dd2666e75`), merged `9f9514a90f1` | foundation | foundation | consistent |
| 1b | **✅ REDO COMPLETE** (`3e228272dd3`, `8d536022eab`, `ec3a73656ea`, `eeb194af24b`) | foundation | foundation | consistent (see §5 for the stale *pre*-redo sections that still sit above it) |
| 1c | no status marker anywhere | foundation | foundation | **silent in plan** — no "EXECUTED/LANDED" line, unlike 1/1b/4d/5/5b. A reader who trusts the plan alone cannot tell 1c ran. |
| 2 | no status marker | foundation | foundation | same silence as 1c |
| 3 | no status marker | foundation | foundation | same silence as 1c |
| 4 | no status marker on the task; the SPLIT section marks 4-A LANDED, 4-B DONE, 4-C unmarked | foundation | foundation | **contradiction-adjacent**: substep 4-C carries no completion mark, so the plan itself does not assert Task 4 finished; ledger/handoff do |
| 4b | no status marker | foundation | foundation | silent in plan (landed as `827bc0a9189`, verified) |
| 4c | no status marker | foundation | foundation | silent in plan |
| 4d | **LANDED, `6a3dd6a9245`**; Steps 0–7 all `[x]` | foundation | foundation | consistent |
| 5 | **COMPLETE** 2026-08-02; all steps `[x]` | COMPLETE | complete | consistent |
| 5b | **COMPLETE** 2026-08-02; all steps `[x]` | COMPLETE | complete | consistent |
| 6 | no status marker; every step unchecked | not named individually; only inside "M2 … remaining Tasks 6 and 6b" | **NOT STARTED** | **CONTRADICTED BY CODE** — see §3.1. Two of Task 6's three required namespace-file tests are committed (`4048163f0dd`, `3b952c6cbde`), and Task 9's own closure note cites them as evidence. |
| 6b | no status marker; steps unchecked; prose reads as if the rename is pending | not named individually; only inside M2 | **NOT CLOSED**; "target spelling already exists in current code" | **CONTRADICTED BY CODE** — see §3.2. Commit `9d92c84ee37` carries Task 6b's *exact* required commit subject and performed the rename. |
| 7 | **IMPLEMENTATION PRESENT, CLOSURE NOT YET EVIDENCED** (`224aacd8eb9`) | same | same | consistent (all three agree) |
| 7a | no status marker | **never mentioned individually**; appears only in "M3: Tasks 7, 7a, 7b, 8" | **NOT IMPLEMENTED** | ledger silent; handoff verified correct against code (§4) |
| 7b | no status marker | **never mentioned individually**; only in M3 | **NOT IMPLEMENTED** | ledger silent; handoff verified correct against code (§4) |
| 8 | **MODEL GATE COMPLETE; PRODUCTION IMPLEMENTATION PRESENT; TASK NOT CLOSED**; Step 1 `[x]`, Steps 2–5 unchecked | PARTIAL, same commits | MODEL COMPLETE + slices present | consistent |
| 9 | **COMPLETE (`ca07cbf87fd`)** — *and* still carries its unamended "Scheduling: after Tasks 4b and 6" prose, and the overview row still reads `Depends on 4d,6` | COMPLETE | COMPLETE, with an explicit note that its scheduling prose says "after Task 6" | **plan-internal contradiction**: marked COMPLETE while the plan's own dependency column says it depends on Task 6, which all three documents call unstarted. Only the handoff manages this. |
| 10a | **OPEN** | 10a OPEN | OPEN | consistent |
| 10b | **PARTIAL — 8/9**, ninth called "an uncommitted ninth family" | 8/9 committed; ninth needs battery/RESULTS/commit | "unfinished ninth Task 10b family; do not stage it" | consistent in substance; **"uncommitted" is FALSE** — the file is tracked (§3.3) |
| 10c | **PARTIAL** — runners exist (`5cbd1b7c4c8`/`5a5e32618af`), results artifact absent | same | same | consistent |
| 10d/e/f/g | **COMPLETE** with commits | COMPLETE | COMPLETE | consistent |
| 11 | no status marker; all steps unchecked | inside "M5: Tasks 11–12" | OPEN FINAL GATE | consistent |
| 12 | no status marker | inside M5 | OPEN; strictly after 11 | consistent |
| 13 | no status marker; overview row exists (`depends on 12`) | "Tasks 13–14 … post-Stage-B/before-upstreaming" | POST-STAGE-B; after Task 12 | consistent |
| 14 | **has a section but NO row in the task-overview table** | grouped with 13 | BEFORE UPSTREAMING | **plan-internal gap** (§5.6) |

#### Summary of true three-way contradictions {#docs-summary-of-true-three-way-contradictions}

Only two rows are contradictions in the strong sense (a document asserts a state the code refutes),
and both point the same way — **the plan and the ledger understate how much of Tasks 6 and 6b has
already landed**:

1. Task 6 — "NOT STARTED" (handoff) / silent (plan, ledger) vs. two committed contract tests.
2. Task 6b — silent (plan, ledger) / "not closed, spelling exists" (handoff) vs. a dedicated commit
   with Task 6b's own required subject line.

Task 9 is a third, weaker case: an internal ordering contradiction rather than a code contradiction.

---

### 2. Foundation Tasks 0–4d: documentary check {#docs-2-foundation-tasks-0-4d-documentary-check}

**Every commit named anywhere in the three documents exists and is an ancestor of `ce312f547c3`.**
57 SHAs were checked (`git cat-file -e` + `git merge-base --is-ancestor`); zero missing, zero
non-ancestor. That includes the whole Task 1/1b chain, `6a3dd6a9245` (4d), the Task 5 chain
`bf396ffa50d → 2a5bb563b26 → 8fb37da10b0 → 95ae0d54a54 → 54aa4812450 → 111bb12a407 → 224aacd8eb9 →
765c50b7cb93`, the twelve-commit Task 5b chain, `ca07cbf87fd` (9), `d34aa06d89f`/`c3cc24c8152`/
`8f14bc119fe` (8), all eight Task 10b family commits, and the Task 10c–10g commits.

**One naming defect in the ledger's foundation anchor — PROSE, IMPRECISE.**
The ledger says "Tasks 0–4d are represented by the Stage-B commit chain through `d278d130024`".
`d278d130024`'s subject is **`Task 5: remove synchronous _ckpt deletion`** — a Task 5 commit, not the
Task 4d boundary. Task 4d landed at `6a3dd6a9245`, which is four commits earlier
(`6a3dd6a9245 → 2b8475fc6f6 → 21ce9e99f4d → 00ebd0c1f31 → d278d130024`). The statement is *technically*
true read as "everything up to and including", but as written it labels a Task-5 commit as the
foundation's end, and the two commits in between (`2b8475fc6f6` "make `cas/ref_catalog` fail closed",
`21ce9e99f4d` "make catalog bootstrap atomic") are catalog work the ledger silently folds into
"Tasks 0–4d". Correct anchor for "foundation through Task 4d" is `6a3dd6a9245`.

**Milestone-boundary observation:** the plan's Task 4 split section marks 4-A LANDED and 4-B DONE but
leaves 4-C — "the wiring and the discovery", which carries the R11 empty-universe fail-closed guard and
obligation 3's production-path pin — with **no completion mark at all**. Task 4-C's content is
load-bearing (it is where the vacuous `0 == 0` frontier guard lands), and the ledger's blanket
"Tasks 0–4d complete" is the only assertion that it ran. Recommend a future executor confirm 4-C's
guard exists in code before Task 7b flips `kDefault`; this audit did not verify it (out of dispatch
scope, and Task 7b's own Step 1b re-asserts the formula).

---

### 3. Foundation spot-checks against code (the load-bearing ones) {#docs-3-foundation-spot-checks-against-code-the-load-bearing-ones}

#### 3.1 Task 6 is not "NOT STARTED" — CODE/TEST-adjacent, filed as PROSE (FALSE) in three documents {#docs-3-1-task-6-is-not-not-started-code-test-adjacent-filed-as-pr}

`src/Disks/tests/gtest_cas_ns_file_read_contract.cpp` is **tracked and committed**, added by
`4048163f0dd` ("Run namespace janitor during deferred GC", 2026-08-02) and extended by `3b952c6cbde`
("Test namespace-file life binding"). It contains two of the three tests the plan's Task 6 Step 1b
requires:

- `CasNamespaceFileReadContract.HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`
- `CasNamespaceFileReadContract.DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes`

The third, `NamespaceFileHotPathsIssueZeroCatalogRequests` (the zero-catalog-GET journal assertion,
which is the read-side half of Constraint 16), is **absent** — a tree-wide grep for
`ZeroCatalogRequest|ZeroCatalogGet` in `src/Disks/tests` returns nothing.

The Task 9 closure note `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md`
(committed as `ca07cbf87fd`) **cites both tests, cites their controlled RED mutations, and cites
`.superpowers/sdd/.../task6-ns-file-contract-report.md` as its evidence** — the very artifact the
handoff instructs the reader to disregard ("does not override the ledger and is not evidence that
Task 6 started or completed").

Why this matters to a future executor, and why it is not a wording nit: a Task-6 implementer who
believes "NOT STARTED" will write those two tests again under the plan's names
(`StaleReaderAfterRebirthNeverSeesNewIncarnation`,
`DelayedWriterFinalizedAfterRebirthWritesOnlyItsOwnIncarnation`) and end up with two pairs of
near-duplicate tests, or will delete the committed ones as strays. The honest statement is: **Task 6's
namespace-file read/rebirth half is partly landed and evidenced; its zero-catalog-GET pin, the ref-side
contract, the `resolveLifeOrSentinel` optionalisation, the snapshot-seam threading, the fixture seam
and the `stageATransition` retirement are not.**

Corroborating measurements for the "not" half, all taken at HEAD:

- `CasRefCatalog::resolveLifeOrSentinel` still exists (`Pool/CasRefCatalog.h`, `Pool/CasRefCatalog.cpp`)
  and still returns a non-optional `NamespaceLifeId` with the sentinel fallback.
- Its remaining callers are **test-only** (18 call sites under `src/Disks/tests`); the only production
  mentions are two comments in `Gc/CasGc.{h,cpp}` warning against it. So the plan's "24 sites, several
  inside per-namespace loops" figure is stale in the direction that *helps* — the production
  re-read problem it describes has largely already been eliminated by Task 5's snapshot seam. Re-derive
  the count at execution rather than trusting the plan's number.
- `stageATransition`: 295 build-input hits tree-wide, 291 of them under `src/Disks/tests`; production
  hits are the definition in `Primitives/CasNamespaceLifeId.h` and the single use inside
  `resolveLifeOrSentinel` (`Pool/CasRefCatalog.cpp`). Task 6's zero-grep gate is therefore almost
  entirely a *fixture* migration, not a production one — which is the opposite of how the plan's
  Step 1c reads.

#### 3.2 Task 6b's rename already landed, under Task 6b's own commit subject — PROSE (FALSE) in the plan {#docs-3-2-task-6b-s-rename-already-landed-under-task-6b-s-own-comm}

`git log -S` shows commit **`9d92c84ee37`** (2026-08-02, ancestor of HEAD):

```
ca: ref — `tryPublishSnapshotAndAdvanceCheckpointOnce`: the name states both durable effects
```

That is character-for-character the commit subject the plan's Task 6b Step 4 requires. The commit
touches `CasRefLedger.{h,cpp}`, `CasPool.{h,cpp}`, `CasRefCowMap.h` and three gtest files;
`trySnapshotPublishOnce` no longer appears anywhere in the tree.

The plan's Task 6b section is unamended and reads as pending throughout — its Files block still cites
`CasRefLedger.h:190` / `.cpp:3440` / `CasPool.h:540` as things "to modify", and Step 2 still says
"Rename (or split under the constraint above)". A future executor following the plan would attempt a
rename that has already happened.

**The genuinely open part of Task 6b, and it is the important part.** Step 1 required locating the
existing three-step ordering coverage *before* renaming, and creating
`src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp` if the ordering was unasserted. That file
**does not exist**, and the rename shipped anyway. So the specific protection Step 1 was written to
provide — "a rename must not be the change that quietly loses an assertion" — was not exercised in the
order it was designed for. The handoff's remaining-work entry for 6b ("audit the existing three-effect
ordering coverage … add any missing tests **before further rename/cleanup**") is right in substance and
stale in framing: the audit is now *retrospective*, done against a tree the rename has already passed
through, which is a weaker instrument than the pre-rename audit the plan specified. That difference
should be stated explicitly wherever 6b is next dispatched.

#### 3.3 `run_gc_partmanifest.sh` is tracked, not "uncommitted" — PROSE (FALSE), minor {#docs-3-3-rungcpartmanifest-sh-is-tracked-not-uncommitted-prose-fa}

Plan: "`run_gc_partmanifest.sh` is an uncommitted ninth family". Ledger: "the local
`run_gc_partmanifest.sh` ninth family still needs its battery, RESULTS record and commit". Handoff:
"the unfinished ninth Task 10b family; do not stage it".

The file is tracked (`git ls-files` matches; first committed in `0129db0050d`) and currently carries
**uncommitted modifications** (` M`). "Uncommitted family" reads as "an untracked new file"; the truth
is "a tracked runner with unstaged edits". The operational instruction ("do not stage it until its
battery and RESULTS record are done") is unaffected and correct.

#### 3.4 `namespaceFilesReadable` / `namespaceIsRemoved` are already gone — PROSE (FALSE) in plan and handoff {#docs-3-4-namespacefilesreadable-namespaceisremoved-are-already-go}

Both symbols return **zero** hits tree-wide. `git log -S` attributes the removal to **`827bc0a9189`**
("ca: ref — namespace files keyed by incarnation; rebirth waits for no file"), i.e. **Task 4b**, not
Task 5 and not Task 6.

Three documents still describe this as future work:

- Plan Task 6 Files: "delete `namespaceFilesReadable` (`:1231`, body `return !store()->namespaceIsRemoved(ns);`)
  and rewire its five call sites (`:1325` …, `:1498` …, `:1647` …, `:1695` …, `:1846` …)".
- Plan Task 6 Interfaces: "`namespaceFilesReadable`'s disposition, decided here and recorded by Task 5.
  Delete it with the `Removed` snapshot."
- Handoff Task 6 row and §5: "remove `namespaceFilesReadable`"; "Delete
  `ContentAddressedMetadataStorage::namespaceFilesReadable` and rewire its five current callers."

A Task-6 executor will grep for a symbol that does not exist. The R1 closure note gets this right
("absent from the current tree"), so the correct fact is already recorded — in the one document that is
not the plan or the handoff.

Note also that Task 5 Step 9's checked box says "Delete `namespaceIsRemoved`", which by the `git log -S`
evidence was already gone by then. That is a checked step claiming a deletion another task performed —
PROSE, IMPRECISE, and the kind of mis-attribution the plan's own Task 1b post-mortem warns about
("every false claim across those rounds was an attribution to a location the author was not reading at
the time").

#### 3.5 Task 7a / 7b really are unstarted — handoff verified CORRECT {#docs-3-5-task-7a-7b-really-are-unstarted-handoff-verified-correct}

Confirming the handoff rather than contradicting it, because a dependency map is only as good as its
endpoints:

- Probe A is fully present: `sampleRefListQuality`, `ref_list_probe`, `gc_probe_a_period` and the six
  `CasGcProbeA*` ProfileEvents still exist across `Gc/CasGc.{h,cpp}`, `Pool/CasPool.h`,
  `src/Common/ProfileEvents.cpp`, `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`,
  `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md`,
  `utils/ca-soak/soak/signals.py`, `gtest_cas_holey_list_detector.cpp`,
  `gtest_cas_retirement_sweep.cpp`, `gtest_cas_gc_log.cpp`.
- `UniversePolicy::kDefault = StageA_Suppressed` (`Gc/CasGc.h`), `AuthoritativeForTest` unrenamed.
- All five `STAGE-A RETURN ITEM` markers are live (`Gc/CasGc.h`, `gtest_ca_wiring.cpp`,
  `gtest_cas_gc_log.cpp`, `tests/broken_tests.yaml`, `04290_content_addressed_no_leftovers.sh`), and all
  three `broken_tests.yaml` entries (`05008_ca_gc_snap_prune`, `04290_content_addressed_no_leftovers`,
  `04295_content_addressed_mutation_no_leftovers`) remain.
- Task 5b's `[RECOVER-REF-TABLE-LIST-RESIDUAL]` closure IS recorded in `BACKLOG.md`
  (`{#recover-ref-table-list-residual-closed}`, naming the commit chain), so Task 7b's documented
  BLOCK-until-CLOSED precondition is genuinely discharged.

---

### 4. Dependency map: handoff vs. the old plan's parallel-execution-lanes section {#docs-dependency-map}

**The handoff and the plan disagree about Task 6's place in the critical path, and the handoff is right.**

The plan's `{#parallel-execution-lanes}` opens with:

> The critical path remains `4d → 5 → (7 ∥ 5b) → 10f → 7a → 7b → 11`

That path **omits Task 6 entirely**, while the plan's own task-overview table lists Task 7b's
dependencies as `4d,5,6,7,7a,10f` — Task 6 included — and Wave P3 separately says Task 7a "may run
beside Task 6's reader/namespace-file implementation" without saying Task 6 must precede 7b. So the
plan contradicts itself in two places about whether Task 6 gates the destruction flip.

The handoff resolves it correctly and explicitly:

> destructive-enable lane: Task 7b waits for **both** `Task 6` and `Task 7 closure → Task 7a`

This matters: if a future executor works from the plan's critical-path line, Task 7b — the one
irreversible-deletion enablement in the stage — lands without the read-side pre-delete revalidation
contract that Task 6 exists to build. Recommend the plan's critical-path line be corrected to
`4d → 5 → (7 ∥ 5b) → 6 → 10f → 7a → 7b → 11` (or at minimum annotated with the Task-6 join).
**PROSE, IMPRECISE — but with a correctness consequence, so it should be fixed in the plan and not
only batched.**

Other lane comparisons, all consistent:

| Lane | Handoff | Plan | Verdict |
|---|---|---|---|
| read-side | `6 → 6b → 11` | overview: 6b depends `4c,6`; Wave P3 "Task 6b waits for Task 6" | agree |
| 6b vs 7a/7b | "6b is **not** a 7b predecessor; may proceed beside 7a/7b after Task 6" | overview 7b deps omit 6b; Wave P3 puts 6b single-owner in `CasRefLedger` | agree (plan's *recommended order* lists 6b before 7a, but that is an order suggestion, not a dependency) |
| model closure | `10a → ninth 10b family → 11`, 10c results also before 11 | overview: 10a after 5b; Wave P0 "10a waits for Task 5b's recovery shape"; 10a and `gc_partmanifest` share one model | agree, and the handoff adds the correct exclusivity warning |
| ownership-duty | `Task 8 closure → 11` | Wave P1: Task 8 after Task 5, poor parallel candidate, overlaps `CasGc.cpp`+`CasRefLedger.cpp` | agree |
| 10f | "already complete", named as 7b's model predecessor | overview: `Task 5 → Task 10f → Task 7b`, hard predecessor | agree |
| serial tail | `11 → 12 → 13`, then 14 | overview 13 deps 12; Task 14 section says before upstreaming | agree (but 14 has no overview row — §5.6) |

One nuance the handoff introduces that the plan does not state: the handoff places Task 8 in an
*independent* lane converging at Task 11, while the ledger's **M3** groups Task 8 with 7/7a/7b. Neither
is wrong — M3 is a milestone bucket, not a dependency — but a reader who treats M3 as a dependency
group would serialise Task 8 behind the destruction flip for no reason.

---

### 5. Milestones M1–M5 {#docs-5-milestones-m1-m5}

**Structural finding first: the plan defines no milestones at all.** `grep -n "Milestone"` over the
3064-line plan returns zero hits. M1–M5 exist only in the ledger, so there is no plan-side statement for
them to follow from — they can only be checked for internal consistency against the ledger's own
per-task states. On that basis:

| Milestone | Ledger claim | Follows from per-task states? |
|---|---|---|
| M1 | COMPLETE — Task 5 integrated, specialist-reviewed, milestone-gated | **Yes.** Task 5 COMPLETE; merge `78cf06456d3` and gate 1930/1930 recorded. |
| M2 | Task 5b complete; Tasks 6 and 6b remain | **Yes as to openness, understated as to progress.** 5b COMPLETE ✓; 6 and 6b both open ✓ — but as §3.1/§3.2 show, 6b's rename and part of 6 have landed, and the ledger records neither. The milestone is open for the right reason; the *distance to close* is misrepresented. |
| M3 | Tasks 7, 7a, 7b, 8; destructive-gate concurrency review and required focused lanes | **Partly.** Task 7 (evidence open) and Task 8 (closure open) have ledger states that support it. **Tasks 7a and 7b have no ledger state at all** — they appear nowhere except inside this milestone line. Verified unstarted against code (§3.5), so the milestone is correct, but by luck rather than by record. |
| M4 | Task 9 complete; Task 10 partial as itemized | **Yes.** 9 COMPLETE, 10a OPEN, 10b 8/9, 10c PARTIAL, 10d–g COMPLETE — matches the plan's own status table exactly. |
| M5 | Tasks 11–12; full final battery, soaks, results, performance report | **Yes.** Both open in all three documents. |

Ledger gaps worth closing: **Tasks 1c, 2, 3, 4, 4b, 4c, 6, 6b, 7a and 7b have no individual ledger
entry.** Six of those are covered by the blanket foundation line; four (6, 6b, 7a, 7b) are load-bearing
open work whose only recorded state lives in a `tmp/` handoff file that is not under version control.

---

### 6. Plan-internal contradictions and stale prose that could mislead a Task-6+ executor {#docs-6-plan-internal-contradictions-and-stale-prose-that-could-mi}

Ranked by how much work a reader would waste or skip.

**6.1 — Task 6's "delete `namespaceFilesReadable` and rewire its five call sites" (FALSE).**
Symbol gone since Task 4b (`827bc0a9189`). Both the plan's Files and Interfaces blocks and the handoff's
Task 6 row still demand it. Highest-cost stale item in the set: it is a named, line-numbered,
five-call-site instruction for work that does not exist.

**6.2 — Task 6b reads as pending when the rename shipped (FALSE), and its Step-1-before-Step-2 guarantee
is already spent.** See §3.2. The plan should be amended to say: rename LANDED at `9d92c84ee37`;
what remains is the *retrospective* ordering-coverage audit and, if the three-step ordering is
unasserted, `gtest_cas_ref_snapshot_publish_ordering.cpp`.

**6.3 — The critical-path line omits Task 6 before Task 7b (IMPRECISE, with a correctness
consequence).** See §4. The plan contradicts its own dependency column.

**6.4 — Task 9 is COMPLETE while the plan still says it is scheduled "after Tasks 4b and 6" and its
overview row still reads `Depends on 4d,6` (IMPRECISE).** The closure note itself resolves this by
citing Task-6 work that already landed (§3.1), which is coherent — but only if a reader knows that work
landed, and the plan and ledger both say it did not. Additionally, the closure note names the test
`StaleReaderAfterSameNameRebirthNeverSeesSuccessorBytes` while the committed test is
`HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes` — a wrong symbol in an evidence citation (PROSE,
IMPRECISE; small, but an evidence index whose names do not resolve is the failure mode Task 12's own
Step 3 exists to prevent).

**6.5 — Task 5 Step 9's checked "Delete `namespaceIsRemoved`" attributes to Task 5 a deletion Task 4b
performed (IMPRECISE).** See §3.4.

**6.6 — Task 14 has a full section but no row in the task-overview table (IMPRECISE, gap).**
The overview runs 0…13. Task 14 ("strip every branch-local reference from the code") is the one item the
plan itself calls mandatory before upstreaming, and it is the one item a reader scanning the overview
table will not see. The ledger and handoff both carry it, so nothing is lost operationally — but the
plan's own self-review checklist §1–§6 also never maps to Task 14.

**6.7 — Task 4-C carries no completion mark (IMPRECISE / possible gap).** See §2. 4-A is "LANDED",
4-B is "DONE", 4-C is unmarked, and 4-C owns the R11 empty-universe fail-closed guard that Task 7b's
frontier gate depends on. Recommend an explicit confirmation of that guard's presence before Task 7b,
independent of Task 7b's own Step 1b.

**6.8 — Superseded decision sections still sit above the section they superseded (IMPRECISE, layout).**
`{#task-1-merge-measured}`, `{#task-1b-redo-done}` and `{#task-1b-redo}` all precede the Task 1b task
body `{#task-1b}`, so a reader arriving at "### Task 1b: pure `prepareRefChunk`…" from a link or an
anchor reads an unqualified pending task description whose completion note is 60 lines *above* it. Same
shape for the Task 4 split: `{#task-4-split}` comes after `{#task-4}`, and the naming trap it warns
about (4-A/4-B/4-C vs 4b/4c) is real and already produced one file collision. Low risk given the
ledger, but this is exactly the plan's own stated failure mode ("a stale `refCkptKey` doc reference
survived four tasks").

**6.9 — Task 6's own figures are stale in the direction that inflates the work (IMPRECISE).**
The plan says `resolveLifeOrSentinel` is called "on **every** call, at 24 sites, several inside
per-namespace loops". Measured at HEAD: 18 call sites, **all in tests**; zero production callers remain
(§3.1). The plan already anticipates this — "Count the sites yourself when you get here — do not trust
any figure written earlier" — so the instruction is sound and the number is what went stale. Worth
correcting anyway, because the stale figure is what makes Task 6 look like a large production refactor
when the remaining production surface is one function and its two comment references.

---

### 7. Verdict {#docs-7-verdict}

- **Foundation (Tasks 0–4d): documentarily sound.** All 57 named commits exist and are ancestors of
  HEAD. One imprecise anchor (`d278d130024` is a Task-5 commit being used as the Task-4d boundary; the
  correct anchor is `6a3dd6a9245`) and one unmarked substep (4-C).
- **Three-way consistency: two real contradictions, both understating landed work** (Task 6 partly
  implemented and cited as evidence by Task 9; Task 6b's rename shipped under 6b's own commit subject),
  plus one internal ordering contradiction (Task 9 complete ahead of its declared Task-6 dependency).
- **Dependency map: the handoff is more accurate than the plan.** The plan's critical-path line omits
  Task 6 before Task 7b and contradicts its own dependency column; the handoff states the join
  correctly.
- **Milestones: internally consistent, but not derivable from the plan** (the plan defines none), and
  M3 covers two tasks — 7a and 7b — that have no ledger state whatsoever.
- **No CODE or TEST defects were found**, and none were sought beyond the spot-checks the dispatch
  named. All findings are PROSE. Four of them (§6.1, §6.2, §6.3, §3.1) would cause a future executor to
  redo, skip, or mis-sequence real work, so they merit correction in the plan itself rather than only
  batching into `deferred-docs-fixes.md`.
