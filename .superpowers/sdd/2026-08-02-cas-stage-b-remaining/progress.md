# SDD ledger — plan: docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md

Published 2026-08-03 in one atomic publication commit (plan + midpoint audit + supersession note
in the old plan + old-ledger close + this ledger + `utils/cas-gate/` tooling). Execution inputs
are exactly: the plan, the design spec
(`docs/superpowers/specs/2026-08-02-cas-stage-b-remaining-design.md`), the midpoint audit
(evidence/provenance only), and this ledger. The old plan and the session handoff are historical
provenance and are not loaded during execution.

Execution mode: two worktrees per the plan's `{#two-worktrees}` map. MAIN
(`/home/mfilimonov/workspace/ClickHouse/master`, `cas-gc-rebuild`) is the single production
writer and sole committer to the branch; LANE-G (`/home/mfilimonov/workspace/ClickHouse/lane-g`)
works on `laneg/<task>` temp branches handed back for integration, preserving its ~224 untracked
investigation artifacts. Full CA gates/builds serialized across worktrees via
`flock tmp/unit_tests.lock`; at most one praktika-or-soak run on the box. SDD mandatory for
T1–T7: implementor → specification review → code-quality review → ordinary commit. Targeted tests
per commit; full CA gate at lane closures.

## Seeded baseline states (tree-verified at `ce312f547c3`, per the midpoint audit) {#baseline}

- Old-plan Tasks 0–5b, 9, 10d/e/f/g: COMPLETE and verified. Do not re-dispatch.
- Task 6: namespace-file half LANDED (APIs life-keyed; `namespaceFilesReadable` deleted in
  `827bc0a9189`; delayed-writer life capture fixed + tested; 2 of 3 required tests committed;
  zero-catalog pinned except the `list` arm). Ref side NOT STARTED (no held-life ref reads, no
  ref contract tests; `resolveLifeOrSentinel`/`stageATransition` present, test-only).
- Task 6b: rename LANDED at `9d92c84ee37` (one retry unit, not split). All four
  ordering/poison/backoff tests MISSING.
- Task 7: implementation PRESENT (`224aacd8eb9`, six focused tests; the three old residues fixed
  in code). Closure evidence OPEN; retirement-fence arm (b) UNTESTED.
- Task 8: model gate COMPLETE (`d34aa06d89f`); production slices PRESENT (`c3cc24c8152`,
  `8f14bc119fe`, 11 direct tests). Closure OPEN: T-1/T-2/T-3 test findings, C-1 footgun, Q-1
  decision execution, C++ mutation demonstrations, task gates.
- Tasks 7a, 7b: NOT IMPLEMENTED (probe A fully live; `kDefault = StageA_Suppressed`;
  `PENDING` double-count already fixed in `8e9b06c2a81`).
- Task 10: 10a OPEN (no RESULTS file exists); 10b 8/9 (ninth = unstaged
  `run_gc_partmanifest.sh` rewrite); 10c runners present, results artifact absent, three runners
  unpinned; 10f `UniverseAuthoritative` disclosure owed.
- Model-tool preflight owed in every model-lane session: `tmp/tla2tools.jar` in this worktree is
  the WRONG jar (1.7.4); the pin is SHA-256
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.

## Task states {#tasks}

