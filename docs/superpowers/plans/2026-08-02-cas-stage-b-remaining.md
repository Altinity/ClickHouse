---
description: 'Self-contained execution plan for the remaining CAS Stage-B work: Task 6 remainder (ref-side read contract, sentinel retirement), Task 6b ordering coverage, Task 7/8 closures, probe-A deletion, destruction enablement, model-debt closure, the Stage-B gate battery and the destructive-baseline performance report. Supersedes the 2026-07-28 catalog plan for execution.'
sidebar_label: 'Stage B remaining work'
sidebar_position: 20260803
slug: /superpowers/plans/cas-stage-b-remaining
title: 'CAS Stage B: remaining work'
doc_type: 'reference'
---

# CAS Stage B: Remaining Work Implementation Plan {#stage-b-remaining-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development is
> MANDATORY for tasks T1–T7. Every slice runs the cycle
> **implementor → specification review → code-quality review → ordinary commit** — no slice skips
> a review stage. T0, T8, and T9 are controller-orchestrated (their subagents analyze logs and run
> batteries; the controller owns the gates and the verdict). Steps use checkbox (`- [ ]`) syntax
> for tracking.

**Goal:** Close Stage B of the content-addressed ref/catalog rework: finish the read-side contract,
close the open evidence on landed implementations, delete probe A, enable production destruction,
close the model debt, run the Stage-B gate battery with four soaks, and write the
destructive-baseline performance report.

