---
id: CAS-93
title: >-
  Scale the soak's GC fixpoint timeout for sanitizer builds and for measured
  round duration
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
references:
  - utils/ca-soak/soak/checker.py
priority: low
type: task
ordinal: 128000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`fixpoint_timeout_s` (`utils/ca-soak/soak/checker.py:119`) assumes one reclaim round per `gc_interval_s` and a fixed
reclaim rate, with a 300 s floor. Under TSan a converging GC can exceed it, and on real AWS 427 s and 328 s rounds broke
the 300 s bound (`history=[3517, 2779]`) while GC was making progress. No sanitizer multiplier exists.

Provenance: BACKLOG/gc.md [gc-checkpoint-timeout-tsan] and the AWS data point in #gc-pending-deletes-fan-out. Verified 2026-09-26 against 59494ebf366 (harness not on antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The bound takes a sanitizer multiplier and the observed round duration from `cas_gc_log`
- [ ] #2 A unit test in `utils/ca-soak/tests` covers the scaled bound
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
First recorded: 2026-08-04 (b4420fe512a, by 'gc-checkpoint-timeout-tsan')
<!-- SECTION:NOTES:END -->
