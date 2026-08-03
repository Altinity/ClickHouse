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
- T3 (Task 7 closure): **COMPLETE** + the fsck-contract slice. Integrated as 8b7926bd66f /
  4e19cfe08e7 / 70ca84c079c / 719c4d0ed87. Arc: two-phase heal (fence correct, test premise was
  stale — lane red since 224aacd8eb9 discovered+reconciled); arm-(b) retirement fence now tested;
  affirmative operator message (user directive); fsck: dead-life residue = janitor_pending soft
  class, observe-then-cut, writer/parser grammar unified (consult-backed, review REJECT→fix
  round→APPROVE; genuine red-first on the ambiguity abort; 05020 golden 17→20 run for real).
  Janitor suppression verified in code (suppress_deletes gates every deleteExact). Reviews:
  draft APPROVE / closure APPROVE-WITH-NONBLOCKING / fsck+fix APPROVE. Post-integration
  verification on MAIN green.
- T4 (Task 8 closure): **COMPLETE** (b17e4d97485 + 4329577bf37; review APPROVE-WITH-NONBLOCKING
  in t4-review.md — reviewer independently reran the full ASan per-suite gate: 296/296 suites,
  1990/1990, 0 sanitizer reports). Q-1 executed as decided; C-1 out of production surface;
  settlement ordering pinned (already correct). T4 debts placed in T8 residual row: TEST-1
  (real-round applied-byte-stability needs a seam). Gate-tooling defects found by the review
  fixed by controller (runner: parameterized-name log path; SUITE_TIMEOUT).
- T5 (probe A deletion): **COMPLETE** (draft pick 5b775616c36 + closure 857b5af19f2 + review
  follow-ups 6c9dd39c1e0). Exact −3/0 delta proven by direct pre/post measurement; enumeration
  anchor real (1 stream-LIST/round × 5 rounds, janitor page counted separately); phase numbering
  consistent across 4 surfaces; review APPROVE-WITH-NONBLOCKING (t5-review.md). C1 fixed: the
  soak DETECTOR_METRICS phase key was dead (fold_ref_list vs emitted fold_ref_group) — 290/290
  pytest after fix. USER DIRECTIVE added to plan: post-final-tidy → parallel codex review + 20m
  plain chaos soak.
- T6a: **COMPLETE** — verdict BENIGN-TRANSIENT, structurally closed by `357cf7b963f`; commit
  `477fe702a7a` (laneg/t6a) integrated as `096b3611988`; review APPROVE-WITH-NONBLOCKING with the
  corrected enumeration + NEW T6→T8 carry (post-flip healthy rounds also show ZERO
  no-usable-checkpoint anomalies); three dead frontier-walk arms placed with T6 Step 1.
- T6 (destruction enablement): **INTEGRATED, review pending** — full arc from `laneg/t6-finish`
  (branch point 73755caa6e5, 6 commits) cherry-picked clean into cas-gc-rebuild as
  `58fd482a800..b7da56d6e25` (flip commit = `ea5506d4d76`); finisher's own gates were green
  (ASan GATE_EXIT=0, integration 19/19 after the janitor-race drains-not-pending fix).
  Integration ahead of review verdict was an explicit USER DIRECTIVE (2026-08-03); heavyweight
  review re-dispatched on Fable (t6-review-fable → t6-review.md) + codex T5+T6 review in flight —
  findings, if any, land as follow-up commits. Soak cards s44/s45 (`laneg/soak-cards`)
  integrated as `c57b2575356`. REVIEW VERDICTS (all three in): opus-lane APPROVE
  (`t6-review-opus.md`, authoritative copy; `t6-review.md` is its earlier identical text) with
  fix round TEST-1/TEST-2; Fable-lane APPROVE (`t6-review-fable.md`), independently converged
  (its T-3 == TEST-1), carries T-1 observation (anomaly/carried-hold gate terms structurally
  redundant on current shapes) to T8/hygiene; codex T5 APPROVE-WITH-FIXES + T6 REJECT on the
  WORK ENVELOPE only (safety confirmed clean) — answered by the new Task T6b (plan amendment
  `e497c4a0e6e`, user-approved minimal fail-close caps). IN FLIGHT: fixround-t6rev on MAIN
  (TEST-1 via option (a) — mutation on exact-key intake, universe-term mutations vacuous by
  construction on the grounded fixture; TEST-2; codex T5-2 cadence 5→32; T5-3 phase-list sync);
  t6b-impl on LANE-G (`laneg/t6b` from `e497c4a0e6e`, 3 slices). Prose batches D44/D45 recorded.
  `[gc-frontier-one-list]` (BACKLOG:136) deferred to a separate focused session AFTER Stage B.
