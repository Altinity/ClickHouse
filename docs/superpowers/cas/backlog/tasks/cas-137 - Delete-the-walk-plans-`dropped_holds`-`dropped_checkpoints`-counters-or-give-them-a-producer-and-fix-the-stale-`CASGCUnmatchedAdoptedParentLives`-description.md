---
id: CAS-137
title: >-
  Delete the walk plan's `dropped_holds`/`dropped_checkpoints` counters or give
  them a producer, and fix the stale `CASGCUnmatchedAdoptedParentLives`
  description
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:gc'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h
  - src/Common/ProfileEvents.cpp
priority: low
type: chore
ordinal: 179000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`buildRefWalkPlan` counts `dropped_holds` and `dropped_checkpoints` when `RefScanSummary::holds` or
`::checkpoint_observations` name a life the catalog cut lacks (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:242-260`).
Nothing in production ever fills those two maps (`Gc/CasGc.h:239-240`), so both counters (`CasGc.h:322-323,338-339`) are
always zero and nothing reads them. A lost hold travels in the adopted parent rows and is counted as `dropped_parent_rows`.
The `CASGCUnmatchedAdoptedParentLives` description (`src/Common/ProfileEvents.cpp:927`) says each drop "is logged with its exact physical
life id". `4d40d453347` removed that warning, and the code comment says "counted, not narrated" (`CasGc.cpp:216-225`).

Provenance: BACKLOG/gc.md#refplan-dead-drop-counters (2031-triage CAS-096). `4d40d453347` is on cas-gc-rebuild only; on altinity/antalya-26.6 check whether the warning is still there before editing the description. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The two maps and counters are removed, or a producer fills them and the REBUILD report row shows them, with a test that moves them
- [ ] #2 The ProfileEvent description matches the code on both branches
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-096')
<!-- SECTION:NOTES:END -->
