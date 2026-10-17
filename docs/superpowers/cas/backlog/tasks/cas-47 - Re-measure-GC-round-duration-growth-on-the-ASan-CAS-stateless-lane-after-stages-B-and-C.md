---
id: CAS-47
title: >-
  Re-measure GC round duration growth on the ASan CAS stateless lane after
  stages B and C
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'area:ci'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:issue'
milestone: m-1
dependencies:
  - CAS-29.1
  - CAS-29.10
references:
  - CA/Gc/CasGc.cpp
  - CA/Pool/CasPool.cpp
  - 'https://github.com/Altinity/ClickHouse/pull/2300'
priority: low
type: research
ordinal: 57000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
PR #2300 CI (2026-09-02, `amd_asan_ubsan cas s3 storage 1/2`): round finish times grew 1.3 to 12 minutes over rounds
117-128 (round 128: candidates=7309, deleted=4591, manifests_deleted=8776); by the end of a two-hour run every round
exceeded ten minutes. The shutdown hang it fed is bounded by `Pool::beginTeardown` (`CA/Pool/CasPool.cpp:1087`,
`c80a00aed71`/`9d7e3931c52`); the growth itself is unchecked. Re-run the same load after B1 and the stage C deadline and
confirm rounds stay flat.

Provenance: BACKLOG/gc.md#gc-round-duration-superlinear-growth (from the deleted random/pr2300-ci-triage-20260902.md item 1); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A fresh trace of the same lane shows round duration per round number, before and after stages B and C
- [ ] #2 Any remaining superlinear term is attributed to a phase and filed, or the item closes
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
First recorded: 2026-09-26 (e35929424c6, by 'gc-round-duration-superlinear-growth')
<!-- SECTION:NOTES:END -->
