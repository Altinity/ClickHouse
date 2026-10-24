---
id: DRAFT-50
title: >-
  Make the UNIQUE KEY delete-bitmap write use atomic file writes on a CAS disk
  when that path is wired
status: Draft
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - src/Storages/MergeTree/UniqueKey/DeleteBitmapFileOps.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: low
type: bug
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The generic crash-safe pattern (write `<name>.tmp`, then `replaceFile`) cannot work on a committed CA part across two autocommit
transactions: `moveFile` services only sources staged in the same transaction and throws `LOGICAL_ERROR` otherwise
(`CA/ContentAddressedTransaction.cpp:1580-1586`). The one caller, `DeleteBitmapFileOps::writeBitmapToStorage`
(`src/Storages/MergeTree/UniqueKey/DeleteBitmapFileOps.cpp:48`), is reached only from `MergeTreeBitmapStore::installBitmap`,
which has no caller outside its store and gtests, behind `allow_experimental_unique_key`.
The escape hatch exists: `IDataPartStorage::supportsAtomicFileWrites` (true for CA), used by
`VersionMetadataOnDisk::storeInfoToDataPartStorage` since `45e43b37aaf`.

Provenance: BACKLOG/replication.md#tmp-replacefile-on-committed-part (2031-triage CAS-057); draft because the path is unwired. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 When the delete-bitmap path gets a production caller, `writeBitmapToStorage` takes the atomic-write branch and a CA-disk test covers it
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
First recorded: 2026-08-21 (7d57211f229, by 'tmp-replacefile-on-committed-part')

2031-triage CAS-137: the audit re-opens CAS-057 on the claim that `MergeTreeBitmapStore` is a production caller; on both branches neither `MergeTreeBitmapStore` nor `SSTIndexWriter` is referenced outside `src/Storages/MergeTree/UniqueKey/` and tests, so the draft status stands. Extend the scope to `SSTIndexWriter::finalizeToStorage`, which uses the same `.tmp` + `replaceFile` pattern (`src/Storages/MergeTree/UniqueKey/SSTIndexWriter.cpp:256-283`).
<!-- SECTION:NOTES:END -->
