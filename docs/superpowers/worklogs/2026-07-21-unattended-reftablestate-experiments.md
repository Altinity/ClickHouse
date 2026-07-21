# Unattended worklog — `RefTableState` closed class + experiment matrix

Round start: 2026-07-21. Spec: `docs/superpowers/specs/2026-07-21-reftablestate-closed-class-experiments-design.md`
(commit `d7bd8a60075`). Branch: `cas-gc-rebuild` (shared with a parallel session — foreign commits
are expected and fine; never rebase/amend; verify `HEAD` after each commit).

Mission (user): writing-plans → subagent-driven development for the whole matrix; watchdog cron
every 20 min; new bugs → systematic debugging + triage (quick fixes fixed inline, design-heavy
items to `docs/superpowers/cas/BACKLOG.md`); at the end — summary with the full comparison table,
findings, remarks; then stop the watchdog.

Success criterion (final 20-min soak): hottest CAS-attributed CPU stack family = blob hashing;
every other CAS family ≤ 1/3 of its samples; zero correctness-invariant hits.

## Timeline {#timeline}

- Spec approved and committed (`d7bd8a60075`). Grounding reads done: `CasRefProtocol.{h,cpp}`,
  `CasRefCowMap.h`, `benchmark_cas_ref_protocol.cpp`, call-site survey (heavy consumers:
  `CasRefLedger.cpp` 13 mentions, `gtest_cas_ref_statemachine.cpp` 83, `gtest_cas_ref_writer.cpp` 12).
- Key pre-plan finding: existing `BM_Admits` measures a *promote* op, which never calls
  `manifestAlreadyOwned` — so it reports O(1) while the production hotspot (add-precommit path)
  is O(N). The suite gets a dedicated add-precommit benchmark.
- Watchdog cron armed (every 20 min). Writing the implementation plan next.
