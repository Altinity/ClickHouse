---
id: CAS-300
title: >-
  Land PR #2415: BACKUP of CAS tables to an S3 or disk destination on the pool's
  endpoint copies payload bytes, not envelopes or empty keys
status: In Progress
assignee:
  - '@k-morozov'
created_date: '2026-09-26 12:37'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:backend'
  - 'area:formats'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:issue'
  - 'origin:2031-triage'
milestone: m-3
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2415'
  - src/Backups/BackupIO_S3.cpp
  - src/Disks/DiskObjectStorage/DiskObjectStorageTransaction.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/IMetadataStorage.h
documentation:
  - docs/en/antalya/cas/operations/backup.md
priority: high
type: bug
ordinal: 379000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`BACKUP` of a CAS table to S3 on the same endpoint takes a server-side copy (`BackupWriterS3::copyFileFromDisk`), which assumes the file
is a whole object from byte 0. On CAS an inline file (`checksums.txt`, `count.txt`, ...) has no object and `getStorageObjects` returns an
empty key, and a blob is `[envelope][payload]`, so the copy starts at the envelope. Today the empty key aborts the backup first; fixing
only that would produce backups that report success and cannot be restored. `BACKUP ... TO Disk(...)` on the same endpoint has the same
bug through `DiskObjectStorage::copyFile` → `copyFileImpl` → `copyObjectToAnotherObjectStorage`.
PR #2415 (draft, @k-morozov): inline entries are read with `readInlineDataToString`; blobs pass the offset from a new
`IMetadataStorage::getObjectPayloadOffset` as `src_offset` to `copyS3File`; `S3ObjectStorage` throws `LOGICAL_ERROR` on an empty key.
Adds `test_cas_backup_s3_native_copy` and `docs/en/antalya/cas/operations/backup.md`. 31 files, generic Disks/Backups code touched.
RESTORE onto CAS is unaffected. The same `copyFileImpl` path is the MOVE-out bug of CAS-254.

Provenance: open PR #2415 (draft, branch cas/backup-native-copy-bug, base antalya-26.6), reconciled by u17-github-cas 2026-09-26. Related CAS-115 (runbook page added by the same PR), CAS-254 (MOVE out of CAS, same copy path).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 PR #2415 or its replacement is merged into antalya-26.6 with `test_cas_backup_s3_native_copy` green
- [ ] #2 BACKUP to S3 and to a same-endpoint disk, then RESTORE, round-trips row checksums for a table with inline files and blobs
- [ ] #3 An empty remote key reaching a server-side copy fails with a named error instead of copying
- [ ] #4 Hunks outside the CAS directories are graded as fork patches (minimal, motivated, portable) in the PR description
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
PR #2415 (draft), branch cas/backup-native-copy-bug, base antalya-26.6.

2031-triage CAS-020: the same-endpoint BACKUP half of the audit finding (`getStorageObjects` drops the envelope offset). MOVE half is CAS-254.
<!-- SECTION:NOTES:END -->
