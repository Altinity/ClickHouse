---
id: DRAFT-11
title: >-
  Trigger a fold from per-shard journal pressure instead of the changed-shard
  count
status: Draft
assignee: []
created_date: '2026-07-06'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:speculative'
  - 'needs:measurement'
  - 'origin:soak'
dependencies:
  - CAS-29.10
references:
  - CA/Pool/CasPool.h
  - CA/Gc/CasGc.cpp
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#c3-pacing
priority: low
type: research
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`gc_fold_threshold` (default 1) and `gc_fold_max_defer_rounds` (default 8) key on changed-shard count (`CA/Pool/CasPool.h`).
The idea keys the fold on per-shard pressure and needs a soak-swept knee. Spec C3 adds deadline-driven pacing, which is
related but distinct; re-evaluate after stage C whether any cadence problem remains.

Provenance: BACKLOG/gc.md [ADAPTIVE-GC-CADENCE].
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 After stage C, a soak shows whether fold cadence is still a measurable cost; if not, close
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
First recorded: 2026-07-06 (199562d12a1, by 'ADAPTIVE-GC-CADENCE')
<!-- SECTION:NOTES:END -->
