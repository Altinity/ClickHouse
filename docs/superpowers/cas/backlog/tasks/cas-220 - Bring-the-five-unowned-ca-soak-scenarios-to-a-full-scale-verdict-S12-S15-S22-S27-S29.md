---
id: CAS-220
title: >-
  Bring the five unowned ca-soak scenarios to a full-scale verdict (S12, S15,
  S22, S27, S29)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/RUN_HISTORY.md
  - utils/ca-soak/scenarios/cards/s15_s18_shards_lifecycle.py
  - utils/ca-soak/scenarios/cards/s28_s33_corner.py
priority: medium
type: task
ordinal: 278000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`utils/ca-soak/scenarios/RUN_HISTORY.md` has `full`-scale rows for 20 scenarios. Unresolved and not owned by another task:
S15 is inconclusive on a card bug (`:596`, `Code: 41 Cannot read DateTime: unexpected number of decimal`); S29 is inconclusive (`:347`, `:599`);
S12, S22 and S27 are green at `ci` scale but have never run at `full`.
Owned elsewhere: S03 (`CAS-152`), S05 (CAS-99), S23 (CAS-97), S01 (CAS-100), S07 (CAS-104), S10 (CAS-98), S11 (CAS-106).

Provenance: BACKLOG/testing-and-ci.md [ci-full-scale-sweep]; verified 2026-09-26 against 8b87aa15d21. The 'RSS-attribution / manifest-cap measurement doc' sub-point is carried by CAS-100 and CAS-107.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every subtask is done and each of the five scenarios has a `full`-scale row in `RUN_HISTORY.md` with pass or fail
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
First recorded: 2026-09-26 (5245fbc7a76, by 'ci-full-scale-sweep')
<!-- SECTION:NOTES:END -->
