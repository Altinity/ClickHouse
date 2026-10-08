---
id: DRAFT-10
title: Point-updatable in-degree so a small-delta round costs O(delta) (Lever B)
status: Draft
assignee: []
created_date: '2026-07-06'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:speculative'
  - 'needs:spec'
  - 'origin:review'
dependencies: []
references:
  - CA/Gc/CasBlobInDegree.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Measured cost of a non-idle small-delta round: 87 ms at 400 parts, 93 s at 10k tables, 398 s at 100k parts.
A point-updatable in-degree structure would make it O(delta). Spec B1 already removes the per-round ref-prefix LIST this
item also claimed; what remains is the O(universe) reduce and run rewrite, overlapping `gc-snapshot-log-structured-runs`.
Needs a new format version (decision-4). Decide whether to pursue it or fold it into the log-structured runs design.

Provenance: BACKLOG/gc.md [Lever B].
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner decides pursue, merge into gc-snapshot-log-structured-runs, or close
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
First recorded: 2026-07-06 (436714d80f0, by 'Lever B')
<!-- SECTION:NOTES:END -->
