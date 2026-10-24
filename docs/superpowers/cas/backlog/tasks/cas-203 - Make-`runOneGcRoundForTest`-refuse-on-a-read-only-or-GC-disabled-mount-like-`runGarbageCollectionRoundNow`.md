---
id: CAS-203
title: >-
  Make `runOneGcRoundForTest` refuse on a read-only or GC-disabled mount like
  `runGarbageCollectionRoundNow`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:gc'
  - 'area:testing'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
priority: low
type: bug
ordinal: 260000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`runGarbageCollectionRoundNow` opens with `checkNotReadOnly("GC round")` and a `gc_enabled` refusal (`CA/ContentAddressedMetadataStorage.cpp:614-619`).
`runOneGcRoundForTest` (`:361-366`) starts at `checkOpAdmitted(CasOpClass::Admin)` with neither, on both branches.
A gtest driving the seam on a `<readonly>` mount runs a destructive round the SQL verb would refuse, so a regression in those gates is invisible.

Provenance: BACKLOG/testing-and-ci.md#gc-round-test-seam-drops-two-gates; verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The seam applies both gates, or calls the production method
- [ ] #2 A gtest shows the seam refuses on a read-only mount and on a GC-disabled mount
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
First recorded: 2026-08-21 (2583e3427aa, by 'gc-round-test-seam-drops-two-gates')
<!-- SECTION:NOTES:END -->
