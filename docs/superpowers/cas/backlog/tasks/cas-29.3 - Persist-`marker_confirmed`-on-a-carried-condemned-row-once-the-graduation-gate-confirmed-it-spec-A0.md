---
id: CAS-29.3
title: >-
  Persist `marker_confirmed` on a carried condemned row once the graduation gate
  confirmed it (spec A0)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-30 13:23'
labels:
  - 'area:gc'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-30
references:
  - CA/Gc/CasBlobInDegree.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#a0-persist-marker-confirmed
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f3
parent_task_id: CAS-29
priority: high
type: bug
ordinal: 109000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`settleEntry` carries a floor-passed condemned entry unchanged when `confirm_condemned_marker` succeeded but the graduation
budget is exhausted (`CA/Gc/CasBlobInDegree.cpp:493-498`, `rmr.still_retired.push_back(e)`), so the run keeps
`marker_confirmed = false` and only the in-process memo remembers the confirmation. After a restart or a leadership change
every carried row pays one synchronous `loadMeta` GET again: 200k GETs and 4 hours in round 1379 on otel.demo, 7,100 of
8,800 s of that `fold_reduce` (audit F3, conclusion 5). The `marker_confirmed` bit already exists in the run format
(`:193`, `:217`), so this is no format change. Fix: carry a copy with `marker_confirmed = true` once the gate confirmed it.
The audit orders it first among the stage A tasks. Stage C's deadline carries rows the same way, so A0 stays necessary after C.

Provenance: spec A0 / audit F3; the restart case is stated in BACKLOG/gc.md#gc-reduce-confirm-marker-read-ahead. If u03 creates an A0 task from #otel-demo-s3-budget-audit-2026-09-25, merge it into this one. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (identical carry line).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest: a round that confirms a marker and carries the row past the budget writes the row with `marker_confirmed = true`
- [ ] #2 After a `Gc` re-creation with N carried confirmed rows, the next round issues no meta GET for them
- [ ] #3 Graduation decisions are unchanged against the existing reduce oracle tests
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
u03-gc-c merged audit F3 here (no separate A0 task was created).

First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `CasBlobInDegree` is 2026-07-01 (c7d94e51817); the finding itself has no record before this migration, so the creation date stays 2026-09-26.

2026-09-30: implemented on the branch (4e177f6adb4, a3e10126c45), then removed from it (c4fdec53a23). Branch `cas/gc-request-cuts` (worktree `gc-request-cuts`, base antalya-26.6 @e2dd0f5d343, HEAD 1edf28f2a9a on 2026-09-30; not pushed, no PR).
- The safety argument of spec `2026-09-29-cas-gc-request-cuts-design.md` §2 needs 'a marker leaves Condemned only after its condemned token is deleted or displaced'. CAS-30 breaks that premise (`docs/superpowers/reports/2026-09-30-cas-30-stale-meta-delete-adjudication.md`), so a confirmation must not outlive the process that made it until CAS-30 is fixed. Hence the dependency.
- No stand run reached the change: it needs a restart or a leader change while rows are carried (`docs/superpowers/reports/2026-09-30-cas-gc-request-cuts/01-before-after-10s-rounds.md`, T3). Its value is unmeasured; do not re-add it without a scenario that shows one.
- The model `docs/superpowers/models/CaGcCondemnMarkerGate.tla` was extended for it (c9c4fa54bf0, e44bbef587e) but deletes body and meta atomically, so it cannot see the CAS-30 schedule.
<!-- SECTION:NOTES:END -->
