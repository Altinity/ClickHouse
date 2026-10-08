---
id: CAS-194
title: >-
  Remove the retry-later window after `TRUNCATE` of a `Join`/`Set` table on a
  CAS disk
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - src/Storages/StorageSet.cpp
  - src/Storages/StorageJoin.cpp
  - R/Pool/CasRefLedger.cpp
priority: low
type: enhancement
ordinal: 251000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`StorageSet::truncate` and `StorageJoin::truncate` call `removeRecursive` then `createDirectories` (`src/Storages/StorageSet.cpp:255-260`, `StorageJoin.cpp:162-167`); on CAS the second is an admission no-op.
The next write throws retry-later ("is Removing: creation waits for its terminal fold", `R/Pool/CasRefLedger.cpp:1354`) until a GC round reclaims the row. Self-healing, bounded by GC round latency.
Pinned by `gtest_cas_ref_writer.cpp:4823` `CASRefWriterNamespaceRemoval.FilesOnlyNamespaceTruncateThrowsRetryLaterUntilGcReclaimsThenRebirths`.
Options: a fast rebirth path in `namespaceLife` when the predecessor is provably terminal, or a `truncate` that waits like `DROP TABLE ... SYNC`.

Provenance: BACKLOG/ref-protocol.md#cas-join-set-truncate. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An `INSERT` right after `TRUNCATE` of a `Join` or `Set` table on a CAS disk succeeds without waiting for a GC round (stateless test)
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
First recorded: 2026-08-04 (63bdac99d21, by 'cas-join-set-truncate')
<!-- SECTION:NOTES:END -->
