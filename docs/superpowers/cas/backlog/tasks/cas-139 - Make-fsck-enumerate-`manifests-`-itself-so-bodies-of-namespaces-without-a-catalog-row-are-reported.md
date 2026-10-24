---
id: CAS-139
title: >-
  Make fsck enumerate `manifests/` itself so bodies of namespaces without a
  catalog row are reported
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:fsck'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
priority: medium
type: bug
ordinal: 181000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
fsck's manifest-debris pass lists `manifests/<ns>/` only for namespaces returned by `Pool::listNamespaces`
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:1093-1122`). That function derives namespaces
from the catalog alone (`Pool/CasPool.cpp:1875-1903`). A namespace whose catalog row is gone but whose manifest bodies remain
is never listed. Its bodies appear in no fsck class and no byte count, so fsck under-reports exactly the debris GC's
orphan sweep and the janitor are meant to reclaim.

Provenance: BACKLOG/gc.md#gc-observability [fsck oracle gaps]. Sibling fsck blind spot for `gc/gen/` is CAS-34. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792 (CasFsck.cpp identical).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck lists the `manifests/` root, attributes every body to a catalog life or reports it as lifeless debris with count and bytes
- [ ] #2 A gtest deletes a namespace's catalog row, leaves one manifest body, and sees it in the fsck report
- [ ] #3 The summary (non-detail) path gains no per-namespace request; the added LIST is paged and deadline-checked
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'fsck oracle gaps')
<!-- SECTION:NOTES:END -->
