---
id: CAS-141
title: >-
  Let `GC REBUILD` take the lease over an undecodable `gc/state` and report why
  the state was rejected
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:gc'
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - src/Disks/tests/gtest_cas_gc_rebuild.cpp
documentation:
  - docs/en/sql-reference/statements/system.md
  - docs/en/antalya/cas/operations/debugging.md
priority: high
type: bug
ordinal: 183000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An undecodable `gc/state` is the disaster `GC REBUILD` exists for, and it cannot recover it.
`rebuildBaseline` classifies the bytes correctly: its decode sits in an empty `catch (...)`
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4073-4080`).
It then calls `acquireOrRenewLease` (`:4206`), which decodes the same bytes again with no `try` (`:4761`).
So `GC REBUILD FORCE` throws `CORRUPTED_DATA` in exactly that case.
The only workaround, deleting `gc/state` from S3 by hand, is documented nowhere.
The empty catch also drops the decode message, so the operator cannot tell real damage from an environmental failure.
No gtest covers rebuild over garbage `gc/state` bytes (`src/Disks/tests/gtest_cas_gc_rebuild.cpp` covers absent state and damaged seals only).
Fix: the rebuild's lease acquisition treats an undecodable current body as replaceable under the observed etag. Regular rounds keep failing closed.

Provenance: BACKLOG/gc.md#rebuild-cannot-recover-undecodable-gc-state (opus review B8) and #rebuild-gcstate-decode-reason-unreported (2031-triage CAS-069). Same entry path as CAS-44. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792 (identical).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest writes garbage `gc/state` bytes; `rebuildBaseline(force)` performs and the next regular round succeeds
- [ ] #2 A regular round over the same garbage still throws `CORRUPTED_DATA` (fail-closed unchanged)
- [ ] #3 The rebuild logs the decode exception and names the reason on its report row
- [ ] #4 The rebuild docs state the undecodable-state case and no longer need a manual S3 delete
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-069')
<!-- SECTION:NOTES:END -->
