---
id: CAS-70.2
title: >-
  Count the `ref_catalog` loop's own conflict pauses in a `ProfileEvents`
  counter
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.cpp
  - src/Common/ProfileEvents.cpp
parent_task_id: CAS-70
priority: medium
type: enhancement
ordinal: 86000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`casUpdateImpl` pauses on a lost race with `op.pause(...)` directly (`R/Pool/CasRefCatalog.cpp:190-191`); `CasOperation::pause` (`R/Backend/CasRequests.h:255`) counts nothing.
`CASRequestConflictPause` is incremented only by `recordConflictPause` inside the engine's `readModifyWrite` (`CasRequests.cpp:65-68`), so the pacing of the hottest key in the pool is invisible.
The acceptance measurement needs this counter.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (deferred item 1). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A counter increments once per pause in the catalog update loop, split by clean conflict versus settled fault
- [ ] #2 A gtest with a forced catalog conflict asserts the counter delta
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `CasRefCatalog` is 2026-07-30 (94bd8deaadd); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
