---
id: CAS-274
title: >-
  Carry blob references instead of re-reading bytes on a CA-to-CA move within
  one pool
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
dependencies: []
references:
  - src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
  - utils/ca-soak/scenarios/cards/s36_s37_disk_move.py
priority: low
type: enhancement
ordinal: 339000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
For a CA destination, `DataPartStorageOnDiskBase::clonePart` (`src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp:790-813`)
runs `copyDirectoryContentIntoTransaction` (`:637-661`), a sequential `readFile`+`writeFile` per file, only to rediscover blob
references the source manifest already names. The upload side is near-free (HEAD-before-PUT dedup), the read side is not.
Unverified: whether the MOVE target ref collides with the source's existing ref when both disks share the table namespace;
the S37 CA-to-CA leg on `ca_local3` decides it. Low value: two CA disks on one pool relocate nothing physically, and a move to
another pool cannot relink anyway.

Provenance: BACKLOG/replication.md#same-pool-move-reads-every-byte (2031-triage CAS-120); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The S37 CA-to-CA leg result is recorded, and it answers whether the target ref collides
- [ ] #2 If kept, a same-pool move adopts the source manifest's references and reads no blob bytes (checked by read counters)
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
First recorded: 2026-08-21 (b9a1774ba4e, by 'same-pool-move-reads-every-byte')
<!-- SECTION:NOTES:END -->
