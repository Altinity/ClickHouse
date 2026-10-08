---
id: DRAFT-25
title: >-
  Decide whether an unattributed write needs its own signal or
  `CAS_WRITE_UNATTRIBUTED` retires
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:backend'
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
documentation:
  - docs/superpowers/cas/2026-09-03-request-contract-rulings.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CAS_WRITE_UNATTRIBUTED` (error code 1037, `src/Common/ErrorCodes.cpp:690`) is unreachable on the Native/S3 path: its only
throw site is `emuMintToken` on the emulated backend (`CA/Backend/CasObjectStorageBackend.cpp:573`), and the request engine
settles a 2xx that fits no grammar by a resolve read, giving up as `GaveUp{Unresolved}`. The three tests that pinned the throw
now pin the give-up. Spec revision 13 left this as an open product question.

Provenance: BACKLOG/operability-and-introspection.md#cas-write-unattributed-product-question (2026-09-03); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An owner decision is recorded: add an unattributed-write event/counter, or retire the error code and the emulated throw
- [ ] #2 The three tests that pinned the old throw match the decision
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
First recorded: 2026-09-26 (b56f61fdd9d, by 'cas-write-unattributed-product-question')
<!-- SECTION:NOTES:END -->
