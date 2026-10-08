---
id: CAS-144
title: >-
  Decide whether `GC REBUILD` is safe against live writers or needs a
  mount-lease interlock
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:gc'
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:high'
  - 'confidence:contested'
  - 'needs:decision'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - programs/disks/CommandCaGcRebuild.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
documentation:
  - docs/en/sql-reference/statements/system.md
  - docs/en/antalya/cas/architecture/garbage-collection.md
priority: medium
type: spike
ordinal: 186000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The offline `clickhouse-disks cas-gc-rebuild` checks only that its own disk is opened read-only
(`programs/disks/CommandCaGcRebuild.cpp`). It never looks at other servers' mount leases, and its comment says it
"must never claim the live server's mount". `SYSTEM CAS GC REBUILD` runs the same `rebuildBaseline` on a live
server by design (`ContentAddressedMetadataStorage::runGcRebuildNow`), with writers active on that and other nodes.
The rebuild's only exclusion is the GC lease with no steal (`Gc/CasGc.cpp:4200-4210`); it throws retry-later if it
"lost authority before the catalog settled". So either the rebuild is already safe against concurrent publishes, which makes the
CLI caveat and a mount interlock unnecessary, or both entry points share a gap. The source calls it a real safety gap
in a destructive tool, but no reproduction exists.

Provenance: BACKLOG/gc.md [gc-rebuild-lease-interlock]. Contested because the SQL entry point runs online on purpose. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written argument, reviewed, states whether a publish racing the rebuild's catalog cut and hot LIST can lose an edge
- [ ] #2 If unsafe: both entry points refuse while any foreign mount lease is live, with a test; if safe: a test races a publish against a rebuild and the docs drop the caveat
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
First recorded: 2026-08-04 (f08734d17df, by 'gc-rebuild-lease-interlock')

2031-triage CAS-004: residual = offline `clickhouse-disks cas-gc-rebuild` has no mount-lease interlock against a live writer; the server-side `SYSTEM CAS GC REBUILD` holds the live GC lease and condemns nothing (ledger `docs/superpowers/cas/2031-triage.md#cas-004`).
<!-- SECTION:NOTES:END -->