**Architecture:** All production work happens in
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` on branch `cas-gc-rebuild`. The
catalog is the sole authority for namespace lives; LIST is a zero-trust hint. Held
`NamespaceLifeId` handles make hot reads catalog-free; destructive actions revalidate exact
catalog tokens per key; the GC destructive gate opens only on a complete, catalog-proven frontier.

**Tech Stack:** C++ (gtest via `unit_tests_dbms`), TLA+/TLC (pinned jar), Python integration lanes
via `ci.praktika`, the `utils/ca-soak` harness.

## Authority and execution inputs {#authority}

This plan is **self-contained and the sole execution authority for the remaining Stage-B work. It
restates every requirement still applicable to the remaining work. The old plan
(`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md`) and the session handoff
(`tmp/task6_handoff_context.md`) are historical provenance only and need not be loaded during
execution. No normative requirement is incorporated by reference.**

Execution inputs, exhaustively: this plan; the design spec
`docs/superpowers/specs/2026-08-02-cas-stage-b-remaining-design.md`; the midpoint audit
`docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` (evidence/provenance, not requirements);
the ledger `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/progress.md`.

**Baseline:** all task briefs below were verified against the tree at `ce312f547c3` (the audit
baseline). Where a step names counts or hit lists, re-derive them at execution time and record the
command and its output; the baseline figures are expectations, not facts to trust.

## What is already done — do not redo {#already-done}

- Tasks 0–5b, 9, 10d/e/f/g of the old plan: complete and verified. Task 5's removal lifecycle,
  dead-life janitor, and `Removing → absent` proved deletion; Task 5b's LIST-independent recovery
  (`chooseRecoveryGrounding`, `committed_through`).
- Namespace-file APIs take `NamespaceLifeId` at all three layers (`CasPlainObjects`, `Pool`,
  `Layout`). `namespaceFilesReadable`/`namespaceIsRemoved` do not exist (deleted by `827bc0a9189`);
  the replacement is optional-returning `namespaceFilesLifeIfReadable`/`readableNamespaceFilesLife`.
- The delayed `CaInlineWriteBuffer` callback in `ContentAddressedTransaction::writeFile` captures
  the exact `NamespaceLifeId` by value at buffer-open; a regression test exists
  (`CasNamespaceFileReadContract.DelayedInlineFinalizeCannotChangeSuccessorTokenOrBytes`).
- `CasNamespaceFileReadContract.HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes` covers the
  stale-reader/rebirth alias contract for namespace files.
- Zero-catalog hot paths for namespace-file read/write/remove/append/rotation:
  `CasNamespaceFileDiskProfile.SteadyStateFileOperationsTouchNoCatalogRefBlobOrManifestKey` (with a
  positive control) and `TheLifeResolutionIsPaidOncePerTableOpen`.
- Ref-side destructive-cleanup revalidation races: the `CasRefGcCleanupAuthority` suite
  (`CatalogTokenMoveBeforeFirstDeleteRefusesEveryRefObjectDelete`,
  `CatalogTokenMoveBetweenKeysAllowsFirstAndRefusesSecondDelete`, plus the GC-fence twins).
- Task 6b's rename: `tryPublishSnapshotAndAdvanceCheckpointOnce` landed at `9d92c84ee37`, one retry
  unit, not split.
- Task 7's implementation (`224aacd8eb9`) and its six `CasDecommissionCatalogDuties` tests; the
  decommission victim selection is catalog-exact (no LIST fallback).
- Task 8's model gate (`d34aa06d89f`, exact RESULTS artifact) and both production slices
  (`c3cc24c8152` duty queue, `8f14bc119fe` orphan nomination) with 11 direct tests; the
  `mutateRefsAfterWriterCleanup` admission seam is a verified partition of the six durable-mutation
  forwards in `CasPool.cpp`.
- The `PENDING` gauge double-count fix (`8e9b06c2a81`): `04290`/`04295` read `pending_condemned`
  alone.

## Global Constraints {#global-constraints}

- Never rebase or amend anywhere; ordinary follow-up commits only. MAIN
  (`/home/mfilimonov/workspace/ClickHouse/master`) stays on `cas-gc-rebuild` and never switches
  branches; LANE-G creates `laneg/<task>` branches per the `{#two-worktrees}` map — that is the
  one sanctioned branch-switching surface.
- One production writer at a time on overlapping `CasRefLedger`/`CasGc` seams. Read-only audits and
  log analysis may run in parallel.
- Every build/test run redirects to a unique file under `build/` (e.g.
  `build/t1a_red_build.log`); a completed log is summarized by an independent subagent. Never
  analyze a live log. No `-j` with `ninja`; never use `nproc`.
- The unit-test binary is `build/src/unit_tests_dbms` (target `unit_tests_dbms`). The **full CA
  gate** is: `utils/cas-gate/generate_cas_suites.sh build` (regenerates the suite list from
  sources and fails loud on any unclaimed suite), then one run of `build/src/unit_tests_dbms` with
  the generated suite list joined as `<suite>.*:<suite>.*:...` (the names in `cas_suites.txt`
  are bare — each needs the `.*` suffix or the filter matches zero tests), logged under
  `build/`. Both gate scripts
  (`generate_cas_suites.sh`, `run_cas_gate_per_suite.sh`) are versioned in `utils/cas-gate/` by
  the publication commit itself — the gate never depends on untracked `tmp/` files.
- TLC reruns only when a model's transition system, invariants, action correspondence, configs,
  runner contract, or checker change — never because C++ moved. Model runners refuse a wrong jar
  via `check_tlc_pin` (SHA-256 `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`);
  no runner may silently accept another jar.
- **Mutation-demonstration wording, mandatory in reports:** "load-bearing mutation demonstration
  performed after implementation; mutation reverted; patch and failing output preserved." This is
  evidence of test sensitivity, not red-first TDD; do not rewrite history. Tests written NEW in
  this plan get genuine red-first runs.
- **Reproducible-inventory rule (T5, T6, and any sweep):** record the exact command, the commit it
  ran at, the full hit list as a versioned artifact (task report), and the expected post-cleanup
  zero. CA-gate test-count deltas are recorded relative to the immediately preceding commit of the
  same lane, never a floating parallel branch.
- PROSE findings (comments, docs, plan text) go to `docs/superpowers/cas/deferred-docs-fixes.md`
  in batches; they never open fix rounds. CODE/TEST findings do.
- Comment policy: keep the reason, drop the internal citation. No comment may reference plans,
  reviews, task numbers, or BACKLOG anchors.
- C++ style: Allman braces. Never `sleep` to fix a race. `EXPECT_THROW` on a `LOGICAL_ERROR` site
  aborts sanitizer lanes — use the death-test split; prefer `expectThrowsCode` with the exact
  error code everywhere.
- **Suite-naming normalization (user directive 2026-08-03):** after T6 lands (and before the
  final tidy re-run and T8's battery), one mechanical sweep renames every CAS test suite to the
  mandatory `Cas` prefix — 23 violators at the time of the directive: the 16 `RefWriter*`
  suites, `CaLifecycle`, `CaWiring`, `CatalogLifecycleReconciler`, `ContentAddressedLog`,
  `RefTableCacheEviction`, and the three parameterized instantiation prefixes
  (`InMemory/`→`CasInMemory/`, `Local/`→`CasLocal/`, `WinnerShape/`→`CasWinnerShape/`). After
  the sweep the routine gate filter is the literal `--gtest_filter='Cas*'`, and
  `utils/cas-gate/generate_cas_suites.sh` is repurposed into the INVARIANT VERIFIER (it fails
  loud if any source-derived CAS suite does not match `Cas*` — the naming convention becomes an
  executing check instead of a hand-maintained list). Abort isolation needs no per-suite runner:
  every `LOGICAL_ERROR` expectation sits behind the death-test split (gtest death tests fork, so
  sanitizer aborts are child-isolated), enforced by the hygiene sweep; the per-suite runner
  remains a diagnostic tool only.
- **Post-tidy parallel kickoff (user directive 2026-08-03):** IMMEDIATELY after the final tidy
  re-run completes, launch in PARALLEL: (1) the codex implementation review (gpt-5.6-sol xhigh,
  nohup — it runs long) over the whole Stage-B implementation delta, and (2) a 20-minute PLAIN
  general soak (`python3 -m soak.run --phase 3 --duration 20m` — NOT a scenarios/cards run) WITH
  CHAOS enabled (the harness's fault-injection mode). Both are additional quality gates ahead of
  the T8 battery; the chaos smoke does not replace the T8 soak program.
- **Final tidy re-run (user directive 2026-08-03):** after ALL C++-changing tasks are complete
  (i.e. once T5/T6/the fsck slice and any fix waves have landed, before T8's battery), run ONE
  incremental clang-tidy pass — `ninja -k 0 -C build_amd_tidy unit_tests_dbms` in the MAIN
  worktree ONLY (that build dir holds the compile database, ninja state and tidy cache; any
  other lane would pay a cold run). Expect a fast incremental pass; any NEW CAS-scoped
  diagnostic is resolved then (fix or NOLINT+reason — CI gate, no noise category). Not a
  per-task gate.
- Held-life authority: a `NamespaceLifeId` originates only from a catalog snapshot resolution.
  Nothing may mint one from a `RootNamespace`, a LIST key, or a path.
- An absent canonical life is inert debris — never a pool-suppressing damage anomaly.
- Integration lanes run from the repo root: `python3 -m ci.praktika run "integration" --test
  "<space-separated selectors>"` (one `--test` flag; repeats collapse to the last), binary
  symlinked at `ci/tmp/clickhouse`, output to `build/<name>.log`. Never overlap praktika runs with
  ca-soak scenarios on this box (praktika prunes docker containers/volumes).
- Stage-B completion semantics: T8 issues the technical verdict (`STAGE B: PASS`/`FAIL`); T9 is the
  mandatory closeout; the ledger records Stage B COMPLETE only after T9's commit; T9 changes the
  verdict only if it discovers a wrong T8 measurement, and then the correction is recorded in both
  documents.

## Task overview and dependencies {#task-overview}

| # | Task | Depends on |
|---|---|---|
| T0 | Bootstrap: prose batch, tooling verification, model preflight | publication commit |
| T1 | Task 6 remainder: ref-side contract, sentinel retirement | T0 |
| T2 | Task 6b remainder: publication-ordering coverage | T1 |
| T3 | Task 7 closure: retirement fence, evidence, inventory | T0 |
| T4 | Task 8 closure: Q-1, fences, reject arm, footgun | T0 (+ the free production writer) |
| T5 | Probe A deletion (old 7a) | T3 |
| T6a | **Frontier-attribution risk spike** (the extracted flip prerequisite) | T0 |
| T6 | Destruction enablement (old 7b) | T1, T5, T6a |
| T7 | Model lane: 10a → ninth 10b family; 10c; 10f disclosure | T0 |
| T8 | Stage B gates (old Task 11) | T1–T7 |
| T9 | Destructive-baseline performance research (old Task 12) | T8 |
| F1 | Follow-up: mechanical split of the two 4000-line files | T9 complete (outside the verdict) |
| F2 | Follow-up: pre-upstream sanitation sweep | F1 (outside the verdict) |

```
publication commit ─→ T0 ─→ (all lanes)

Lane A: T1(6) ─→ T2(6b) ───────────────────────┐
Lane B: T6a (frontier spike) ──┐               │
        (join: T1 + T5 + T6a) ─┴─→ T6(7b) ─────┤
Lane C: T3(7-close) ─→ T5(7a) ─┘               ├─→ T8(11) ─→ T9(12) ─→ [F1 → F2]
Lane D: T7 (10a → 10b⁹ ∥ 10c) ─────────────────┤
T4(8-close, when the production writer frees) ─┘
```

- **T6a runs EARLY, right after T0** — the frontier-attribution question is the one potentially
  unbounded unknown in the plan, and the investigation needs neither T1 nor T5 (only the flip
  does). Answering it in parallel with T1/T3/T7 bounds the schedule; if it finds a real gap, the
  resulting production fix is scheduled on the production-writer lane before T6.
- T6 waits for **T1, T5, and T6a's verdict**. T1's pre-delete revalidation and handle contract is
  a hard predecessor of enabling destruction. T2 is NOT a T6 predecessor.
- T7's two inner lanes are mutually independent and converge only before T8. 10a and the ninth
  10b family share one model (`CaGcRootLocalPartManifestCore`) and are strictly sequential.
- T4 runs whenever the single production writer is free (its production edits are small: the C-1
  move and, at most, the settlement-ordering fix); its test-only steps may proceed anytime.
- T8's early-startable pieces (report skeleton, suite inventory, `build_tidy`, E4 soak-command
  pinning) may begin in any idle window; its batteries and soaks wait for every predecessor.

**Gate cadence (wall-clock discipline):** targeted tests on EVERY commit; the full CA gate runs
at **lane closures** — the end of T1 (the T1c retirement commit), T2, the T3→T5 lane (at T5,
which also owns the exact count delta), T4, T6, and T8's battery — not after every mechanical
slice. Where a task step below says "full CA gate", this cadence rule decides whether it runs at
that step or at the lane's closure.
**Delegation:** the mechanical bulk (T1c's fixture migration, most of T5's deletion sweep) goes
to a cheap implementor subagent per the standing policy; reviews stay with the controller.

## Two-worktree execution map {#two-worktrees}

Two worktrees run the lanes concurrently. Never share one dirty worktree between concurrent
writers; each worktree has exactly one writer at a time.

**MAIN — `/home/mfilimonov/workspace/ClickHouse/master`, branch `cas-gc-rebuild`.** The single
production writer for `CasRefLedger`/`CasGc`/`CasPool` seams and the ONLY worktree that commits
to `cas-gc-rebuild`. Owns, in order:

1. the publication commit, then T0;
2. **T1** (all three slices; T1c's mechanical migration delegated but committed here);
3. **T5** (production `CasGc` deletion; starts after lane-g's T3 is integrated);
4. **T4's production steps** (the C-1 move out of `CasOrphanManifestSweep`, and the
   settlement-ordering fix if Step 1 finds one) — slotted whenever the writer is free between
   T1 slices / after T5; T4's commit lands here;
5. **T6** (flip + closeout, after T1 + T5 + T6a's verdict);
6. **T8** batteries, soaks, verdict; **T9**; later F1/F2.
7. Integration of every lane-g branch: `git merge --ff-only` (or cherry-pick) of `laneg/<task>`
   branches in dependency order; after each integration verify `git log -1` shows the expected
   HEAD (shared-repo index races have misattributed commits before).

**LANE-G — `/home/mfilimonov/workspace/ClickHouse/lane-g`, temporary branches.** Currently on
the fully-merged `cas-gc-rebuild-task5b` at `2769d788463` with ~224 untracked investigation
artifacts — PRESERVE them (they survive `git checkout -b`; never clean/reset this worktree
broadly). For each assigned task: `git checkout -b laneg/<task> <latest integrated cas-gc-rebuild
commit>` and hand finished ordinary commits back to MAIN for integration. No pushes. Owns:

1. **T6a** (the frontier spike) — immediately after T0; its instrumentation commit, if kept,
   goes to MAIN for integration before T6;
2. **T3** (Task 7 closure: test edits in `gtest_cas_decommission_catalog_duties.cpp`, mutation
   demonstrations, the ownership inventory, the report) — no production `CasGc`/`CasRefLedger`
   edits, safe beside MAIN's T1;
3. **T7** (the whole model lane — `docs/superpowers/models/` ownership is disjoint from all C++);
4. **T2** after T1 is integrated (new test file + read-only subjects; branch from the
   post-T1 commit);
5. **T4's test-only steps** (T-1/T-2/T-3 edits in `gtest_cas_orphan_nomination.cpp` /
   `gtest_cas_writer_duties.cpp`) if MAIN's writer is still busy — handed to MAIN to combine
   with the C-1 move into T4's commit;
6. **T8's early pieces**: the `build_tidy` build (in lane-g's own build directory), the E2
   executable-prose sweep, the E3 report skeleton, the E4 soak-command pinning.

**Cross-worktree resource rules:**
- Full CA gates and `unit_tests_dbms` builds are serialized ACROSS worktrees with
  `flock "$(git rev-parse --git-common-dir)/unit_tests.lock"` — the common dir is SHARED by both
  worktrees, while each worktree's `tmp/` is its own directory and would silently give each lane
  a different lock (two concurrent gates have exhausted `/tmp` inodes on this box —
  check `df -i` if a gate fails strangely).
- Praktika integration runs and ca-soak scenarios: at most ONE on the box at any time, owned by
  MAIN (praktika prunes docker containers/volumes and would kill a running soak).
- The `test_content_addressed_drop_pool_member` lane run of T3 is therefore executed by MAIN on
  lane-g's behalf (or by lane-g in a slot MAIN grants) — the report records who ran it and at
  which commit.
- Both worktrees redirect logs into their OWN `build*/` directories; log filenames carry the
  task id.

---

### Task T0: Bootstrap {#t0}

**Files:**
- Modify: `docs/superpowers/cas/deferred-docs-fixes.md`
- Verify (versioned by the publication commit): `utils/cas-gate/generate_cas_suites.sh`,
  `utils/cas-gate/run_cas_gate_per_suite.sh`
- Verify (worktree state, no commit): `tmp/tla2tools.jar`

**Interfaces:**
- Produces: the versioned CA-gate tooling every later task's gate step uses; the batched prose
  record later reviewers consult so prose findings do not re-enter fix rounds.

- [ ] **Step 1: batch the PROSE findings.** Append one dated section to
  `docs/superpowers/cas/deferred-docs-fixes.md` titled
  `2026-08-02 Stage-B midpoint audit — batched prose findings`, listing every PROSE finding from
  the six audit reports (`docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` carries them
  consolidated), one line each: location, grade (FALSE/IMPRECISE), correct fact. Include at
  minimum: the Task-5 Files-list phantom `gtest_cas_ns_removal_lifecycle.cpp`;
  `resolveLifeOrSentinel`'s stale doc comment; the old plan's Task-6/6b/7a/7b stale figures
  (superseded wholesale, one line saying so); the stale model prose (`{#fix-runners}` closing
  paragraph, `models/README.md` runner column, `cas/06-tla-models.md` jar + runner list); Task 9
  closure-note citations (`OldFileHiddenByListIsInvisibleAfterRebirth` →
  `ColdReaderUsesCatalogCutWhileOldFileSurvivesRemoval`;
  `StaleReaderAfterSameNameRebirthNeverSeesSuccessorBytes` →
  `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes`; the `mountpointObjectKey` attribution; the
  scratch-report provenance) — fixing the two test-name citations in
  `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md` directly is allowed here
  (they are one-word renames in an evidence index); everything else waits for its named executor.
- [ ] **Step 2: verify the gate tooling (versioned by the publication commit, not created
  here).** `utils/cas-gate/generate_cas_suites.sh` and `utils/cas-gate/run_cas_gate_per_suite.sh`
  are already in the tree. Run
  `utils/cas-gate/generate_cas_suites.sh build > build/t0_gatecheck.log 2>&1` and confirm it
  regenerates the suite list from sources and exits 0 (it fails loud on any unclaimed suite —
  that failure mode is the point).
- [ ] **Step 3: model-tool preflight (worktree state, not a deliverable).**
  `sha256sum "$(readlink -f tmp/tla2tools.jar)"` — if it is not
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`, obtain the official
  `tla2tools.jar` whose digest matches the pin (the checker recorded in
  `docs/superpowers/models/CaGcDestructiveGateCore_RESULTS.md`: TLC `2026.07.18.145032`, rev
  `30cc360`; source: the tlaplus/tlaplus GitHub releases/nightly), place it at
  `tmp/tla2tools-official.jar`, verify the digest, and repoint:
  `ln -sf tla2tools-official.jar tmp/tla2tools.jar`. If a matching jar cannot be obtained, STOP
  the model lane and escalate — never weaken the pin or let a runner pick another jar. Record the
  verification output in the ledger. Every future model-lane session repeats this check.
- [ ] **Step 4: Commit** (docs + tooling only):
  `ca: stage B — batch audit prose findings; version the CA gate tooling`.

---

### Task T1: Task 6 remainder — ref-side read contract and sentinel retirement {#t1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
  (and `.h` as needed for handle plumbing)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.{h,cpp}`
  (delete `resolveLifeOrSentinel` in slice c)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasNamespaceLifeId.h`
  (delete `stageATransition` in slice c)
- Modify: `src/Disks/tests/cas_test_helpers.h` (the single fixture seam)
- Create: `src/Disks/tests/gtest_cas_ref_read_contract.cpp`
- Modify: `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp` (the `list`-arm test)
- Modify: ~39 test files currently naming `stageATransition` (slice c, mechanical; re-derive the
  list with the slice-c grep)
- Modify (only if the T1a classification finds class-4/5 sites there): the ref table-cache layer
  marked by the `RefTableCacheEviction*` test family

**Interfaces:**
- Consumes: `CasRefCatalog::Snapshot` (the one-read-per-operation seam Task 5 established);
  `Pool::getNamespaceFile(const NamespaceLifeId &, const std::string &)` and siblings;
  `CasRefCatalog::lifeIfCataloged(Backend &, const Layout &, const RootNamespace &) ->
  std::optional<NamespaceLifeId>`.
- Produces: catalog-free hot ref reads through held lives; the named fixture seam in
  `cas_test_helpers.h` that every CA test uses for sentinel/raw-shape construction; a tree with
  zero `stageATransition` build-input hits.

**Read-side contract (normative, restated in full). The zero-GET rule and the mandatory-read
rule apply to DISJOINT surfaces — do not blur them:**
- **Held ref reads and lists: zero catalog GETs.** Live ref readers hold a `NamespaceLifeId`; an
  old handle can address only its old physical prefix. A stale reader may see stale data or
  `NotFound`, never a newer incarnation, and never a catalog-fence rejection introduced solely by
  the hot read.
- **Namespace-file read/write/remove/list through a held life: zero catalog GETs** (already
  landed except the `list` pin — slice T1b).
- **Ref mutations: an admission/authority catalog snapshot is permitted and may be mandatory.**
  Mutations are not hot reads; classes 1–3 of the T1a taxonomy keep their reads.
- **Current-life destructive cleanup: per-key revalidation is mandatory** (the class-3 reads).
- **Stale ref WRITER:** if a held ref-writer API exists at a T1a-classified site, a stale writer
  may write only to its old incarnation or fail, never target the new life — and it needs its own
  test. If no held ref-writer API exists, do NOT create one for symmetry; record in the report
  that the requirement is vacuous at this seam (the namespace-file delayed-writer contract
  already covers the write side that exists) and drop it.
- Within one operation, two life resolutions must be unable to observe different catalog
  snapshots: pass the operation's one `CasRefCatalog::Snapshot`, never re-read.
- Catalog resolution may be optional; it must never recover authority from an object key or LIST
  result.
- Destructive cleanup of a **current** catalog life revalidates the exact life, catalog entry
  token, and fence immediately before every per-key `deleteExact`; token changed before the first
  delete → delete nothing; changed between two deletes → the first may have landed, the second is
  refused. Task 5's dead-life janitor keeps its stronger-ordering exception (fresh catalog cut per
  LIST page, GC-fence check, exact-token delete per key, no O(keys) catalog GETs); do not
  generalize that page-cut optimization to current-life `cleanupRefObjects`.

#### Slice T1a — classification, then held-life ref reads {#t1a}

- [ ] **Step 1: classify the ten catalog reads.** `grep -n "CasRefCatalog::read"
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp` (expect ~10
  sites; re-derive). Classify each site in the task report into exactly one of:
  1. admission/identity resolution — one snapshot per operation required;
  2. mutation authority — the read may be mandatory; **keep**;
  3. current-life destructive revalidation — mandatory; **keep**;
  4. held-handle hot path — a catalog read is forbidden; **remove**;
  5. duplicate read within one operation — **remove**.
  Only classes 4 and 5 are removal candidates. The classification table (site, enclosing function,
  class, disposition, one-line reason) lands in the task report **before the first production
  edit**. If a site resists classification, that is a design question for the controller, not a
  guess. The classification also answers the stale-ref-writer question from the contract block:
  if it finds a held ref-writer seam, that seam gets its own stale-writer test in Step 2; if it
  finds none, the report records the requirement as vacuous and no API is invented for symmetry.
- [ ] **Step 2: write the two failing ref tests** in new
  `src/Disks/tests/gtest_cas_ref_read_contract.cpp` (suite `CasRefReadContract`), using the
  fixtures of `gtest_cas_ns_file_read_contract.cpp` as the template for lifecycle-real
  drop/rebirth (catalog `casUpdate` to `Removing`, `deleteCompletedRemoving` with cleanup evidence
  under a held fence, `casAdmitEntry` for the successor — not raw sentinel writes):

```cpp
/// Sketch — adapt to the fixture idioms of gtest_cas_ns_file_read_contract.cpp.
TEST(CasRefReadContract, HeldLifeAfterSameNameRebirthReadsStaleOrNotFoundNeverSuccessorRefs)
{
    /// life1: publish a ref under a held NamespaceLifeId; keep the handle (and its reader/
    /// snapshot state) alive. Drop the namespace and admit life2 under the same RootNamespace;
    /// publish a DIFFERENT ref value at the same logical name under life2.
    /// Assert: every read through the life1 handle yields life1's value or NotFound;
    /// EXPECT_NE(read_through_life1, life2_value) on every observation; a fresh resolution
    /// yields life2's value (positive control that life2 is live and readable).
}

TEST(CasRefReadContract, HotRefReadThroughHeldLifeIssuesZeroCatalogRequests)
{
    /// Open the table / resolve the life once. Reset the recording backend's op journal
    /// (the CasNamespaceFileDiskProfile pattern). Perform listRefs + point ref reads through
    /// the held life. Assert the journal records ZERO requests against layout.refCatalogKey()
    /// (and a positive control: the ref stream/snapshot keys WERE touched, so the journal
    /// is alive).
}
```

- [ ] **Step 3: run both, verify they fail** for the right reason:
  `ninja -C build unit_tests_dbms > build/t1a_red_build.log 2>&1 && build/src/unit_tests_dbms
  --gtest_filter='CasRefReadContract.*' > build/t1a_red_test.log 2>&1`; expected: both FAIL —
  the alias test observing successor data or the hot read journal showing catalog GETs. If either
  passes already, STOP: the contract may already hold at that seam; reclassify before writing
  production code, and record the finding.
- [ ] **Step 4: implement** — plumb held lives through the class-4 sites and delete class-5 reads.
  Pass the operation's existing `CasRefCatalog::Snapshot`/resolved life down the call chain.
  Prefer passing the held `NamespaceLifeId` directly; add a `fromLiveHandle` factory ONLY if some
  site genuinely cannot receive the value, and under the authority-source restriction (no minting
  from `RootNamespace`/LIST key/path). Class 2/3 sites are untouched.
- [ ] **Step 5: green + no-regression**:
  `ninja -C build unit_tests_dbms > build/t1a_green_build.log 2>&1 && build/src/unit_tests_dbms
  --gtest_filter='CasRefReadContract.*:CasRefGcCleanupAuthority.*:RefTableCacheEviction*' >
  build/t1a_green_test.log 2>&1`; expected: PASS. The `CasRefGcCleanupAuthority` suite green
  proves the class-3 revalidation reads survived.
- [ ] **Step 6: Commit** `ca: ref — held-life ref reads; catalog reads classified, hot path catalog-free`
  (classification table in the report; commit message names the report).

#### Slice T1b — namespace-file `list` arm + Task 9 evidence re-check {#t1b}

- [ ] **Step 1: write the failing `list`-arm test** in
  `src/Disks/tests/gtest_cas_ns_file_read_contract.cpp`:

```cpp
TEST(CasNamespaceFileReadContract, ListThroughHeldLifeIssuesZeroCatalogRequests)
{
    /// Resolve the life once; write two files under it; reset the recording journal;
    /// call listNamespaceFiles(life) (via the Pool surface); assert both names come back,
    /// AND the journal shows zero requests to layout.refCatalogKey()
    /// (positive control: the journal recorded the _files/ prefix LIST).
}
```

- [ ] **Step 2: run, verify outcome honestly.** Expected from the audit: the production path is
  already catalog-free, so this test may pass immediately. If it passes: record it as a
  **coverage pin, not a fix** (no false RED claim), and keep it. If it fails: implement the
  minimal fix in `CasPlainObjects::listNamespaceFiles` and record genuine RED/GREEN.
  Log: `build/t1b_list_test.log`.
- [ ] **Step 3: re-check the movable Task 9 claim.** The closure note
  `docs/superpowers/cas/2026-08-02-r1-verbatim-file-aliasing-closure.md` claims steady
  namespace-file operations add no hot-path catalog request. With the `list` arm now pinned,
  re-read that sentence against the new test's evidence; append a one-line evidence update to the
  note ONLY if the fact changed (expected: it did not — the update is then not written).
- [ ] **Step 4: Commit** `ca: ref — namespace-file list arm pinned catalog-free` (report records
  the name-equivalence mapping: the two committed tests ↔ the two originally-specified names, so
  no later gate "finds" them missing).

#### Slice T1c — one fixture seam, then sentinel retirement (three commits) {#t1c}

- [ ] **Step 1 (commit 1 of 3): the seam.** In `src/Disks/tests/cas_test_helpers.h`, introduce ONE
  named fixture entry point for every deliberately-nonproduction shape, e.g.:

```cpp
/// TEST-ONLY seam. The single place tests obtain identities and catalog shapes production
/// cannot create. Divergences from production, exhaustively:
///   1. fixtureLife(ns) returns the deterministic sentinel identity (production lives are
///      opaque and catalog-born);
///   2. admitRawLiveEntry(...) writes state = Live with NO _ckpt (production reaches Live
///      only through completeCreation, which publishes _ckpt first) — kept deliberately:
///      recovery/failure tests need a Live row without _ckpt;
///   3. writeRefLogTxnRaw(...) writes at the fixture identity, bypassing birth.
struct CasTestFixtureSeam { /* ... */ };
```

  The seam initially DELEGATES to `NamespaceLifeId::stageATransition` internally — no behavior
  change, one new name. Do NOT "repair" `Live`-without-`_ckpt` fixtures by fabricating `_ckpt`;
  they intentionally test otherwise-unreachable inputs. Build + targeted tests green
  (`build/t1c_seam_*.log`). Commit:
  `ca: tests — one named fixture seam for nonproduction CA shapes`.
- [ ] **Step 2 (commit 2 of 3): mechanical migration.** Re-derive the hit list:
  `git grep -n "stageATransition" -- 'src/' > build/t1c_migration_inventory.txt` (baseline
  expectation: ~291 test hits in 39 files + 1 production line + 1 declaration + 2 comments; the
  new `gtest_cas_writer_duties.cpp` use is in scope). Convert every test hit to the seam. Also
  convert the 17 test call sites of `resolveLifeOrSentinel` to either a real catalog resolution or
  the seam, per what each test actually needs. This is mechanical work suitable for delegation;
  review the diff, not the keystrokes. Full CA gate green
  (`build/t1c_migration_gate.log` — suite counts must not drop). Commit:
  `ca: tests — migrate fixtures onto the named seam`.
- [ ] **Step 3 (commit 3 of 3): retirement.** Delete `CasRefCatalog::resolveLifeOrSentinel`
  (declaration, definition, doc comment) and `NamespaceLifeId::stageATransition` (with its
  `__STAGE_A_TRANS` spelling and both comments referencing it). The seam now constructs its
  fixture identity locally (same bytes, test-only). Record the gate:
  `git grep -n "stageATransition" -- 'src/' ':!*.md'` → **zero hits** (paste the empty output +
  command into the report; generated logs and scratch reports are not build inputs). Full CA gate
  green. Commit:
  `ca: ref — read-side contract: handle-scoped reads and namespace files, pre-delete life revalidation`.
- [ ] **Step 4: T1 task gates.** (a) full CA gate green (post-T1c count recorded vs T1's own
  previous commit); (b) the dedup-log/write-side lane green — the `CasNamespaceFileDiskProfile.*`
  and `CasNamespaceFileRequestProfile.*` suites (in-gate) plus one integration run:
  `python3 -m ci.praktika run "integration" --test
  "test_content_addressed_s3 test_content_addressed_shared_pool" > build/t1_integration.log 2>&1`
  — this covers the write side of the zero-catalog contract (the namespace-file operation profile
  must not regress); (c) task review (spec compliance + code quality) passes.

---

### Task T2: Task 6b remainder — publication-ordering coverage {#t2}

**Files:**
- Create: `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp`
- Test subjects (read, not modified):
  `.../Pool/CasRefLedger.cpp` — `tryPublishSnapshotAndAdvanceCheckpointOnce`,
  `admitSnapshotPublishUnderStateLock`, `advancePublishBackoff`, `resetPublishBackoff`,
  `dispatchSnapshotPublisher`, `settleSnapshotPublish`

**Interfaces:**
- Consumes: the landed rename (one retry unit); the recording-backend op journal from
  `cas_test_helpers.h`.
- Produces: the three-effect ordering pinned by tests; the `Poisoned` refusal predicate tested;
  the publish backoff characterized.

**Normative ordering (restated):** 1. the immutable snapshot body becomes durable; 2. `_ckpt`
advances; 3. the new snapshot is adopted in memory. `Poisoned` blocks publication (a durable
transaction may be missing from the cached view) and triggers re-recovery. Retry/backoff semantics
are NOT changed by this task — it only pins them.

- [ ] **Step 1: write the four tests** (all in the new file, suite
  `CasRefSnapshotPublishOrdering`):

```cpp
TEST(CasRefSnapshotPublishOrdering, SnapshotBodyIsDurableBeforeCheckpointAdvances)
{
    /// Drive one successful publish through the Pool surface with the recording backend.
    /// Assert the op journal shows the body PUT strictly before the _ckpt CAS
    /// (compare journal indices of the two keys; positive control: both present exactly once).
}
TEST(CasRefSnapshotPublishOrdering, AdoptionHappensLastAndOnlyAfterBothDurableEffects)
{
    /// Inject a backend fault that fails the _ckpt CAS after a durable body PUT.
    /// Assert in-memory state did NOT adopt (newest snapshot id unchanged); then let the
    /// retry succeed and assert adoption happened exactly once, after both effects.
}
TEST(CasRefSnapshotPublishOrdering, PoisonedRefusesPublicationAndTriggersReRecovery)
{
    /// Drive the ledger into the Poisoned state (the recovery-mismatch path), then request
    /// a publish. Assert: zero body PUTs and zero _ckpt writes in the journal, and the
    /// re-recovery entry point was invoked (observable state transition), not a silent skip.
}
TEST(CasRefSnapshotPublishOrdering, PublishBackoffDecisionsAreCharacterized)
{
    /// Plain characterization (not differential): for a fixed sequence of admission attempts
    /// under a controlled clock, record admitSnapshotPublishUnderStateLock's accept/refuse
    /// decisions and pin them as literals; advancePublishBackoff then resetPublishBackoff
    /// change the subsequent decisions in the pinned way.
    /// NOTE: CasRequestControllerBackoff is a DIFFERENT mechanism; do not copy from it.
}
```

- [ ] **Step 2: run — each test must fail or be impossible before any support code, then pass.**
  These pin existing behavior, so most will pass immediately once the observation plumbing exists;
  for each, do a **sensitivity check**: apply a one-line mutation (swap the PUT/CAS order behind a
  test seam is not possible without production edits — instead assert against a deliberately
  wrong expected order first and watch it fail, proving the journal observes what it claims), then
  restore. Record per test which kind of evidence it has (genuine RED vs sensitivity check), using
  the mandatory wording. Logs: `build/t2_ordering_*.log`.
- [ ] **Step 3: full CA gate** green (`build/t2_gate.log`); the gate's suite generator must claim
  the new suite (it derives from sources — verify the new file is picked up).
- [ ] **Step 4: Commit** `ca: ref — snapshot publication ordering, poison refusal and backoff pinned`.
  Review; T2 closes milestone M2's remaining test debt.

---

### Task T3: Task 7 closure — decommission evidence {#t3}

**Files:**
- Modify: `src/Disks/tests/gtest_cas_decommission_catalog_duties.cpp`
- Test subject (read; modify only if the fence proves broken):
  `.../Tools/CasDecommission.cpp` — `decommissionPoolMember`'s retirement tail
- Integration: `tests/integration/test_content_addressed_drop_pool_member/test.py` (run, not
  modified)

**Interfaces:**
- Consumes: the landed implementation (`224aacd8eb9`): `_ckpt`-presence gate on `Removing`
  (`CORRUPTED_DATA` when absent), `admin->dropNamespace(life)` resumption, `request_gc_round` +
  `SCOPE_EXIT` wake, `retirement_catalog_cut`/`victim_still_owned` (arm a) and the
  `fresh_retirement_catalog` token/value comparison (arm b).
- Produces: both retirement refusal arms tested; the closure evidence T8's battery counts on.

**Normative decommission contract (restated):** after claiming the victim root, enumerate its
catalog entries exactly (no LIST); `Removing` with `_ckpt` → resume and append the terminal under
the claimed fence; `Removing` without `_ckpt` → corruption, still owned; a proved-complete
`Removing` row stays GC-owned (decommission wakes GC, never deletes rows); once the row is absent,
surviving opaque `_ckpt`/life objects are janitor debris; a FINAL exact catalog GET/token check
precedes slot retirement; retirement is FORBIDDEN while any entry owned by that root remains.

- [ ] **Step 1: fix the fence test.** The existing
  `FinalCatalogFenceKeepsSlotWhenVictimEntryAppearsDuringDrain` injects its late catalog entry
  during the roots-prefix LIST — before `retirement_catalog_cut` is read — so it exercises arm (a)
  and stays green if arm (b) is deleted. Two changes:
  1. rename it to `VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot` (what it proves), asserting
     arm (a)'s warning string ("catalog still owns victim namespaces");
  2. add `CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot`: rewire the injecting
     backend to fire on the **catalog GET** (the second read, `fresh_retirement_catalog`), i.e.
     mutate the catalog after the first cut is taken; assert retirement refused with arm (b)'s
     warning string ("catalog changed after the victim ownership check") and the slot retained.
- [ ] **Step 2: red-first for the new test**: run it against the current code with arm (b)'s
  comparison commented out locally (sensitivity check — mutation applied, output captured,
  mutation reverted, using the mandatory wording), and genuinely red-first if Step 1's rewiring
  finds arm (b) unreachable (that would be a CODE finding: escalate before patching).
  Logs: `build/t3_fence_*.log`.
- [ ] **Step 3: mutation demonstrations for the load-bearing duties** (each: apply, capture
  failing output, revert; mandatory wording in the report): the `_ckpt`-absence `CORRUPTED_DATA`
  throw (skip the head check), the GC-wake `SCOPE_EXIT` (drop the callback), and the ownership
  fence (skip `victim_still_owned`). Expected: the six existing tests catch each mutation; record
  which test caught which.
- [ ] **Step 4: the ownership inventory.** Into the task report: the enumeration showing
  decommission selects victims exclusively by exact catalog-name ownership —
  `grep -n "list(\|starts_with\|victim" .../Tools/CasDecommission.cpp` annotated line by line:
  entry iteration over `catalog_cut.catalog.entries` with the canonical-path-component match;
  `deleteListedPrefix` confined to `staging/<srid>/` and `serverRootDataPrefix(<srid>)` loose
  debris; **no life-key prefix fallback**. The inventory is the record the old residues demanded;
  the fixes themselves are already in the tree.
- [ ] **Step 5: the integration lane**, post-implementation by construction (HEAD contains
  `224aacd8eb9`): `python3 -m ci.praktika run "integration" --test
  "test_content_addressed_drop_pool_member" > build/t3_drop_pool_member.log 2>&1`; expected 2/2
  passed; paste the structured result into the report. NOTE: this lane carries a
  `STAGE-A CONTRACT` banner asserting suppression-era behavior — it passes TODAY and is edited by
  T6, not by this task.
- [ ] **Step 6: full CA gate green** (`build/t3_gate.log`); review; **Commit**
  `ca: decommission — catalog-exact duties; retirement fenced on owned entries` (the plan-required
  closure subject; contains the test fixes + report reference).

---

### Task T4: Task 8 closure — duty queue and orphan nomination {#t4}

**Files:**
- Modify: `src/Disks/tests/gtest_cas_orphan_nomination.cpp` (T-1, T-3)
- Modify: `src/Disks/tests/gtest_cas_writer_duties.cpp` (T-2)
- Modify: `.../Gc/CasOrphanManifestSweep.{h,cpp}` and its two test callers (C-1)
- Test subjects (read): `.../Pool/CasPartWriteTxn.cpp` (dtor), `.../Pool/CasPool.{h,cpp}` (duty
  queue + `mutateRefsAfterWriterCleanup`), `.../Gc/CasGc.cpp` (`orphan_sweep` phase)

**Interfaces:**
- Consumes: `Pool::enqueueWriterCleanupDuty` (noexcept, sticky-failure bit),
  `Pool::writerCleanupDutiesPending`, `Pool::drainWriterCleanupDuties`,
  `planManifestCursorPage` → `ManifestSweepResult::Nomination` → `foldDeltasIntoGeneration`'s
  trailing `std::vector<BlobSourceRetirement>`.
- Produces: the Q-1 ownership decision executed and tested; a production surface with no
  unretired-delete manifest API.

**Q-1 decision (normative, adopted, ownership stated precisely):**
- the writer cleanup duty **owns precommit-owner settlement and the active-build-floor pin, not
  physical body deletion**. Its ordering: (1) append the exact `OwnerTransition` removal, or
  observe conclusive absence; (2) only then `retireBuildSeq`; (3) only then remove the duty. A
  settlement failure retains the duty for retry — no stop between the actions leaves the
  obligation ownerless (this is the shape `Pool::drainWriterCleanupDuties` implements; the
  existing test deliberately requires the body to still EXIST after settlement);
- **physical unreachable bodies of `Rejected`/`Uncertain`/`Durable` attempts remain
  GC/orphan-sweep-owned** — the writer path never exact-deletes a body;
- `Uncertain` or potentially `Durable` precommits are never settled destructively by the writer
  path; the orphan sweep nominates and deletes their bodies after its own reachability and
  authority checks;
- missing attribution → suppression, never a destructive fallback.

- [ ] **Step 1: verify the settlement ordering in code.** Read `Pool::drainWriterCleanupDuties`:
  confirm the order is OwnerTransition-append-or-proven-absence → `retireBuildSeq` → duty
  removal, and that the exception path restores `draining` and RETAINS the duty. If the current
  code retires the duty (or the build seq) before the settlement is durable, that is a CODE
  finding — fix it under a failing test (`DutySurvivesSettlementFailureForRetry`: inject a
  throwing backend on the settlement append; assert `writerCleanupDutiesPending()` is still true
  after the throw and the next drain settles it). If the ordering already holds, write the same
  test as a pin and record it as such. Log: `build/t4_ordering_*.log`.
- [ ] **Step 2 (T-2): the reject-arm wedge drain.** Add to `gtest_cas_writer_duties.cpp`:

```cpp
TEST(CasWriterDuties, WedgeResolvedAsRejectDrainsTheDutyAsNoOp)
{
    /// Drive a lane into the WEDGED state with an uncertain grant (the
    /// UncertainAdoptedGrant... test shows how), then resolve the wedge as REJECT
    /// (durable absence). Assert: the next mutation drains the duty as a no-op
    /// (no OwnerTransition removal appended for the absent precommit), the wedge is
    /// cleared (refLaneWedgedForTest false), and minActive advances past the rejected seq.
}
```

  Red-first is genuine here (the scenario has model coverage only). Logs: `build/t4_reject_*.log`.
- [ ] **Step 3 (T-1): tighten the two loose fences** in `gtest_cas_orphan_nomination.cpp`:
  `CorruptManifestIsRetainedAndSurfaced` and `TokenAbaIsRetainedAndSurfaced` replace
  `EXPECT_THROW(runRegularRoundReclaiming(*f.gc), DB::Exception)` with
  `expectThrowsCode(ErrorCodes::CORRUPTED_DATA, [&] { runRegularRoundReclaiming(*f.gc); })`
  (the helper `gtest_cas_writer_duties.cpp` already uses). Run; both green.
- [ ] **Step 4 (T-3): accounting on the real round.** Extend
  `RetiresExactManifestSourcesBeforeDelete` with the two assertions currently living only in the
  synthetic test: `retired.unmatched_removes == 0` and the B2 `applied` ordinal vector byte-stable
  across the nominating round — so neutrality is proven where the end-to-end path runs. This also
  discharges Q-1 acceptance 4 (accounting names each debris class's owner) together with Step 1's
  pin.
- [ ] **Step 5 (C-1): remove the footgun from the production surface.**
  `sweepManifestCursorPage` plans a page and exact-deletes with no source-edge retirement and no
  `gc/state` adoption — the S42 shape, callers are tests only. Move it out of
  `CasOrphanManifestSweep.{h,cpp}` into the test side (e.g. a static helper in a small
  `src/Disks/tests/cas_sweep_test_support.h` included by `gtest_cas_orphan_manifest_sweep.cpp` and
  `gtest_cas_sweep_deletion_premise.cpp`, or directly into the TUs if only trivially shared). A
  comment-only disposition is EXCLUDED. Production translation units compile without it;
  `git grep -n "sweepManifestCursorPage" src/ | grep -v tests` → zero hits (record command +
  output).
- [ ] **Step 5b: Q-1 acceptance mapping, recorded in the report** — each condition names its
  evidence:
  1. `Rejected` debris does not leak forever → Step 2's reject-arm test + Step 1's
     retention-on-failure pin, **plus the physical half: the PHYSICAL body of a rejected attempt
     is eventually nominated and deleted by the orphan sweep** (duty settlement alone leaves the
     body in place by design). If no existing test drives a rejected attempt's body end-to-end
     through nomination to deletion, add `RejectedAttemptBodyIsEventuallyNominatedAndSwept`;
  2. eventual nomination for `Uncertain`/`Durable` bodies → `RetiresExactManifestSourcesBeforeDelete`
     (nomination on the real round) +
     `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant` (writer-abandoned remnant
     reaches the sweep); if neither covers a writer-abandoned `Uncertain` body end-to-end, add
     that scenario rather than arguing coverage;
  3. missing attribution → suppression, never destructive fallback: verify (and cite in the
     report) that nomination planning is gated on `!suppress_destructive` with a positive test
     observation (a suppressed pass produces zero nominations — extend an existing suppression
     test with that assertion if none records it);
  4. accounting names each debris class's owner → Step 4's real-round accounting assertions +
     Step 1's pin.
- [ ] **Step 6: mutation demonstrations** for the load-bearing Task 8 behaviors (mandatory
  wording; apply → capture → revert): drop `mutateRefsAfterWriterCleanup` from the `dropRef`
  delegate (expect `DropRefServicesPendingDutyBeforeRemovingTheRef` to fail); make
  `enqueueWriterCleanupDuty` drop the duty on the `Uncertain` branch (expect
  `PendingDutySkipsCleanFarewellAndSuccessorSweepsTheCrashRemnant` to fail); skip source-edge
  retirement in the nomination path (expect `RetiresExactManifestSourcesBeforeDelete` to fail).
- [ ] **Step 7: gates + closure.** Full CA gate green (`build/t4_gate.log`) and the S3 lane:
  `python3 -m ci.praktika run "integration" --test "test_content_addressed_s3
  test_content_addressed_gc_s3" > build/t4_s3.log 2>&1` — expected all passed (destruction still
  suppressed at this point; the lanes assert the suppression-era contract). Review. **Commit**
  `ca: writer/gc — duty queue, uncertain-grant retirement guard, neutral orphan nomination (R2+R3)`
  (the closure subject; message records the Q-1 decision in one sentence).

---

### Task T5: delete probe A (old Task 7a) {#t5}

**Files:**
- Modify: `.../Gc/CasGc.h` (delete `sampleRefListQuality` decl + contract comment + the
  `GcRoundPlanSignatureAccess` `friend`; fix the `RefScanSummary` doc)
- Modify: `.../Gc/CasGc.cpp` (delete the definition, the single call site under
  `PHASE … ref_list_probe`, the five ProfileEvents increments and six `extern` decls; keep
  `enumerateRefPrefix` — see Step 2; fix stale comments; re-derive phase numbering)
- Modify: `.../Pool/CasPool.h` (delete `PoolConfig::gc_probe_a_period` + doc block; it is a
  `PoolConfig` field only — verify no `DECLARE` in `ContentAddressedSettings.cpp`, no XML/DDL
  binding, in the Step-1 inventory)
- Modify: `src/Common/ProfileEvents.cpp` (delete the six events: `CasGcRefScanDisagreements`,
  `CasGcProbeADue`, `CasGcProbeAPerformed`, `CasGcProbeASkipped`, `CasGcProbeAHolePresent`,
  `CasGcProbeAHoleAbsent`)
- Modify: `src/Interpreters/ContentAddressedGarbageCollectionLog.cpp` (drop `ref_list_probe` from
  the phase enum + both column comments; generic phase plumbing untouched)
- Modify: `docs/en/operations/system-tables/content_addressed_garbage_collection_log.md` (delete
  the `ref_list_probe` row — user-facing doc and enum change together)
- Delete: `src/Disks/tests/gtest_cas_holey_list_detector.cpp` (verify all 3 tests are probe-A-only
  first)
- Modify: `src/Disks/tests/gtest_cas_retirement_sweep.cpp` (delete
  `ProbeAReportsAHintHoleAndTheRoundFoldsThroughItAnyway` and
  `TheDetectorsCadenceIsOnEveryFoldingRoundsRow`; CONVERT
  `TheRoundEnumeratesTheRefPrefixOnceAndTheDetectorAddsTheSecond` →
  `TheRoundEnumeratesTheRefPrefixExactlyOnce`)
- Modify: `src/Disks/tests/gtest_cas_ref_catalog.cpp` (delete the `ProbeSignature`
  `static_assert` pin), `src/Disks/tests/gtest_cas_gc_log.cpp` (phase-order list + the
  `metricsOf(rows, 0, "ref_list_probe")` assertion), `src/Disks/tests/cas_test_helpers.h`
  (`HintHoleBackend` doc), `src/Disks/tests/gtest_cas_gc_arithmetic_intake.cpp` (comment)
- Modify (Python, ALL live consumers): `utils/ca-soak/soak/signals.py`,
  `utils/ca-soak/soak/metrics.py` (`probe_a_holes` column + projection),
  `utils/ca-soak/soak/run.py`, `utils/ca-soak/tests/test_checkpoint_signal_capture.py`,
  `utils/ca-soak/tests/test_metrics_signal_columns.py`, `utils/ca-soak/tests/test_signals.py`
- Modify (docs, same commit): `docs/superpowers/cas/2026-07-28-ref-rework-adjacent-findings.md`
  (verify R7's supersession note matches what was deleted), `docs/superpowers/cas/todo-20260726.md`
  (mark item 1's "Remove probe B1" proposal OVERRIDDEN — B1/B2 are KEPT),
  `docs/superpowers/cas/BACKLOG.md`, `docs/superpowers/cas/2026-07-28-stage-a-RESULTS.md`,
  `docs/superpowers/cas/2026-07-28-stage-a-retirement-verdicts.md`,
  `docs/superpowers/cas/11-walkthrough.md`,
  `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md` (§5's probe-A sentence)

**Interfaces:**
- Consumes: nothing from other tasks (T3 ordering is for lane ownership, not code).
- Produces: exactly one full `cas/ns/stream/` enumeration per round, asserted on every round; the
  post-T5 CA-gate test count T8 uses as its baseline.

**KEEP (named so the deletion cannot over-reach):** B1 (`logs_accounted == logs_applied` on
`fold_ref_intake`) and B2 (ordinals/`produced=false` accounting); the mount capability probe
(`Backend/CasProbe.h`, `Backend/CasSentinelProbe.h`); false positives:
`gtest_cas_upload_fanout.cpp`'s `probe_acquired`, `gtest_cas_gc_shard_plan.cpp`'s local `probe_a`
`ManifestId`, poco's `probe_and_set…ca_location`.

- [ ] **Step 1: the inventory (the deletion's red-first substitute).**
  `git grep -nE "probe_a|ProbeA|ref_list_probe|sampleRefListQuality|RefScanDisagreements" --
  . > build/t5_inventory_before.txt` at the task's base commit (record the SHA). Paste into the
  task report; classify every hit: delete / correct / false positive with reason. The Files list
  above is the expectation; the inventory is the authority.
- [ ] **Step 2: decide the helper's fate on measured callers.** `enumerateRefPrefix` has THREE
  callers (`listRefPrefix`, the detector, the rebuild path). The detector caller dies; the helper
  is **kept** (rebuild still needs it); record in the report that the old collapse-the-helper
  option is dead. Also inspect `RefScanSummary` for fields whose only consumer was the detector
  and delete them with it.
- [ ] **Step 3: re-derive the phase numbering.** The `PHASE N/…` comment sequence in
  `Gc/CasGc.cpp` is already corrupt (9 and 17 missing, 15 duplicated) and the emitted phase list
  has 19 names including `pre_fold_ref_drain`. Renumber from the actual `GcPhaseTimer` sites
  after deleting `ref_list_probe`: `grep -n 'GcPhaseTimer' .../Gc/CasGc.cpp` is the source of
  truth; the post-deletion emitted-phase count (expected 18) goes into `gtest_cas_gc_log.cpp`'s
  phase-order expectation.
- [ ] **Step 4: convert the enumeration test.** `TheRoundEnumeratesTheRefPrefixExactlyOnce`
  asserts, by LIST count attributed to the `cas/ns/stream/` prefix, exactly ONE full enumeration
  per folding round — **on every round in a multi-round run** (the old cadence sampled round 16;
  a one-round test proves nothing), with the bounded `cas/ns/` janitor page counted separately
  and never mistaken for a hot scan.
- [ ] **Step 5: delete + build + gates.** Before running, write the expected CA-gate delta into
  the report (3 tests + 1 suite from the detector file, plus 2 deleted retirement-sweep tests;
  derive the exact number from the inventory). Full CA gate: count goes DOWN by exactly that
  delta relative to the immediately preceding commit of this lane (`build/t5_gate.log`). Soak
  unit tests: `python3 -m pytest utils/ca-soak/tests -q > build/t5_soak_tests.log 2>&1` — green
  after the Python updates.
- [ ] **Step 6: after-grep.** Re-run the Step-1 grep → only the classified false positives remain
  (`build/t5_inventory_after.txt`, pasted). **Commit** (ONE commit, docs included):
  `ca: gc — delete probe A: no second full ref LIST per round`.

---

### Task T6a: frontier-attribution risk spike {#t6a}

**Files:**
- Create: `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t6-frontier-attribution.md` (the
  verdict artifact T6 consumes)
- Modify (only if attribution needs a temporary counter): `.../Gc/CasGc.cpp` — an instrumentation
  commit is allowed but must be either kept as a permanent observability improvement (reviewed)
  or fully reverted before T6; no half-state.

**Interfaces:**
- Consumes: T0 only. Runs in parallel with T1/T3/T7 (read-mostly; any instrumentation commit
  coordinates with the single production writer).
- Produces: the attribution verdict that gates T6's flip.

**The question (the one potentially unbounded unknown in this plan):** healthy CA-local pools
measured `frontier INCOMPLETE` — `3155/3157` and `11358/11369` namespaces proven — with zero
anomalies and zero holds. Something non-anomalous leaves namespaces unproven, and the production
warning does not carry enough information to attribute it.

- [ ] **Step 1: reproduce instrumented.** Run the `05007`/`05010`-shaped stateless workload
  locally (`tests/queries/0_stateless/05010_content_addressed_mounts_gc_health.sh` is the
  cheapest known reproducer); attribute EVERY unproven row to one of: post-LIST append above the
  frozen tail; probe-budget exhaustion (`frontier_unprobed_budget` observes this today);
  effective hold; genuine gap; undecodable `_ckpt`; whole-fold abort. Extend the frontier warning
  or add a temporary counter if the existing signals cannot discriminate. Logs under
  `build/t6a_*.log`.
- [ ] **Step 2: the verdict.** Write `t6-frontier-attribution.md`: the attribution table with
  evidence, and ONE of:
  - **BENIGN-TRANSIENT** (e.g. append-above-tail): state why a healthy pool converges to a
    complete frontier between rounds, and what T8's soak must show (backlog and unproven count
    drain to zero STABLY). T6 may proceed on its other predecessors.
  - **REAL GAP**: the fix is a normal production change — model correspondence if it touches the
    modeled transitions, failing test, review — scheduled on the production-writer lane BEFORE
    T6. T6 stays blocked until the fix lands and the reproduction shows a complete frontier.
- [ ] **Step 3: Commit** the verdict artifact (and the instrumentation, if kept):
  `ca: gc — attribute the incomplete-frontier deficit on healthy pools`.

### Task T6: destruction enablement (old Task 7b) {#t6}

**Files:**
- Modify: `.../Gc/CasGc.h` (`UniversePolicy`: `kDefault` → the authoritative value; rename
  `AuthoritativeForTest` → `Authoritative`; the `kDefault` comment is rewritten to invariant +
  reason only, per Step 1 — the return-item inventory lives in the task report, never in the
  comment)
- Modify: `src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp` (the ONE intentional Stage-B edit:
  the kill-shot survives on PROOF, not suppression; the explicit-`AuthoritativeForTest` variant
  collapses into the production case)
- Modify: `src/Disks/tests/gtest_ca_wiring.cpp` (restore `EXPECT_EQ(after.unreachable, 0u)`),
  `src/Disks/tests/gtest_cas_gc_log.cpp` (restore `CasGcLog.EmitsStartFinishWithCounts`'s real
  assertion per its marker)
- Modify: `tests/broken_tests.yaml` (remove ALL FOUR entries: `05008_ca_gc_snap_prune`,
  `04290_content_addressed_no_leftovers`, `05010_content_addressed_mounts_gc_health`,
  `04295_content_addressed_mutation_no_leftovers`)
- Modify (the second marker class — suppression-contract assertions that go red on the flip):
  `tests/integration/test_content_addressed_drop_pool_member/test.py`,
  `tests/integration/test_content_addressed_gc_s3/test.py`,
  `tests/integration/test_content_addressed_ref_snaplog/test.py` (2 sites),
  `tests/integration/test_content_addressed_shared_pool/test.py` (2 sites),
  `tests/integration/test_cas_replicated_relink/test.py`,
  `utils/ca-soak/scenarios/cards/s28_s33_corner.py`,
  `utils/ca-soak/scenarios/framework/assertions.py` (5 sites),
  `utils/ca-soak/scenarios/tests/test_leftovers_stage_a.py` — each restored to assert the
  destruction-era contract its banner names
- Prerequisite artifact: `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t6-frontier-attribution.md`

**Interfaces:**
- Consumes: T1's read-side/pre-delete contract; T5's single-enumeration round; the complete
  marker inventory.
- Produces: production destruction enabled; every Stage-A return item closed with its real
  assertion; the destructive rounds T8's soaks measure.

**The gate formula (normative, corrected).** Computed once in `Gc::fold` under "THE DESTRUCTIVE
GATE":

```
frontier_complete = universe_authoritative
    && frontier_namespaces > 0
    && frontier_proven == frontier_namespaces

suppress_destructive = anomalies || carried_holds || !frontier_complete
```

Five independently-suppressing conditions: (1) anomalies non-empty; (2) carried holds non-empty;
(3a) `!universe_authoritative`; (3b) `frontier_namespaces == 0` — the empty-universe floor:
`{} == {}` must still fail closed; (3c) `frontier_proven != frontier_namespaces` (budget
exhaustion enters here — an exhausted probe budget leaves rows unproven). The flip changes ONLY
what `frontier_complete` is allowed to become (catalog-proven true instead of hard-wired false).
If the flip requires touching any other term, the flip is wrong and the task raises instead of
landing. Task 5's catalog-only pre-fold drain does not consult `suppress_destructive` (its
unresolved obligation aborts before DEFER/fold/adoption) — unchanged. No namespace-local
narrowing: every carried hold stays pool-wide suppression (blob in-degree is pool-wide; no
ownership-partition proof exists).

- [ ] **Step 0 (prerequisite, blocks the flip): consume T6a's verdict.** Read
  `.superpowers/sdd/2026-08-02-cas-stage-b-remaining/t6-frontier-attribution.md`. BENIGN-TRANSIENT
  → proceed (and carry its "what T8's soak must show" clause into T8's criteria). REAL GAP → the
  fix must already be landed and the reproduction re-run showing a complete frontier; otherwise
  this task stays blocked. The flip does not proceed on an unexplained deficit.
- [ ] **Step 1: the flip + rename** (one commit, code only): `kDefault = Authoritative`
  (renamed), the kill-shot test's expectations updated (same scenario now survives on proof —
  zero deletes because namespace A is IN the catalog and probed). The `kDefault` comment is
  REWRITTEN to state only the invariant and the reason (the gate opens only on a complete,
  catalog-proven frontier; anomalies/holds/incomplete frontier still suppress) — **no task
  numbers, no test-file lists, no counts**: the complete return-item inventory lives in the task
  report (Step 2), per the comment policy.
- [ ] **Step 1b: assert the formula, all five arms.** In the gtest layer (extend the kill-shot
  file or `gtest_cas_gc_frontier_gate.cpp`): a healthy round on a catalog universe yields
  `frontier_complete == true`, `suppress_destructive == false`, and performs real deletes; then
  each suppressor ALONE — one anomaly; one carried hold; a zero-namespace universe (the 3b floor,
  previously untested anywhere); one budget-exhausted round (3c) — each with EVERY delete family
  inert, asserted per family, not as an aggregate zero. **(3a) gets a MANDATORY C++ test — the
  model cannot stand in for it** (`CaGcDestructiveGateCore` pins `UniverseAuthoritative == TRUE`,
  and `sab_gate_accepts_empty_universe` sabotages the empty-universe floor, not authority): keep
  an explicit negative-policy test seam after the flip (the `runRegularRound` policy parameter,
  passing `StageA_Suppressed` explicitly) and run one round with it, asserting
  `suppress_destructive == true` and every delete family inert, per family.
- [ ] **Step 2: inventory-driven closeout (second commit).** Reproducible-inventory rule:
  `git grep -in "task 7b" -- 'src/' 'tests/' 'utils/' > build/t6_markers_before.txt` (expect 15
  non-doc files at baseline) plus `git grep -n "STAGE-A RETURN ITEM"` and
  `git grep -n "STAGE-A CONTRACT"`. For the three stateless tests already asserting the real
  contract (`05008`, `04290`, `04295`): remove their yaml entries, run them UNCHANGED, paste
  output — the drain-to-`PENDING = 0` loop passing is the end-to-end proof of the flip. `05010`:
  remove its yaml entry and run. The two weakened gtests: restore real assertions, green. The
  seven banner files: restore each to assert the destruction-era contract (e.g.
  `drop_pool_member`'s "Stage A must reclaim NOTHING" becomes the reclamation assertion its
  banner names). Exit condition: all three greps return zero non-historical hits
  (`build/t6_markers_after.txt`), zero yaml entries remain.
- [ ] **Step 3: gates.** Full CA gate; both CA-S3 stateless lanes; and
  `python3 -m ci.praktika run "integration" --test "test_content_addressed_gc_s3
  test_content_addressed_s3 test_content_addressed_shared_pool
  test_content_addressed_drop_pool_member test_content_addressed_ref_snaplog
  test_cas_replicated_relink" > build/t6_integration.log 2>&1` — destruction now ACTIVE: delete
  families nonzero in lane logs, zero anomalies. Logs summarized by subagent.
- [ ] **Step 4: Commits.** Commit 1 (Step 1+1b):
  `ca: gc — universe authoritative: production destruction enabled (Stage B)`. Commit 2 (Step 2):
  `ca: tests — close every Stage-A return item on the destruction contract`. Separately
  revertable; no delete-side optimization rides along.

---

### Task T7: model lane — Task 10 closure {#t7}

**Files (lane A):**
- Create: `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md`
- Modify: `docs/superpowers/models/CaGcRootLocalPartManifestCore.tla` and/or its configs (only if
  the 10a verdict kills the premise)
- Modify+commit: `docs/superpowers/models/run_gc_partmanifest.sh` (the unstaged ninth-family
  rewrite: 43 fast rows + 5 `SLOW=1` rows, five expectation kinds, temporal gate wiring,
  fail-closed selection)

**Files (lane B):**
- Modify: `docs/superpowers/models/run_gclease.sh`, `run_gcshardincarnation.sh`,
  `run_b140danglemerge.sh` (add `check_tlc_pin`)
- Create/extend: the 10c before/after results record (in
  `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` per-family sections, plus RESULTS files
  for `CaGcLeaseCore` and `CaB140DangleMerge` which have none)
- Modify: `docs/superpowers/models/README.md` (runner column: the four `(inline TLC)` rows),
  `docs/superpowers/models/2026-07-28-v9-phase-RESULTS.md` (`{#fix-runners}` closing paragraph)

**Files (10f disclosure):**
- Modify: `docs/superpowers/models/CaGcDestructiveGateCore_RESULTS.md` (one sentence),
  `docs/superpowers/models/CaGcDestructiveGateCore.tla` (one comment at `UniverseAuthoritative`)

**Interfaces:**
- Consumes: the T0 jar preflight (pinned checker at `tmp/tla2tools.jar`).
- Produces: every model runner asserted and pinned; the 10a verdict T8's battery cites; the named
  sharding-arm debt.

**Lane A (strictly sequential — one model):**

- [ ] **Step A1: the 10a verdict.** Audit `listedTok` in `CaGcRootLocalPartManifestCore.tla`: the
  skip premise is "LIST surfaces a live root-shard token; `CanSkipShard` compares it against
  persisted fold coverage to skip the body read" — a durable-coverage claim gated on a listing.
  Judge it against the landed architecture (universe from catalog; LIST-independent recovery;
  LIST may only offer newer candidates/diagnostics/garbage nominations). Write the verdict into
  the new `CaGcRootLocalPartManifestCore_RESULTS.md`: either the premise SURVIVES (say which code
  seam still legitimately consumes a listed token) or it DIED (rewrite or retire the affected
  configs — `EnableTokenDiff`, `GDiscoverSkip`, the `_sab_skip*` rows — and record which). Model
  edits, if any, are their own reviewed commit; TLC reruns are justified here (the model changes).
- [ ] **Step A2: the ninth family.** After A1 (the battery must run against the post-verdict
  model): run `SLOW=1 docs/superpowers/models/run_gc_partmanifest.sh >
  build/t7_gc_partmanifest.log 2>&1`; resolve every row to its expected outcome; write the
  RESULTS section (per-row table, checker identity, state counts). **Name the sharding-arm debt
  explicitly in RESULTS**: the three `known-model-error UnchangedCompositeVars` rows
  (`sab_crosssharddisplacement`, `sab_reducerownsfence`, `stage5_sharding`) mean the model's
  Phase-4 sharding arm is UNPROVEN and booked as `KNOWN` — a standing model debt, listed as such
  (it also enters T8's residual gate row). Verify the three temporal cfgs use `PROPERTY` (not
  `PROPERTIES`) so the classifier does not fall through to `error`. Commit runner + RESULTS:
  `ca: tla — gc_partmanifest whole-suite runner, ninth family asserted`.

**Lane B (independent of lane A):**

- [ ] **Step B1: pin the three runners.** `run_gclease.sh`, `run_gcshardincarnation.sh`,
  `run_b140danglemerge.sh`: source `tlc_temporal_gate.sh`, call `check_tlc_pin "$JAR"` before any
  TLC invocation (the `run_gcrounddefer.sh` shape).
- [ ] **Step B2: the before/after results record.** Run all four 10c runners
  (`> build/t7_10c_<name>.log 2>&1` each); record per-runner row tables + checker identity;
  create the missing RESULTS files for `CaGcLeaseCore` and `CaB140DangleMerge`; fix the README
  runner column and the `{#fix-runners}` paragraph (these two prose fixes are 10c closure
  content, not batch material). Commit:
  `ca: tla — 10c runners pinned and recorded; runner table corrected`.

- [ ] **Step C1: the 10f disclosure.** One sentence in `CaGcDestructiveGateCore_RESULTS.md`'s
  correspondence section and one comment at `UniverseAuthoritative == TRUE`: the term is pinned
  TRUE because the model gates the post-flip posture; its production coverage comes from T6's
  per-term assertions (state the reason, cite no plan). May ride with either lane's commit.

---

### Task T8: Stage B gates (old Task 11) {#t8}

**Files:**
- Create: `docs/superpowers/cas/2026-08-XX-stage-b-RESULTS.md` (date at execution)
- Create: the preserved-specimen directory (named path recorded in the results file)
- Build dir: `build_tidy` (its own configure; NEVER add `ENABLE_CLANG_TIDY` to an existing build
  dir)

**Interfaces:**
- Consumes: everything T1–T7 produced; the post-T5 CA-gate count as the battery baseline.
- Produces: the `STAGE B: PASS`/`FAIL` verdict; the specimen T9 samples.

**Early-startable (any idle window, before predecessors close):**

- [ ] **Step E1: AMD tidy lane.** Configure `build_tidy` with the `BuildTypes.AMD_TIDY` cmake
  shape (`ci/defs/defs.py` profile; Debug, no-sanitizer, x86_64, `-DENABLE_CLANG_TIDY=1`; the
  repo `.clang-tidy` supplies checks; set `CTCACHE_DIR` for the cache). Build to
  `build_tidy/tidy_build.log`. **Scope (user directive 2026-08-03): every CAS-related diagnostic
  is FIXED, regardless of which stage introduced it** — files under
  `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, `src/Disks/tests/gtest_ca*`/
  `gtest_cas*`/`cas_*`, and CA-owned regions of shared files (e.g.
  `ContentAddressedGarbageCollectionLog.cpp`). Fixes land as a reviewed slice with targeted
  re-tidy of the changed TUs + the affected unit suites (release+ASan); a diagnostic whose "fix"
  would change deliberate semantics (e.g. an intentionally-uninitialized field) is suppressed
  with a NOLINT + reason instead, recorded per site. Non-CAS diagnostics are reported without
  fixing.
- [ ] **Step E2: executable-prose sweep.** Grep the Stage-B diff (old plan Task 0 baseline →
  current) for "whenever/always/must also/in the same change" in comments; for each hit, convert
  the rule to something that fails a build or test, or record in `BACKLOG.md` why it cannot be.
- [ ] **Step E3: report skeleton + suite inventory + artifact directories.**
- [ ] **Step E4: pin the four soak commands BEFORE any long run.** The harness lives in
  `utils/ca-soak` (run from that directory). Known shapes:
  - general 90-minute soak (run d): `python3 -m soak.run --seed <seed> --phase 3 --duration 90m
    > <specimen_dir>/general_soak.log 2>&1` — phase 3 is the real duration-driven soak (phase-1
    `--ops` runs finish ~10× faster and do not qualify);
  - scenario runs (a)–(c): `python3 -m scenarios.run --scenario <name> --seed <seed>
    --duration <30m|…> > <specimen_dir>/<name>.log 2>&1`; enumerate the registered names first
    with `python3 -m scenarios.run --list > build/t8_scenario_list.txt` (churn is the
    S34/S35 family; rebirth and decommission selectors are chosen from that listing).
  Record in the report skeleton, before starting: the exact command line per run (a)–(d), the
  seed, the compose variant, the specimen directory (one named directory, e.g.
  `utils/ca-soak/logs_archive/2026-08-XX-stage-b-specimen/`, created up front), and the PASS
  criteria each run is checked against. **If a required scenario does not exist as a card**
  (rebirth with concurrent namespace-file readers/writers, or the decommission
  hidden-`Removing` shape), build the card and RUN it — never skip a required scenario as
  needs-infra, and never let a retired premise return INCONCLUSIVE (fail at entry instead).
  Reminder: soaks never overlap praktika runs on this box.

**The residual-cleanup gate row — enumerated in full, each item walked and recorded as
fixed / no-longer-applicable / accepted (one line of why):**

1. Task-1 review minors — **this is the final list; T8 performs NO archaeology.** The verbatim
   per-item enumeration did not survive in any artifact (search performed once, at plan-writing:
   the survivors are the characterizations in the old plan's placement-sweep record, and the
   midpoint audit records the loss as historical-unrecoverable). The walkable items, exhaustively:
   (1a) a stale comment; (1b) `"spells"` → `"decodes to"` wording; (1c) an IWYU include; (1d) a
   report/table count mismatch; (1e) a naming-collision clause; (1f) a noted tension. Walk each
   against the current tree: fixed / no-longer-applicable (unlocatable in the current tree counts
   as this, one line saying so) / accepted. Minor 2 (the `listNamespaces` DDL-path ruling) was
   re-opened and RESOLVED by Task 1c's record-and-continue reversal — record, no action. Minor 8
   (bump-B verification) was re-homed to Task 4 and closed with the foundation — record, no
   action.
2. Task-1 re-review MINOR-B (`rebuildBaseline` gen-0 nested-shape exposure) — discharged in Task
   1c (second guard in `recoverRefTable` with an equivalence argument); verify the guard exists,
   record the symbol.
3. Task-1 re-review NITs C–F — content lost; recorded as **historical-unrecoverable in the
   midpoint audit at publication** (with the search evidence). T8 records "closed as
   historical-unrecoverable per the audit" and does NOT search again.
4. Task 5's deferred symmetric regression test for the exact-delete exception branch
   (`deleteCompletedRemovingAtSnapshot`'s `casPut`-throw path; the fence-callback siblings are
   covered, the CAS-throw branch is not) — implement it here or accept with one line of why.
5. The two comment-policy citations audit-t8 named: `ContentAddressedTransaction::writeFile`'s
   "(directive §namespace-file-requirements)" and `CasNamespaceLifeId.h`'s "Task 6 DELETES it"
   (the latter dies in T1c; verify). Fix = keep the reason, drop the citation.
6. The Q-2 ABA-edge sequencing observation (orphan-sweep edges retired before the
   `TokenMismatch` throw; cursor advanced in the same CAS) — record as accepted-by-design with
   the sequencing argument, or turn into a comment at the throw site.
7. The 10b sharding-arm `KNOWN` model debt (from T7-A2) — re-check it is named in RESULTS and
   carried to the post-B residual list.

**The battery and soaks (after all predecessors):**

- [ ] **Step 1: full CA gtest gate** vs the post-T5 baseline count (delta explained line by
  line: T1/T2/T3/T4 additions, T5 deletions).
- [ ] **Step 2: all CA integration lanes local, green** — the full selector set from T6 Step 3
  plus any CA lane the suite inventory (E3) lists beyond it; one praktika run per group, logs
  summarized by subagent.
- [ ] **Step 3: the four REQUIRED soaks**, each run with its E4-pinned command into the E4
  specimen directory. **Staged execution (user directive 2026-08-03): each scenario first runs a
  20-minute smoke variant; the full-length run starts only if its smoke survived** (any smoke
  failure → RCA before burning the long slot). The smoke runs are pre-qualification, not
  substitutes: PASS criteria and the specimen come from the full-length runs only.
  (a) **churn** — create/drop namespaces ≥1/s for ≥30 min under load: catalog entry count returns
  to baseline (flatness: O(Creating+Live+Removing)), zero alias reads, fsck clean;
  (b) **rebirth adversarial** — drop/recreate under concurrent readers + stale cleanup resume,
  readers INCLUDE namespace-file readers/writers: zero reads resolving to a newer incarnation
  across the whole run; `_files` debris from dead incarnations trends to zero via the janitor
  without ever blocking a rebirth;
  (c) **decommission** — victim with hidden `Removing` entries recovered under the claimed fence;
  completed rows deleted only by GC; leftover opaque checkpoints reclaimed by the janitor;
  (d) **the 90-minute general soak** (separate run, same PASS criteria as Stage A) carrying the
  **sequential-baseline destructive workload**: current sequential implementation only — no
  `MultiDelete`, no parallel deletes, no delete-side concurrency. "Faster" is explicitly not a
  goal; this is the honest cost baseline.
- [ ] **Step 3c: the cost inventory, every line MEASURED** (un-measurable lines NAMED as un-timed,
  never estimated). `GcPhaseTimer` is always-on and emits per-phase duration+metrics rows into
  `system.content_addressed_garbage_collection_log`, so five lines read off existing phase rows:
  `pending_deletes` (metrics `redeleted`/`graduated`/`deleted`/`absent`/`replaced`/`spared`);
  `manifest_deletes` (`attempted`/`deleted`); `orphan_sweep` (its retention breakdown);
  `ref_object_cleanup`; `namespace_cleanup` (`attempted`/`deleted`/`leaked`/`suppressed` +
  `janitor_pages`/`janitor_keys`/`janitor_deleted`). **Generation pruning is the exception**: it
  runs inside `round_commit` with no row of its own — measure via its metrics
  (`generations_visited`/`pruned_through`/`generations_referenced`) + the shared
  `deletePrefixWholesale` primitive; if that cannot separate it, it goes into T9's un-timed list
  BY NAME. Rounds-to-fixpoint from the round sequence. Report per line: invocation count, S3
  operations by verb, wall time, share of round time.
- [ ] **Step 3d: the six result criteria, as gate rows** (a row without a measurement is FAIL,
  not blank):

  | # | Criterion | Measured by | PASS |
  |---|---|---|---|
  | 1 | Healthy rounds really perform destructive work | per-family delete counts per round in `system.content_addressed_garbage_collection_log` | every family with work nonzero on healthy rounds; no family silently inert |
  | 2 | `ca-fsck --detail` finds no dangling / stale-edge | fsck at soak end AND a mid-soak checkpoint | zero dangling, zero stale-edge, both runs |
  | 3 | Backlog reaches zero STABLY | `pending_condemned` + cleanup backlog sampled per round to fixpoint | reaches zero and STAYS zero across ≥3 further rounds |
  | 4 | Holds/anomalies still suppress every irreversible path | inject one hold and one anomaly during the soak | all delete families inert for those rounds, per family; round still completes |
  | 5 | No second full stream LIST after probe-A removal | LIST counts per round attributed by prefix and phase | exactly ONE full `cas/ns/stream/` enumeration per round, EVERY round; the bounded `cas/ns/` janitor page reported separately |
  | 6 | Phase timings + S3 op counts give the baseline | the Step-3c inventory | recorded as the explicit `MultiDelete`/concurrency baseline, un-timed spans named |

- [ ] **Step 3e: insert-path guard.** The dedup-log-bearing workload's namespace-file operation
  profile is UNCHANGED vs the Task-4b baseline: compare per-operation backend request counts from
  the soak's `content_addressed_log`/ProfileEvents (not a micro-benchmark). Any increase on that
  path is a Stage-B FAIL.
- [ ] **Step 3f: PRESERVE the specimen.** Server logs, `content_addressed_log` /
  `content_addressed_garbage_collection_log` tables (or dumps), profile artifacts, harness output
  — under one named directory, path written into the results file. Do NOT tear the environment
  down; T9 samples it.
- [ ] **Step 4: the results file.** Battery table; the residual gate row walked (all 7 items);
  the Step-3c/3d table; the specimen path; the post-B residual list (R4 registry, the head-CAS
  north star, the `ApplyPending` debug-only evaluation, the 10b sharding-arm debt, whatever items
  1/3 recorded as UNRECOVERABLE); the verdict line `STAGE B: PASS` or `FAIL`. T8 issues the
  technical verdict; the ledger stays short of COMPLETE until T9.
- [ ] **Step 5: Commit** `ca: stage B — gate battery results + verdict`.

---

### Task T9: destructive-baseline performance research (old Task 12) {#t9}

**Files:**
- Create: `docs/superpowers/reports/2026-08-XX-gc-destructive-baseline-perf.md`
- Modify: `docs/superpowers/cas/BACKLOG.md` (one entry per ranked opportunity)

**Interfaces:**
- Consumes: T8's preserved specimen (never a re-run — a second run is a different specimen).
- Produces: the report that justifies (or refutes) next-round `MultiDelete`/concurrency work; the
  Stage-B closeout.

**Required report shape (restated in full):**
- Scope + evidence rule up front: which specimen, which artifacts, what the numbers can and
  cannot answer. It links the predecessor
  (`docs/superpowers/reports/2026-07-29-gc-perf-audit-soak.md`) and says which of its ranked
  opportunities the destructive baseline confirms, refutes, or leaves untouched.
- Phase decomposition of the destructive round: per-phase wall time and S3 operations by verb,
  covering every T8 Step-3c inventory line, plus time and ROUNDS to fixpoint.
- Un-timed spans NAMED, never silently estimated; a load-bearing un-timed span says what
  instrumentation would answer it. An estimate presented as a measurement is the one failure mode
  this section exists to prevent.
- Before/after only where honest: probe-A removal HAS one (LIST count and round time pre/post-T5)
  and gets it; destruction does NOT (it never ran before T6) — say so instead of comparing
  against a suppressed round, which measured a different workload.
- Ranked opportunities for the NEXT round, each with the motivating measurement and a
  falsification condition — specifically whether `MultiDelete` and delete concurrency are worth
  it, which phase they touch, and the measured ceiling.
- Evidence index: every figure traceable to an artifact path + query/command.

- [ ] **Step 1:** sample the preserved specimen only.
- [ ] **Step 2:** write the report to the shape above.
- [ ] **Step 3:** re-read every figure from the artifacts AT WRITE TIME, immediately before
  committing; any figure that cannot be re-derived from the evidence index is removed, not
  softened.
- [ ] **Step 4:** BACKLOG entries per ranked opportunity. **No optimization lands in this task.**
- [ ] **Step 5: Commit** `ca: reports — GC destructive-baseline performance research`. Update the
  ledger: **Stage B COMPLETE** (T9 is the mandatory closeout; the T8 verdict changes only if T9
  found a wrong measurement, recorded in both documents).

---

### Task F1 (follow-up, outside the Stage-B verdict): mechanical split {#f1}

**Files:** `.../Gc/CasGc.cpp`, `.../Pool/CasRefLedger.cpp` (each ~4000 lines), their headers, and
new sibling TUs along the existing semantic seams.

- [ ] **Step 1:** capture request-count/order equivalence goldens BEFORE moving any code (the
  recording-backend op journals of representative operations, written to test fixtures).
- [ ] **Step 2:** split each file along its existing seams (GC: phases/fold/janitor; ledger:
  writer lanes/recovery/publication). No lifecycle or runtime boundary redesign — this is
  mechanical.
- [ ] **Step 3:** rerun the UNCHANGED goldens + full CA gate; zero behavioral delta.
- [ ] **Step 4: Commit** per file: `ca: split CasGc.cpp along phase seams (mechanical)` /
  `ca: split CasRefLedger.cpp along lane seams (mechanical)`.

### Task F2 (follow-up, before upstreaming): sanitation sweep {#f2}

- [ ] **Step 1:** measure the inventory: `git grep -inE "task [0-9]|review [a-z0-9]|finding
  [0-9]|BACKLOG|superpowers" -- 'src/'` (baseline expectation: ~76 comment references + operator-
  visible strings; re-derive). Include the audit-named survivors not already fixed by T1c/T4/T8.
- [ ] **Step 2:** for each: keep the REASON verbatim, delete the citation; operator-visible
  strings get priority.
- [ ] **Step 3:** remeasure → zero; full CA gate; **Commit**
  `ca: strip branch-local references from production code`.

---

## Provenance (non-normative) {#provenance}

Requirement bodies in this plan originate from:
`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` (Tasks 6, 6b, 7, 7a, 7b, 8,
10, 11, 12, 13, 14), corrected against
`docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` and the design spec
`docs/superpowers/specs/2026-08-02-cas-stage-b-remaining-design.md`. None of these documents needs
to be opened to execute this plan.
