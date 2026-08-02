---
description: 'Design for the plan that supersedes the Stage-B catalog plan for all remaining work: audit-grounded baselines, a self-contained task set (Task 6 remainder through the Stage-B gates), corrected dependencies, reproducible inventories, and the Q-1 debris-ownership decision. Grounded in the 2026-08-02 midpoint audit.'
sidebar_label: 'Stage B remaining-work design'
sidebar_position: 20260802
slug: /superpowers/specs/cas-stage-b-remaining-design
title: 'CAS Stage B: remaining-work plan design'
doc_type: 'reference'
---

# CAS Stage B: remaining-work plan design {#stage-b-remaining-design}

**Date:** 2026-08-02. **Status:** APPROVED (user review with eight corrections, folded in).
**Branch:** `cas-gc-rebuild`, baseline HEAD `ce312f547c3`.
**Grounding:** the six-report midpoint audit (Tasks 5/5b, 6/6b, 7/7a/7b, 8/9, 10, document
consistency), to be committed as `docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md`.

## 1. Why a new plan {#why}

The audit verified every completion claim of
`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` against the tree. The
foundation is sound — all 57 named commits exist and are ancestors of HEAD; Tasks 0–5b and 9 are
genuinely complete — but the catalog plan and the session handoff systematically misdescribe the
remaining work:

- Task 6's namespace-file production paths and two of its three required tests are already landed;
  the plan still orders their construction, names a deleted symbol (`namespaceFilesReadable`) with
  five line-numbered call sites, and quotes a caller count (`24 sites`) whose production component
  is now zero.
- Task 6b's rename already shipped under Task 6b's own required commit subject (`9d92c84ee37`);
  the plan section reads as wholly pending.
- The plan's critical-path line omits Task 6 before Task 7b, contradicting its own dependency
  column — the one line that, followed literally, enables production destruction without the
  read-side pre-delete revalidation contract.
- Task 7a's and 7b's stated inventories are stale in load-bearing ways (a third
  `enumerateRefPrefix` caller; a second, disjoint Stage-A marker class the exit grep cannot see;
  four `broken_tests.yaml` entries, not three; five independent suppressor terms, not three).

Amending the 3063-line plan in place was rejected: its amendments have gone stale three times, and
each amendment round increased the odds that an executor reads a superseded paragraph as live.

## 2. Supersession and self-containment {#supersession}

The new plan, `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`, is **self-contained and
the sole execution authority for the remaining Stage-B work. It restates every requirement still
applicable to the remaining work. The old plan and the session handoff are historical provenance
only and need not be loaded during execution. No normative requirement is incorporated by
reference.**

Concretely:

