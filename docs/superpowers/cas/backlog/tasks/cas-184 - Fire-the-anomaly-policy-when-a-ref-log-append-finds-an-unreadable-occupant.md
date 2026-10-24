---
id: CAS-184
title: Fire the anomaly policy when a ref-log append finds an unreadable occupant
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - R/Pool/CasRefLedger.cpp
priority: medium
type: bug
ordinal: 241000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The "occupant unreadable" arm sets the lane `Faulted` and counts `CASRefAppendOccupantUnreadable` (`R/Pool/CasRefLedger.cpp:3814-3818`) but never calls `on_impossible_interference`.
The sibling foreign-occupant arm does (`:3868-3874`), which triggers the automatic remount. The unreadable arm alone leaves a `Faulted` lane that needs an operator.

Provenance: BACKLOG/ref-protocol.md#lane-residuals-2031-cas-017 residual 2. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792 (counter at :3777 there).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An unreadable occupant at the append's id triggers `on_impossible_interference` exactly like a foreign occupant (gtest asserts the callback and the remount)
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
First recorded: 2026-08-21 (7a9b839cd49, by 'lane-residuals-2031-cas-017')

2031-triage CAS-017: residual 2 (a `Faulted` lane with no automatic remount).
<!-- SECTION:NOTES:END -->
