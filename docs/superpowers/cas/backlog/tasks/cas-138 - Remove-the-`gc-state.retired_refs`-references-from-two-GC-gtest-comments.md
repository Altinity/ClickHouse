---
id: CAS-138
title: Remove the `gc/state.retired_refs` references from two GC gtest comments
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:testing'
  - 'area:gc'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - src/Disks/tests/gtest_cas_gc_ack_floor.cpp
  - src/Disks/tests/gtest_cas_gc_round_defer.cpp
priority: low
type: chore
ordinal: 180000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`retired_refs` was deleted with the `RetiredSet` family in `37813b4ea75` (retired-in-snapshot T7). Two test comments still
describe lookups through it: `src/Disks/tests/gtest_cas_gc_ack_floor.cpp:88` and `src/Disks/tests/gtest_cas_gc_round_defer.cpp:494`.

Provenance: residue found while verifying BACKLOG/gc.md [retired-refs-map-staleness] (dropped as obsolete). Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `git grep retired_refs -- src` returns only the historical note in `CasFsck.cpp:827` or nothing
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
First recorded: 2026-08-04 (f08734d17df, by 'retired-refs-map-staleness')
<!-- SECTION:NOTES:END -->
