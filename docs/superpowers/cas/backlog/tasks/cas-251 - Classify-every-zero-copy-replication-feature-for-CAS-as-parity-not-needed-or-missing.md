---
id: CAS-251
title: >-
  Classify every zero-copy replication feature for CAS as parity, not needed, or
  missing
status: To Do
assignee: []
created_date: '2026-09-03'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:replication'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreePartsMover.cpp
  - src/Storages/StorageReplicatedMergeTree.cpp
  - src/Storages/MergeTree/DataPartsExchange.cpp
  - tests/integration/test_cas_replicated_relink/test.py
priority: medium
type: research
ordinal: 316000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
CAS keeps `supportZeroCopyReplication()` false by design (`src/Disks/DiskObjectStorage/DiskObjectStorage.h:54-58`), so every
MergeTree path behind it silently takes the non-zero-copy branch on a CA disk. Nobody has listed which of those paths CAS
already covers by construction and which it lacks.
Scope: every site behind `allow_remote_fs_zero_copy_replication`, `supportZeroCopyReplication`, the `*SharedData*` family,
`tryToFetchIfShared`, `remote_fs_metadata` and the ten `*zero_copy*` MergeTree settings.
Known parity: metadata-only fetch (relink), same-pool `ATTACH`/`REPLACE PARTITION` (`05025_cas_attach_partition_cross_disk`),
queue-clone relink (`test_cas_replicated_relink`), the three `disable_*_for_zero_copy_replication` guards (`05024_cas_freeze`).
Known missing, already filed: one replica merges while others relink (CAS-103), `MOVE` relink (`move-to-ca-relink-from-replica`).
The prerequisite forced-relink-on-fetch design landed (`b794a1517dd`).

Provenance: BACKLOG/replication.md#zero-copy-parity-audit; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A table with one row per site lists the verdict (parity / not needed / missing) and the code or test that proves it
- [ ] #2 Every `missing` verdict not already filed has its own backlog task
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
First recorded: 2026-09-03 (9be5befc3ec, by 'zero-copy-parity-audit')
<!-- SECTION:NOTES:END -->
