---
id: DRAFT-48
title: Publish sub-single-part blobs from memory instead of staging them on disk
status: Draft
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: low
type: enhancement
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Optional step 7 of the S3-native staging design: buffer blobs smaller than one multipart part in memory and feed them straight
to the ordinary unconditional `publishBlob` stream, skipping local disk staging and native copy. Not implemented.
It must keep the mandatory blob HEAD (decision-1) and the shared `publication_attempted` state. No measurement shows the gain.

Provenance: BACKLOG/formats-and-storage.md [S3-native staging §7]; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A measurement on a small-part workload shows whether disk staging of small blobs costs enough to change
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'S3-native staging §7')
<!-- SECTION:NOTES:END -->
