---
id: CAS-265
title: >-
  Reject a CAS object header with compatibility version 0 in
  `checkCompatibility`
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:formats'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp
documentation:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/README.md
priority: low
type: chore
ordinal: 330000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`checkCompatibility` (`CA/Formats/CasFormat.cpp:72-78`) throws `UNKNOWN_FORMAT_VERSION` only above `G_BUILD`. Since the
generation reset (decision-3) every class starts at the shared `{1, 1}` baseline (`CasFormat.cpp:28`, `changePoints`), so the
only below-birth version today is 0. `decodePoolMeta` already refuses it (`CA/Formats/CasPoolMetaFormat.cpp:102-106`); the
other decoders accept it.
When a class first gets its own change array, the same check must refuse a version below that class's first generation.
Behavior-only: it rejects headers no build writes, so the frozen format is untouched (decision-4).

Provenance: BACKLOG/formats-and-storage.md#cas-format-version-floor, narrowed after the generation reset. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every decoder refuses a `v = 0` header as `CORRUPTED_DATA`, checked centrally, with one gtest per format family
- [ ] #2 The check reads the floor from `changePoints(id).front()` so a later per-class array is covered
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
First recorded: 2026-08-04 (5345f6b693b, by 'cas-format-version-floor')
<!-- SECTION:NOTES:END -->
