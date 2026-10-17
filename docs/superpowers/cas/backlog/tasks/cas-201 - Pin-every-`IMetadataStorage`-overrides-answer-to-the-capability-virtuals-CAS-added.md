---
id: CAS-201
title: >-
  Pin every `IMetadataStorage` override's answer to the capability virtuals CAS
  added
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:testing'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h
  - src/Disks/MetadataStorageWithPathWrapper.h
priority: low
type: task
ordinal: 258000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
CAS added capability virtuals with a `false` default to `IMetadataStorage` (`src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h`):
`tryCreateWriteBuffer` (`:125`), `transactionIsStagingOverlay` (`:331`), `supportsAtomicFileWrites` (`:335`), `supportsTransactionalMutableFiles` (`:377`).
Seven classes override the interface (Plain, ContentAddressed, Local, Cache, PlainRewritable, Web, `MetadataStorageWithPathWrapper`);
only Local, Plain and PlainRewritable have gtest files. A wrapper that forgets to forward a capability silently changes MergeTree's write path
for the wrapped disk. Pinning the answers is cheaper than unit-testing each override in full, which is upstream scope.

Provenance: BACKLOG/testing-and-ci.md [imetadatastorage-override-coverage-gap], reframed from 'unit-test every override'; verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 One table-driven gtest asserts each override's answer to each CAS-added capability, including wrappers over a CA storage
- [ ] #2 Adding a new capability virtual without extending the table fails the test
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
First recorded: 2026-08-04 (f08734d17df, by 'imetadatastorage-override-coverage-gap')
<!-- SECTION:NOTES:END -->
