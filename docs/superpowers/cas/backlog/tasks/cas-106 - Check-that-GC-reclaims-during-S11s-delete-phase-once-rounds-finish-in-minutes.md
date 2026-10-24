---
id: CAS-106
title: Check that GC reclaims during S11's delete phase once rounds finish in minutes
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:35'
labels:
  - 'area:gc'
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:soak'
milestone: m-1
dependencies:
  - CAS-29
references:
  - utils/ca-soak/scenarios/cards/s09_s11_mutations.py
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
priority: low
type: task
ordinal: 144000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
In S11 at scale, GC did not reclaim while the delete phase ran; pool bytes grew until after it ended. Same O(N) GC-lag family as the rounds-in-minutes spec, but not shown to be fixed by it.

Provenance: BACKLOG/performance.md#scale-findings [S11 capacity]. Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S11 at full scale on a build with spec stages B and C shows reclaimed bytes growing during the delete phase
- [ ] #2 If it does not, the stage that blocks reclaim is named and a GC task is filed
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'S11 capacity')
<!-- SECTION:NOTES:END -->
