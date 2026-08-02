# SDD ledger — plan: docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md

Execution mode (2026-08-02): unattended accelerated milestones. One persistent C++ writer in
`/home/mfilimonov/workspace/ClickHouse/master` on `cas-gc-rebuild`; read-only audits and log analysis may run in parallel.
Micro-increments run only an incremental affected target plus the narrow changed tests. Broad CA,
object-storage, integration and soak gates run at milestone boundaries, not after every commit.

Completed foundation: Tasks 0–4d are represented by the Stage-B commit chain through
`d278d130024`; do not re-dispatch them. Historical checklist boxes are not used as execution state.

Task 5: COMPLETE.
- The final residual audit found the lifecycle, model, capacity, hygiene and gate obligations complete.
  The stale `CasRefCatalog` publication prose and missing post-fold unreadable-terminal signal/test were
  the only confirmed residuals; `765c50b7cb93` closes both and its specialist review passed.
- Evidence: focused residual gate 21/21; Task 5 branch gate 1929/1929; post-merge gate at
  `78cf06456d3` 1930/1930; S3 selectors 3/3. Merge-resolution review passed.
- Minor deferred test strengthening: the exact-delete exception branch has no symmetric direct
  regression test. The implementation is symmetric and nonblocking, so this does not reopen Task 5.
- Clean handoff point: Task 6 is not started; resume it in a new session from status commit
  `ce312f547c3` (whose parent is merge `78cf06456d3`).
- The main worktree retains `.superpowers/sdd/task-5-report.md` as an unrelated unstaged file and
  `docs/superpowers/models/run_gc_partmanifest.sh` as the unfinished ninth Task 10b family. Preserve
  the former; finish the latter with its battery and RESULTS record before committing it.

Task 5b: COMPLETE.
- Model checkpoint: `c863cdd7fa60` (`ca: model LIST-independent ref recovery frontier`).
- Production baseline: `357cf7b963f4f8f9b114ba2b8c7eb202c2c2259a`
  (`ca: ref — LIST-independent recovery: exact checkpoint frontier, no hint-derived history`).
- Closing chain: `3747975bbbf`, `8183a1af1800`, `e48b476d90f`, `4ab9b452e660`,
  `60cbec2bd274`, `7ac127b650a`, `613faf8166e`; all closing reviews passed.
- Final gates: full CA 1929/1929, 2 disabled; S3 selectors 3/3; writer model 5/5 and delta-intake
  model 15/15 at `c863cdd7fa60`.
- All four baseline review debts are closed; no Task 5b debt remains open.

Task 7: IMPLEMENTATION PRESENT, CLOSURE EVIDENCE OPEN.
- `224aacd8eb9` contains the catalog-exact decommission implementation and focused tests.
- Do not reimplement it. The red-first record and named `test_content_addressed_drop_pool_member`
  lane evidence still need to be recovered or rerun before task closure.

Task 8: PARTIAL.
- Model Step 1 is complete in `d34aa06d89f`.
- Production slices `c3cc24c8152` and `8f14bc119fe` and their direct tests are present.
- Task-level red-first/full-CA/S3 evidence, review and closure commit remain open.

Task 9: COMPLETE in `ca07cbf87fd`.

Task 10: PARTIAL.
- 10a OPEN; `listedTok` remains the named unaudited residual.
- 10b is 8/9 committed; the local `run_gc_partmanifest.sh` ninth family still needs its battery,
  RESULTS record and commit.
- 10c has all four runners but lacks the required before/after results artifact.
- 10d, 10e, 10f and 10g are COMPLETE; see the authoritative plan's execution table for commits.

Milestone M1: COMPLETE — Task 5 integrated, specialist-reviewed and milestone-gated.
Milestone M2: Task 5b complete; remaining Tasks 6 and 6b need one read/recovery contract review and milestone gate.
Milestone M3: Tasks 7, 7a, 7b, 8; destructive-gate concurrency review and required focused lanes.
Milestone M4: Task 9 complete; Task 10 partial as itemized above.
Milestone M5: Tasks 11–12; full final battery, soaks, results and performance report.
Tasks 13–14 are explicitly post-Stage-B/before-upstreaming follow-ups and are not on the Stage-B
critical path unless the authoritative plan's completion criterion says otherwise.

LEDGER CLOSED (2026-08-03). Execution moved to plan
`docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md` with ledger
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/progress.md`, published atomically with the
midpoint audit `docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` and the supersession
note in the old plan. No further entries here.
