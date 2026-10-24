---
id: CAS-136
title: >-
  Stamp `GcFoldBegin` and `GcFoldEnd` with the round being folded, not the
  previous one
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
priority: low
type: bug
ordinal: 178000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Both fold events set `e.round = state.round` (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:639`, `:676`),
while every other event of the round uses `new_round = state.round + 1` (`:450`, e.g. `:514`, `:747`).
A `cas_log` join of fold events to their round's `cas_gc_log` rows is therefore off by one. One-line fix on both events.

Provenance: BACKLOG/gc.md#gc-outcome-budget-skews-round-report-counters (2031-triage CAS-101) refinement 2. Refinement 1 (report counters tallied from budget-capped outcome logs, still open at CasGc.cpp:905-914) is carried by CAS-29.11. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest runs two rounds and sees `GcFoldBegin`, `GcFoldEnd` and the round's `Finish` row carry the same round number
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-101')
<!-- SECTION:NOTES:END -->