- T6b (work-envelope budgets): **COMPLETE and INTEGRATED** — full `laneg/t6b` arc (8 commits
  through `ae11315badb`) cherry-picked clean into cas-gc-rebuild ending `e8b0d0220b0`. Nine
  per-round budget settings (six from the slices + three C-fixes), all fail-close, mutation-proven,
  full CA gate green both builds (278/296, 0 fail 0 abort). Review APPROVE-WITH-NONBLOCKING
  (t6b-review.md); C1/C4 closed by `0255a67f419`. C2 (`gc_round_manifest_cleanup_budget`) was
  REVERTED after `soak-t6b-report.md` showed it leaks: the ref-log intake cursor that discovers
  each owner-removed manifest commits in the SAME round's CAS as the cap-declined entry, so a
  cap-declined entry is never re-derived by this one-shot pipeline (run-1, cap=5000: 112,518
  skipped, 110,218 permanently unreachable, checkpoint FAIL; run-2, cap disabled: full drain of
  223,714, unreachable=0, PASS). codex T6-2 bullet-4 answered WON'T-CAP with this evidence pair;
  the setting, `GcRoundWorkBudget`'s `max_manifest_cleanup_objects`/`manifestCleanupAvailable`, and
  the C2 gtest were removed entirely; real bounding needs durable retry (moving the edge-consumption
  point past the delete), tracked as `[gc-mf-cleanup-durable-retry]` in BACKLOG. The codex T6 REJECT
  on the remaining caps is ANSWERED with honest
  residuals: T6-1 count axes bounded, retained-BYTE axis reduced (1000→100 bodies) NOT bounded;
  `recoverRefTableDetailedFromAuthority` internal cost = one coarse unit, NOT bounded (correct seam:
  fsck/rebuild need the complete table); capped spared entries lose their audit outcome record
  (one-shot log, settlement itself unconditional — INV_NO_LOSS holds); defaults UNCALIBRATED by
  design — T8 soak calibrates, incl. the `gc_round_sweep_namespace_budget=20` throughput watch item.
  Fix round (TEST-1/TEST-2 + codex T5-2/T5-3) landed earlier as `7b9a8fc8f2d`+`075b2ed5f01`.
- T7 (model lane): **COMPLETE**. Lane B: aab2a21d699 + e1599389f93 (10c runners pinned/recorded;
  10f disclosure; shared temporal-smoke regex hardened). Lane A: integrated as afed91f65d2 (A1:
  listedTok RETIRED, 4 configs removed) + e05a62a7b17 (A2: ninth battery — 42/44 immediate, live
  GREEN@extended-bound reproducing historical counts, stage5_lazytrim UNPROVEN-BY-TIMEOUT).
  Review APPROVE-WITH-NONBLOCKING (t7-laneA-review.md; retirement argument verified stronger than
  stated; no coverage dropped). TWO NAMED MODEL DEBTS carried to T8 residual row: Phase-4
  sharding-arm KNOWN (UnchangedCompositeVars) + stage5_lazytrim UNPROVEN-BY-TIMEOUT (4h, 233M
  distinct states). MAIN's uncommitted runner copy reconciled (checkout --, equivalence verified
  by the review).
- T8 (Stage B gates): early pieces IN PROGRESS — E1 tidy DONE end-to-end: full AMD-tidy build
  collected 119 unique CAS diagnostics; ALL resolved (fix or NOLINT+reason; 3 real defects found
  incl. a fault-injection-disabling grandparent call and an optional-deref UB risk); verified by
  per-TU re-tidy + per-suite gates release 278/278 / ASan 296/296, zero aborts (draft
  b0f87e8aaf1 + closure c0d2cad0dfd). E2-E4 + residual tests drafted on draft/t8. Battery/soaks
  still blocked on T1–T7. SCHEDULED ADDITIONS (user directives): Cas-prefix suite normalization
  post-T6; ONE final incremental tidy re-run after all C++ tasks; staged 20-min smoke soaks.
- T9 (perf research): BLOCKED on T8. Stage B is COMPLETE only when T9's commit lands; T8 issues
  the technical verdict.
- F1/F2: follow-ups after T9, outside the Stage-B verdict.

Historical-unrecoverable items (Task-1 minors verbatim list; NITs C–F) are recorded once in the
midpoint audit `{#historical-unrecoverable}`; T8 performs no archaeology.
