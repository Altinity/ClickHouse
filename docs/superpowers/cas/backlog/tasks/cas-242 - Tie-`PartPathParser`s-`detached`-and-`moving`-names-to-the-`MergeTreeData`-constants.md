---
id: CAS-242
title: >-
  Tie `PartPathParser`'s `detached` and `moving` names to the `MergeTreeData`
  constants
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:read-path'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h
  - src/Storages/MergeTree/MergeTreeData.h
priority: low
type: chore
ordinal: 307000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CA/Parts/PartPathParser.h:25,41` hardcode `kDetachedDirName = "detached"` and `kMovingDirName = "moving"` instead of using
`MergeTreeData::DETACHED_DIR_NAME` / `MOVING_DIR_NAME` (`src/Storages/MergeTree/MergeTreeData.h:227-228`). If upstream ever renames
either directory, the parser misparses part paths silently. If including `MergeTreeData.h` is too heavy for this header,
a `static_assert` of equality in a `.cpp` that already includes both is enough.

Provenance: BACKLOG/docs-and-cleanup.md#orphan-triage-2026-08-04 [partpathparser-duplicated-path-constants]; verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A build fails if the parser's names and the `MergeTreeData` constants differ
- [ ] #2 The existing part-path parser gtests pass unchanged
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
First recorded: 2026-08-04 (f08734d17df, by 'partpathparser-duplicated-path-constants')
<!-- SECTION:NOTES:END -->
