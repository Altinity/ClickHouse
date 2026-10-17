---
id: DRAFT-52
title: >-
  Decide whether CAS refuses non-Atomic databases or gets a durable journal for
  cross-namespace RENAME
status: Draft
assignee: []
created_date: '2026-09-26 12:54'
labels:
  - 'area:ref-ledger'
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2031'
documentation:
  - docs/superpowers/cas/2031-triage.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Cross-namespace `moveDirectory` (RENAME TABLE, cross-engine move) republishes refs one at a time with no journal. A crash mid-walk leaves the table split between two namespaces until an operator repeats the `RENAME`.
The code says true atomicity "would need a durable move-journal (deliberately out of scope)" (`CA/ContentAddressedTransaction.cpp:1300-1301`).
Nothing is lost: `republishRef` publishes the destination before dropping the source and is idempotent on re-entry. Refs added during the walk are blocked by the exclusive table lock.
Only non-Atomic (deprecated `Ordinary`) databases reach this branch, because Atomic databases rename without moving the data directory.
DRAFT-45 (committed-ref DDL overlay) would make the walk transaction-scoped but not crash-durable.
Options: (a) refuse `Ordinary` databases on CAS disks outright, which also removes the `detached`/`moving` name-folding case of CAS-261; (b) a durable move journal replayed at mount; (c) keep as is and document the manual re-`RENAME`.

Provenance: docs/superpowers/cas/2031-triage.md#cas-006 (2031-triage CAS-006); related DRAFT-45 and CAS-261. Draft because the fix direction is undecided. Verified 2026-09-26 against cae9288ee65 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records which option is taken and why, with the Ordinary-database population on known deployments as input
- [ ] #2 If (a): creating or attaching a CAS table in a non-Atomic database fails with a clear error, and CAS-261 is closed or narrowed
- [ ] #3 If (c): the operator docs describe the split state after a crash mid-RENAME and the recovery step
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
