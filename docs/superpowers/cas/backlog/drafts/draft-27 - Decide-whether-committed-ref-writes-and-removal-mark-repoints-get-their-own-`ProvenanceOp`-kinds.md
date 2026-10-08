---
id: DRAFT-27
title: >-
  Decide whether committed-ref writes and removal-mark repoints get their own
  `ProvenanceOp` kinds
status: Draft
assignee: []
created_date: '2026-07-15'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:formats'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:on-s3-format'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ProvenanceOp` (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.h:26-34`) has
Insert, Merge, Mutation, Attach and Repack. Committed-ref writes and removal-mark repoints both use `Other`
(`ContentAddressedTransaction.cpp:421,788`, `Parts/PartFolderAccess.cpp:503`), so the audit trail cannot tell them apart.
The op is persisted in the blob envelope, which is format v1 since 26.6.4: new values need a format version and readers that
accept unknown words (decision-4). Product-owner call.

Provenance: BACKLOG/gc.md#gc-observability [ProvenanceOp operability gap]. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792 (unchanged on both).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner decides whether the distinction is worth a format version, recorded with `backlog decision create`
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
First recorded: 2026-07-15 (6e02e1f77e3, by 'ProvenanceOp operability gap')
<!-- SECTION:NOTES:END -->
