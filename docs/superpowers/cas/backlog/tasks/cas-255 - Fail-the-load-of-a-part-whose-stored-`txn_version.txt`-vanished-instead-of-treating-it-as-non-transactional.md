---
id: CAS-255
title: >-
  Fail the load of a part whose stored `txn_version.txt` vanished instead of
  treating it as non-transactional
status: To Do
assignee: []
created_date: '2026-09-17'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'origin:review'
milestone: m-8
dependencies:
  - CAS-177
references:
  - src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/Local/MetadataStorageFromDiskTransactionOperations.cpp
  - 'https://github.com/Altinity/ClickHouse/pull/2396'
  - 'https://github.com/Altinity/ClickHouse/issues/2344'
documentation:
  - >-
    docs/superpowers/specs/2026-09-16-transaction-metadata-store-best-effort-design.md
priority: medium
type: bug
ordinal: 320000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`VersionMetadataOnDisk::loadMetadata` (`src/Interpreters/MergeTreeTransaction/VersionMetadataOnDisk.cpp:48-95`) returns
`NonTransactionalTID`/`NonTransactionalCSN` when neither `txn_version.txt` nor its `.tmp` exists. That is right for a part that
never had the file and wrong for one whose file was stored and then lost. `ReplaceFileOperation::execute`
(`src/Disks/DiskObjectStorage/MetadataStorages/Local/MetadataStorageFromDiskTransactionOperations.cpp:428`) can lose the
destination when both the move and its undo fail.
With the bounded retry of PR #2396, a retried `setAndStoreRemovalTID(EmptyTID)` reloads the synthesized record and accepts it,
so a running snapshot can see a part created after it.
Fix: if the in-memory info says a record was stored (`storing_version > 0`) and no file is found, throw a `CORRUPTED_DATA`-class
error, not `LOGICAL_ERROR` (input-reachable). Generic MergeTree code, upstream-portable; a separate PR after #2396.

Provenance: BACKLOG/formats-and-storage.md#version-metadata-reload-fail-closed; found by a codex review of the retry branch. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A fault test where replacement and undo both fail makes the reload throw instead of returning a non-transactional record
- [ ] #2 Parts that never had the file still load as non-transactional
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
First recorded: 2026-09-17 (bbba96594a7, by 'version-metadata-reload-fail-closed')
<!-- SECTION:NOTES:END -->
