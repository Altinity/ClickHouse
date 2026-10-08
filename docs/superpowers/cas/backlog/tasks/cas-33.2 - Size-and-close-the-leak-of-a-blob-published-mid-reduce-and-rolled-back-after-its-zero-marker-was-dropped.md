---
id: CAS-33.2
title: >-
  Size and close the leak of a blob published mid-reduce and rolled back after
  its zero marker was dropped
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:on-s3-format'
  - 'confidence:plausible'
  - 'needs:measurement'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasBlobInDegree.cpp
documentation:
  - docs/superpowers/worklogs/2026-09-03-cas-gc-head-read-ahead-consult.md
parent_task_id: CAS-33
priority: low
type: bug
ordinal: 42000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A blob absent at its condemn `HEAD` but present when the merge closes it is not condemned that round, and its zero
marker is per-generation and dropped on carry (`CA/Gc/CasBlobInDegree.cpp:106`). For ordinary garbage this is correct:
present at the merge implies a publisher and an edge. The residue is a publication that lands mid-phase and then rolls
back: no edge, never revisited until a rebuild. The HEAD read-ahead widens the window but did not open the class.
Not sized. The fix is a carried marker (run format change, decision-4) or a sweep, not a change in the fold.

Provenance: BACKLOG/gc.md#gc-reduce-zero-marker-dropped-on-carry; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A measurement on a soak or a test harness gives how often the shape occurs per round
- [ ] #2 Either a fix lands with a test that shows the rolled-back body reclaimed, or the owner defers it to the R4 reclaimer
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
First recorded: 2026-09-04 (cd518660419, by 'gc-reduce-zero-marker-dropped-on-carry')
<!-- SECTION:NOTES:END -->
