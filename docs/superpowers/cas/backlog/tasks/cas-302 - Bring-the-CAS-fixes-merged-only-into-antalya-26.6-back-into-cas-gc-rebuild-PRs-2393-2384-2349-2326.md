---
id: CAS-302
title: >-
  Bring the CAS fixes merged only into antalya-26.6 back into cas-gc-rebuild
  (PRs #2393, #2384, #2349, #2326)
status: To Do
assignee: []
created_date: '2026-09-26 12:37'
labels:
  - 'area:ci'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2393'
  - 'https://github.com/Altinity/ClickHouse/pull/2384'
  - 'https://github.com/Altinity/ClickHouse/pull/2349'
  - 'https://github.com/Altinity/ClickHouse/pull/2326'
priority: high
type: chore
ordinal: 381000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Several CAS pull requests were merged into `altinity/antalya-26.6` and never came back to `cas-gc-rebuild`: #2393 (do not probe listing keys as JSON), #2384 (cap the RustFS blocking-thread pool on the CAS-S3 lanes), #2349, and #2326 (GC scheduler stop/join race, tracked as CAS-166). The two branches drift silently; the grooming of 2026-09-26 found several backlog claims that were true on one branch only. Bring the merged fixes over (merge or cherry-pick), then add a periodic check that every CAS-labelled PR merged into antalya-26.6 is an ancestor of cas-gc-rebuild.

Provenance: GitHub reconciliation 2026-09-26 (unit u17); no backlog task tracked the branch sync.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every CAS-labelled PR merged into antalya-26.6 is an ancestor of cas-gc-rebuild (list produced by `gh pr list --label CAS --state merged` and `git merge-base --is-ancestor`)
- [ ] #2 CAS-166 is closed by the same sync
- [ ] #3 A documented one-line check exists that reports the next drift
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