- T0 (bootstrap): **COMPLETE** (2026-08-03). Prose batch D30–D38 + R1-note citation fixes in
  `5fbc13c629c`. Gate tooling verified AND hardened post-publication in `5c42d88cddd` (TEST_P
  suites claimed — 276 suites/0 unclaimed vs the 273 that silently missed
  `CasBackendContract`/`CasGcCompletedRemovalFenceRace`; runner exits nonzero on FAIL/ABORT;
  explicit compile-guarded death-test list; shared `git-common-dir` gate lock; plan branch-switch
  constraint scoped to MAIN; `kDefault`-comment instruction aligned with comment policy).
  MAIN worktree preflight: pinned TLC jar installed at `tmp/tla2tools.jar`
  (digest `cc4803dc…e516b3` verified; source: lane-g's `tmp/tla2tools-official.jar`). Preflight is
  per-worktree state — repeat in any new model-lane session.
- T1 (Task 6 remainder): IN PROGRESS on MAIN.
  - T1a: **COMPLETE** (2026-08-03). Classification verdict: ALL TEN `CasRefCatalog::read` sites
    KEEP (no class-4/5) — zero production edits; slice = three coverage pins in new
    `gtest_cas_ref_read_contract.cpp` (rebirth-alias via held runtime; zero-catalog/zero-backend
    warm reads; stale `dropNamespace(life1)` refusal at the one held-writer seam, `NETWORK_ERROR`
    retry-later). Stale-ref-writer requirement recorded as satisfied at that seam and vacuous
    elsewhere. Commits: `ed76b256a50` (tests + classification + report), `ec0cfbc1007` (review
    APPROVE-WITH-NONBLOCKING; prose → D39). Sensitivity checks preserved in
    `build/t1a_sensitivity_test{1,2,3}.log`; reviewer re-verification 15/15 + 4/4.
  - T1b: COMPLETE (be3394a1528 list-arm pin; strengthened to exact touched-set form in
    fd7af51a992 after review; review APPROVE-WITH-NONBLOCKING).
  - T1c: COMPLETE (b5c812ba56a seam; 2d508a38b09 migration by codex, controller-committed;
    b7053eeb70c retirement — zero-greps 0/0).
  - LANE GATES (post-hygiene): release 1977/1977/277 exit 0 (hygiene_gate_release_v2.log);
    ASan FULL gate 1982/1982/295 exit 0 (hygiene_gate_v3.log). T1 LANE FULLY CLOSED.
  - Gate-hygiene follow-ups landed: 6c992f3561f (fence-race fixture + LOGICAL_ERROR death-split,
    recurrence #6 of the abort class), a046ad621a1 (3-site heap-use-after-free: dangling pointer
    into the destroyed CasRefCatalog::read temporary). All three defects were pre-existing and
    previously INVISIBLE (prefix-filter gap, no full ASan gate since the tests' birth).
- T2 (Task 6b remainder): **COMPLETE** (404b6ecbe3a + ecc638f8861 + tautology-test removal;
  review APPROVE-WITH-NONBLOCKING in t2-review.md). Vocabulary ruling recorded: plan's "Poisoned"
  = `RefLaneState::NeedsRecovery`; production is recover-then-proceed; test pins
  recover-before-publish + published-snapshot-contains-missing-txn. Positive contract property
  corroborated: `resolveRef` also recovers unconditionally — no seam exposes an un-recovered
  read. Nonblocking T2 debts placed in T8's residual row: F1 (reset assertion holds even if
  reset were no-op), F2 (4000ms cap unpinned from below), F4 (`settleSnapshotPublish`
  uncharacterized).
- T3 (Task 7 closure): NOT STARTED. LANE-G.
- T4 (Task 8 closure): NOT STARTED. Tests LANE-G / production steps MAIN.
- T5 (probe A deletion): NOT STARTED. MAIN after T3 integration.
- T6a: **COMPLETE** — verdict BENIGN-TRANSIENT, structurally closed by `357cf7b963f`; commit
  `477fe702a7a` (laneg/t6a) integrated as `096b3611988`; review APPROVE-WITH-NONBLOCKING with the
  corrected enumeration + NEW T6→T8 carry (post-flip healthy rounds also show ZERO
  no-usable-checkpoint anomalies); three dead frontier-walk arms placed with T6 Step 1.
- T6 (destruction enablement): BLOCKED on T1 + T5 + T6a verdict. MAIN.
- T7 (model lane): IN PROGRESS on LANE-G (laneg/t7). A1 COMMITTED `a19066a7893` — verdict:
  `listedTok` skip premise RETIRED (model/configs reworked). A2 ninth-family battery running
  (nohup; runner rewrite taken from MAIN's preserved uncommitted copy with controller approval).
  Lane B (10c) not started.
- T8 (Stage B gates): early pieces may start anytime; battery/soaks BLOCKED on T1–T7. MAIN.
- T9 (perf research): BLOCKED on T8. Stage B is COMPLETE only when T9's commit lands; T8 issues
  the technical verdict.
- F1/F2: follow-ups after T9, outside the Stage-B verdict.

Historical-unrecoverable items (Task-1 minors verbatim list; NITs C–F) are recorded once in the
midpoint audit `{#historical-unrecoverable}`; T8 performs no archaeology.
