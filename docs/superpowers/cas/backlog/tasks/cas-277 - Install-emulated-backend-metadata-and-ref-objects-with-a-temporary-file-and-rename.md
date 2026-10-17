---
id: CAS-277
title: >-
  Install emulated-backend metadata and ref objects with a temporary file and
  rename
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
priority: low
type: bug
ordinal: 344000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Blob bodies on the emulated backend are installed atomically (`emuPublishBlobAtomically`, `CA/Backend/CasObjectStorageBackend.cpp:488`),
but ordinary metadata and ref writes go through `emuWrite` (`:478`, called `:867`) and `LocalObjectStorage::writeObject` on the
final key with `O_TRUNC`. A concurrent reader of the same key can see a half-written object; the known case is two concurrent
fetches writing one `detached/<part>` ref. Safe on S3 (atomic PUT). CI, unit tests and local pools only.

Provenance: BACKLOG/formats-and-storage.md [B66a]; the POSIX backend would retire it. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Mutable emulated writes go through a sibling temporary file plus rename, keeping the existing lock and token semantics
- [ ] #2 A gtest with a concurrent reader never observes a partial object
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

Identifier trace: the earliest docs mention of `LocalObjectStorage` is 2026-06-11 (99466809c92); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