- The three requirement bodies still live in the old plan — Task 11's gate table and soak/criteria
  rows, Task 12's report shape, and Task 7b's suppression-formula contract — are **copied into the
  new plan**, shortened and corrected per the midpoint audit (e.g. the formula text gains the
  `frontier_namespaces > 0` conjunct the old plan's quotation dropped).
- References to the old plan appear only as non-normative provenance footnotes ("historical
  origin"), never as instructions an executor must open.
- The execution inputs are exactly: the midpoint audit report, this spec, the new plan, and the new
  ledger. `tmp/task6_handoff_context.md` is not an execution input.

This duplicates three short requirement bodies. That duplication is deliberate: it cuts the
execution context sharply and prevents closed problems from re-entering through stale
paragraphs. Here duplication serves better than abstraction.

## 3. The publication commit {#publication-commit}

The new plan cannot contain a task that creates the new plan. One **atomic publication commit**,
made before the new plan's execution begins, adds together:

1. the new plan;
2. the midpoint audit report;
3. the supersession note at the top of the old plan (stating the self-containment doctrine of §2
   verbatim: the old plan need not be loaded or interpreted; the new document contains all
   remaining obligations);
4. the closing entry in the old SDD ledger
   (`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/progress.md`), pointing forward;
5. the new seeded ledger
   (`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/progress.md`), recording the tree-verified
   baseline states — including the states the old ledger never carried (6b rename landed at
   `9d92c84ee37`; 7a/7b explicitly unstarted; the exact Task-6 landed/missing split of §4).

At no commit boundary do two execution authorities coexist, and the plan does not describe its own
creation. The new plan's first task (T0) therefore starts **after** publication and contains only
the PROSE batch and the model-tool preflight.

## 4. Verified baseline the plan starts from {#baseline}

Stated precisely — "half landed" is banned from the plan because it goes false silently. At
`ce312f547c3`:

**Landed and tested (do not redo):**

- Namespace-file APIs take `NamespaceLifeId` at all three layers (`CasPlainObjects`, `Pool`,
  `Layout`); no namespace-only key helper remains.
- `namespaceFilesReadable`/`namespaceIsRemoved` deleted (Task 4b, `827bc0a9189`); the replacement
  is optional-returning `namespaceFilesLifeIfReadable`/`readableNamespaceFilesLife` (11 callers).
- The delayed `CaInlineWriteBuffer` callback captures the exact `NamespaceLifeId` by value at
  buffer-open and never re-resolves at finalize.
- Committed tests `CasNamespaceFileReadContract.HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`
  and `…DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes` (equivalents of two of the three
  required namespace-file tests; red-first record in
  `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task6-ns-file-contract-report.md`,
  corroborated by the tree).
- Hot-path zero-catalog behaviour pinned for read/write/remove/append/rotation by
  `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey`
  (with a positive control) and `TheLifeResolutionIsPaidOncePerTableOpen`.
- Ref-side destructive-cleanup revalidation races covered non-vacuously by the
  `CasRefGcCleanupAuthority` suite (token/fence moved before first delete and between two keys).
- Task 6b's rename (`tryPublishSnapshotAndAdvanceCheckpointOnce`, one retry unit, not split).
- Task 7's implementation (`224aacd8eb9`) with its six focused tests; the three carried residues
  from the old plan are already fixed in code.
- Task 8's model gate (exact RESULTS artifact) and both production slices with 11 direct tests;
  the duty-queue admission seam is a verified partition of the durable-mutation surface.
- Task 9's closure note; the `PENDING` double-count fix (`8e9b06c2a81`); Task 10 units d/e/f/g.

**Missing (the plan's subject matter):**

- Ref-side life handle (`NamespaceLifeId::fromLiveHandle` does not exist); `CasRefLedger.cpp`
  performs `CasRefCatalog::read` at ten sites; no ref-side rebirth-alias or zero-catalog-GET test;
  `gtest_cas_ref_read_contract.cpp` absent.
- The `list` arm of the namespace-file zero-catalog pin.
- `resolveLifeOrSentinel` (test-only, 17 call sites) and `stageATransition` (1 production line,
  1 declaration, ~291 test hits in 39 files) still present; no single named fixture seam.
- All four Task-6b ordering/backoff/poison tests; the `Poisoned` publication-refusal predicate and
  the publish backoff (`admitSnapshotPublishUnderStateLock`/`advancePublishBackoff`/
  `resetPublishBackoff`) have zero test references.
- Task 7 closure evidence (no red/mutation record, no post-implementation
  `test_content_addressed_drop_pool_member` run, no ownership inventory) and the untested
  retirement token-fence (arm b).
- Task 8 closure (T-1 loose throw fences, T-2 reject-arm drain has model coverage only, T-3
  accounting asserts not on the real round, C-1 `sweepManifestCursorPage` footgun, Q-1 decision,
  C++ mutation demonstrations, task-level gates, closure commit).
- Tasks 7a, 7b in full; Task 10a verdict; the ninth 10b family (unstaged edits to
  `run_gc_partmanifest.sh`); 10c results artifact and the three unpinned runners; the 10f
  `UniverseAuthoritative` disclosure.
- Tasks 11 and 12.

## 5. Task set {#tasks}

### T0 — bootstrap (post-publication) {#t0}

- Batch all ~20 PROSE findings from the six audit reports into
  `docs/superpowers/cas/deferred-docs-fixes.md` in one pass (standing directive: no prose fix
  rounds).
- **Model-tool preflight (worktree state, not a durable result):** `tmp/tla2tools.jar` in this
  worktree is the rejected 1.7.4 jar. The preflight verifies the pinned jar
  (SHA-256 `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`) is what the runners
  see, and the plan records the exact restore command. Runners already fail fast via
  `check_tlc_pin`; no runner may silently search for another jar. Every future model-lane session
  repeats this preflight — it is an environment precondition, not a task deliverable.

### T1 — Task 6 remainder: ref-side contract and sentinel retirement {#t1}

**(a) Ref life handle — classification before code.** The slice begins with a recorded
classification of each of the ten `CasRefCatalog::read` sites in `CasRefLedger.cpp` into:

1. admission/identity resolution — one snapshot per operation is required;
2. mutation authority — the read may be mandatory; keep it;
3. current-life destructive revalidation — the read is mandatory; keep it;
4. held-handle hot path — a catalog read is forbidden; remove it;
5. duplicate read within one operation — remove it.

Only classes 4 and 5 are removal candidates. "Hot paths off the ten sites" as a blanket goal is
explicitly rejected — it invites deleting an authority check. The classification lands in the task
report before the first production edit. Then plumb held lives through the ref reader/table-cache
paths. **Admissible authority source:** a held life originates only from a catalog snapshot
resolution; nothing may mint a `NamespaceLifeId` from a `RootNamespace`, a LIST key, or a path.
If the held capability at each site is already a `NamespaceLifeId`, pass it directly — the
`fromLiveHandle` factory the old comment in `CasNamespaceLifeId.h` promises is added only if a
site genuinely needs it, and under the same source restriction. Add the two missing ref tests in new
`gtest_cas_ref_read_contract.cpp`: a reader holding inc1 across drop+rebirth reads
stale-or-`NotFound`, never inc2; a hot ref read performs zero catalog requests (op-journal
assertion). Stale-writer contract: writes through an old handle land under the old incarnation or
fail; they never target the new life.

**(b) Namespace-file closure.** Add the `list`-arm zero-catalog assertion (through a held life, op
journal shows zero catalog GETs), completing the third required namespace-file test. Keep the two
committed tests under their current names and record the name-equivalence mapping in the task
report. Re-check the one Task 9 closure-note claim the audit flagged as movable (the zero-catalog
sentence) and append a one-line evidence correction to the closure note only if the new test
changed the fact.

**(c) Fixture seam and sentinel retirement — three commits:**

1. add the single named fixture seam in `cas_test_helpers.h` (documented list of deliberate
   divergences: sentinel-addressed objects; `Live` rows without `_ckpt` stay intentionally
   possible for recovery/failure tests);
2. mechanically migrate the ~291 test-side `stageATransition` hits (39 files, including the new
   use in `gtest_cas_writer_duties.cpp`) onto the seam;
3. delete `resolveLifeOrSentinel` and `NamespaceLifeId::stageATransition`, and record the
   tree-wide zero-grep over build inputs.

The existing `CasRefGcCleanupAuthority` coverage is asserted-as-existing in the report; it is not
rewritten. Final commit subject (carried from the requirement):
`ca: ref — read-side contract: handle-scoped reads and namespace files, pre-delete life revalidation`.

Gates: affected targeted tests, full CA unit gate, and the object-storage/integration lane that
exercises dedup-log rotation (write side of the zero-catalog contract).

### T2 — Task 6b remainder: publication-ordering coverage {#t2}

The rename is landed; no rename work. Retrospective coverage audit is already done (midpoint
audit): the three-effect order is unasserted. Create
`gtest_cas_ref_snapshot_publish_ordering.cpp` with:

- `SnapshotBodyIsDurableBeforeCheckpointAdvances` (backend op-journal order of body PUT vs `_ckpt`
  CAS);
- `AdoptionHappensLastAndOnlyAfterBothDurableEffects` (positive adoption assertion; only the
  fenced-out negative exists today);
- `PoisonedRefusesPublicationAndTriggersReRecovery` (the predicate is entirely untested);
- a **plain characterization test** of the publish backoff (`admitSnapshotPublishUnderStateLock`'s
  decisions for a fixed clock sequence) — plain, not differential: the pre-rename tree no longer
  exists to capture "before" literals from. The `CasRequestControllerBackoff` family is a different
  mechanism and is not coverage here.

Gate: full CA. Own commit and review; T2 does not block T6.

### T3 — Task 7 closure {#t3}

- Fix the retirement-fence test: rewire the injection to fire between the two catalog reads (e.g.
  on the catalog GET rather than the roots LIST), rename the existing test to what it actually
  proves, and add the arm-(b) test (token changed at the final check → retirement refused; both
  warning strings asserted).
- **Load-bearing mutation demonstration, performed after implementation; mutation reverted; patch
  and failing output preserved.** This wording is mandatory in the report — it is evidence of test
  sensitivity, not red-first TDD, and the plan does not rewrite history.
- Run the `test_content_addressed_drop_pool_member` lane at a commit containing `224aacd8eb9`;
  record the structured result.
- Write the catalog-exact ownership inventory (decommission selects victims exclusively by exact
  catalog-name ownership; no life-key prefix fallback) into the task report.
- Review; closure commit.

### T4 — Task 8 closure {#t4}

**Q-1 decision — debris ownership, adopted:**

- the writer cleanup duty **owns** provably `Rejected` debris (absence of durable publication
  proven) and is retired only **after** the exact deletion succeeds or absence is confirmed;
  a cleanup failure retains the duty for retry — the duty is never removed first, so no stop
  between the two actions can leave debris ownerless;
- `Uncertain` or potentially `Durable` bodies are **never** deleted by the writer path;
- such bodies are nominated and deleted by the orphan sweep after its own reachability and
  authority checks.

This is simpler and safer than making `CasPartWriteTxn` reason simultaneously about transaction
outcome and global reachability. Acceptance conditions, each a test or measurement in this task:

1. the reject-arm wedge-drain test proves `Rejected` debris does not leak forever;
2. orphan-sweep tests cover eventual nomination for `Uncertain`/`Durable` bodies;
3. missing attribution leads to suppression, never to a destructive fallback;
4. accounting states explicitly which owner holds each debris class.

Also in T4: tighten the two `EXPECT_THROW(…, DB::Exception)` fences to
`expectThrowsCode(ErrorCodes::CORRUPTED_DATA, …)` (T-1); add the reject-arm C++ test (T-2, also
acceptance 1); add the B2-ordinal/unmatched-remove accounting assertions to the real-round
nomination test (T-3, also acceptance 4); remove the `sweepManifestCursorPage` footgun from the
production surface (C-1: delete it from the production translation unit/header or move it into a
test-only translation unit — a comment-only disposition is excluded, because a production-looking
API that must not be called is the footgun). Load-bearing mutation demonstrations use the T3
wording verbatim.
Gates: full CA + the S3 evidence the task always owed. Closure commit.

### T5 — Task 7a: delete probe A {#t5}

The old plan's inventory, corrected by the audit:

- `enumerateRefPrefix` now has **three** callers (round scan, detector, rebuild) — the helper is
  kept; only the detector caller dies. The one-LIST criterion is re-derived accordingly (the
  rebuild caller is not on the regular folding-round path).
- The Python consumer set is `signals.py`, `metrics.py`, `run.py`, and three soak unit tests —
  all updated, none left silently watching dead counters.
- The `PHASE N/…` comment numbering is re-derived from the `GcPhaseTimer` sites (the sequence is
  already corrupt at HEAD; no arithmetic on the old plan's count).
- The compile-time pin in `gtest_cas_ref_catalog.cpp` (`GcRoundPlanSignatureAccess`) and its
  `friend` declaration are in the deletion inventory.
- Convert (do not delete) the round-enumerates-once test to assert exactly one full
  `cas/ns/stream/` enumeration on every round.

**Reproducible inventory rule (applies to T5 and T6):** the report records the exact grep
command(s), the baseline commit they ran at, the full hit list as a versioned artifact, and the
expected post-cleanup zero; never a bare count. The CA-gate test-count delta is recorded relative
to the immediately preceding commit of this task's own lane, not to a floating parallel branch.

### T6 — Task 7b: destruction enablement {#t6}

Prerequisite (unchanged, still open): the frontier-attribution question — why healthy pools leave
a handful of namespaces unproven — is answered with an instrumented reproduction
(`task7b-frontier-prereq-audit.md` is BLOCKED on exactly this; its Task-5b blocker is discharged).
No flip while the attribution is unexplained.

Then:

- flip `kDefault`, rename `AuthoritativeForTest` → `Authoritative` (compiler-enumerated);
- assert **all five** independently-suppressing conditions, each alone, with per-family inertness:
  anomalies; carried holds; `!universe_authoritative`; `frontier_namespaces == 0` (the
  empty-universe floor the old plan's three-term quotation hid); `frontier_proven !=
  frontier_namespaces`;
- closeout driven by the **complete** marker set: case-insensitive `Task 7b` grep over
  `src/ tests/ utils/` (15 non-doc files at baseline: 8 `STAGE-A RETURN ITEM` sites + 7
  `STAGE-A CONTRACT` banner sites), plus all **four** `broken_tests.yaml` entries including
  `05010_content_addressed_mounts_gc_health`; the seven banner sites are integration/soak
  assertions that go red on the flip and are part of the inventory, not collateral;
- gates: full CA + both CA-S3 lanes + `test_content_addressed_gc_s3`, delete families nonzero,
  zero anomalies; `04290`/`04295` drain-to-zero with assertions intact is the end-to-end proof;
- two commits: the flip alone, then the closeout (yaml removals + assertion restorations),
  separately revertable. Inventory discipline per the T5 rule.

### T7 — model lane: Task 10 closure {#t7}

Two independent inner lanes that converge only before Task 11:

- **Lane A:** 10a verdict on the `listedTok` skip premise (creating the missing
  `CaGcRootLocalPartManifestCore_RESULTS.md`), then the ninth 10b family (`run_gc_partmanifest.sh`
  battery + RESULTS + commit), which shares the same model and must not be edited concurrently
  with 10a. The RESULTS record **names the sharding-arm debt explicitly**: three configs pin
  `known-model-error UnchangedCompositeVars`, so the model's sharding arm is unproven and booked
  as `KNOWN` — this debt must not vanish into a column.
- **Lane B:** 10c before/after results artifact for the four runners; add `check_tlc_pin` to the
  three unpinned runners; fix the README runner-column and `{#fix-runners}` stale prose (these two
  are 10c closure content, not batch prose).

Plus the small 10f disclosure: one sentence in `CaGcDestructiveGateCore_RESULTS.md` and one
comment at `UniverseAuthoritative` stating the term is pinned TRUE because the model gates the
post-flip posture, and that T6's per-term assertions are where that term gets its coverage.

### T8 — Stage B gates (Task 11) {#t8}

The requirement body is carried into the new plan from the old Task 11, shortened and corrected:
AMD tidy lane in its own `build_tidy`; executable-prose sweep; the residual-cleanup gate row —
which the new plan **enumerates explicitly, item by item with a disposition column** (the audit
stays evidence/provenance, never a requirement source an executor must load). The row's content,
fixed here: the Task-1 review minors 1–7 and the re-review's MINOR-B plus NITs C–F (copied
verbatim into the new plan at plan-writing time); Task 5's deferred symmetric regression test for
the exact-delete exception branch; the two comment-policy citations audit-t8 named (the
`writeFile` directive reference; the `stageATransition` comment, which T1(c) deletes anyway); the
Q-2 ABA-edge sequencing observation; and the 10b sharding-arm `KNOWN` model debt (named in T7's
RESULTS, re-checked here). Then: full CA
gtest gate against the post-T5 baseline (per the T5 delta rule); all CA integration lanes; the four
soaks (churn, rebirth with namespace-file readers/writers, decommission, and the separate
90-minute general soak carrying the sequential-baseline destructive workload); the six result
criteria as PASS/FAIL gate rows; the Constraint-16 insert-path request-profile guard; specimen
preservation for T9; the results file with the `STAGE B: PASS`/`FAIL` verdict.

### T9 — destructive-baseline performance research (Task 12) {#t9}

Carried from the old Task 12, unchanged in substance: sample the preserved T8 specimen (never
re-run), phase/S3 decomposition covering the full cost inventory, un-timed spans named rather than
estimated, honest before/after only where a before exists (probe A yes, destruction no), ranked
falsifiable opportunities, evidence index, figures re-read at write time, BACKLOG entries. No
optimization lands.

### Stage-B completion semantics {#completion-semantics}

- T8 issues the **technical verdict** (`STAGE B: PASS`/`FAIL`).
- T9 is the **mandatory closeout**: the ledger does not record Stage B as COMPLETE until T9's
  report is committed.
- T9 does not change T8's verdict — unless its analysis of the specimen discovers that a T8
  measurement was wrong, in which case the verdict is corrected on that evidence and the
  correction is recorded in both documents.

### Follow-ups (in the document, outside the Stage-B verdict) {#follow-ups}

**The follow-ups do not gate `STAGE B: PASS` and run after the T9 closeout.**

- **F1 (old Task 13) — post-baseline mechanical split:** request-count/order goldens first, then
  split `CasGc.cpp` and `CasRefLedger.cpp` along existing seams, then the unchanged goldens rerun.
- **F2 (old Task 14) — pre-upstream sanitation:** strip branch-local documentation references and
  coordination markers from production code (the audit's 76-reference comment inventory is the
  starting measurement); keep each rationale, drop each citation; remeasure before declaring
  clean.

## 6. Dependencies {#dependencies}

```
publication commit ─→ T0 ─→ (all tasks)

T1(6) ────────────────→ T2(6b) ────────────┐
T3(7-close) ─→ T5(7a) ─┐                   │
T1(6) ─────────────────┴─→ T6(7b) ─────────┤
T7 lane A (10a → 10b⁹) ────────────────────┼─→ T8(11) ─→ T9(12) ─→ [F1 → F2]
T7 lane B (10c) ───────────────────────────┤
T4(8-close) ───────────────────────────────┘
```

- T6 waits for **both** T1 and T5 (the join the old plan's critical-path line dropped; T1's
  pre-delete revalidation contract is a hard predecessor of enabling destruction).
- T2 is not a T6 predecessor and may run beside T5/T6 after T1.
- T7's two inner lanes are independent of each other and of the code chain; they converge only
  before T8. 10a and the ninth 10b family share one model and are strictly sequential.
- T4 is an independent lane; one production writer at a time on overlapping
  `CasRefLedger`/`CasGc` seams.
- T8's early-startable pieces (report skeleton, suite inventory, tidy build) may begin in idle
  windows; its batteries and soaks wait for every predecessor.

## 7. Execution discipline {#discipline}

Carried unchanged from Stage-B practice: SDD with per-slice TDD where tests are new (mutation
demonstrations, labelled as such per T3's wording, where tests exist before their evidence);
ordinary commits, never rebase/amend; build/test logs to unique files under `build/` with
independent subagent analysis of completed logs; TLC reruns only when the model, invariants,
correspondence, configs, runner contract or checker change; mechanical work delegated per the
standing codex policy with Claude-side review; PROSE findings batched, never fix-rounds; the
comment policy (keep the reason, drop the internal citation) applies to every line the tasks
touch.
