---
id: CAS-244
title: >-
  Name the storage-class requirement when a CAS read hits an archived object
  (`InvalidObjectState`)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:backend'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies:
  - CAS-243
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
documentation:
  - docs/en/antalya/cas/bucket-requirements.md
priority: low
type: enhancement
ordinal: 309000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A blob moved to Glacier fails with a raw `S3Exception`: `isObjectNotFound` (`CA/Backend/CasObjectStorageBackend.cpp:339-346`)
classifies only `NO_SUCH_KEY` / `RESOURCE_NOT_FOUND` / `NoSuchKey`, and `InvalidObjectState` appears nowhere in `src/`.
It fails closed, so this is diagnosability, not correctness. Name the requirement in the error (objects must stay in a
directly readable storage class) and point at `bucket-requirements.md`. No restore-and-retry path is wanted.

Provenance: BACKLOG/docs-and-cleanup.md#bucket-requirements-lifecycle-worm-glacier (Glacier half, 2031-triage CAS-012); verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A CAS read of an object answering `InvalidObjectState` throws an error naming the storage-class requirement
- [ ] #2 A unit test injects `InvalidObjectState` and asserts the message; the read still fails closed
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

Identifier trace: the earliest docs mention of `NoSuchKey` is 2026-06-06 (4df7956ec79); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
