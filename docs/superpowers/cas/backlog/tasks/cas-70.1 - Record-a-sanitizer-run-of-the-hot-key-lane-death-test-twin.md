---
id: CAS-70.1
title: Record a sanitizer run of the hot-key lane death-test twin
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:testing'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:review'
dependencies: []
references:
  - src/Disks/tests/gtest_cas_ref_catalog.cpp
  - ci/defs/job_configs.py
documentation:
  - docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md
parent_task_id: CAS-70
priority: low
type: task
ordinal: 85000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CASRefCatalogDeathTest.AStaleHintCasAdmitEntryAdmitsWhenTheStoreHasRoomAborts` (`src/Disks/tests/gtest_cas_ref_catalog.cpp:2516`, both branches) is recorded as never executed under a debug or sanitizer build (phase A plan, Task 6 step 2).
Altinity CI runs `Unit tests (asan_ubsan)`, `(tsan)` and `(msan)` on pull requests, so it has likely run since the lane landed; no report proving it was found.
Either cite a CI unit-test report where it passed, or run `CASRefCatalog*:CASHotKeys.*` once in a local ASan build.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (first outstanding bullet). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A link to a CI unit-test report, or a local ASan log, shows the death test and the `CASHotKeys.*` suite passing
- [ ] #2 The phase A plan's status table is updated to reference that evidence
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `CASRefCatalog` is 2026-08-03 (0f7b755bb16); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
