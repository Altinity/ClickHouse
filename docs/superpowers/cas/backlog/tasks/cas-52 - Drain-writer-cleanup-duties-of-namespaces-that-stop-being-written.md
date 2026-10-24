---
id: CAS-52
title: Drain writer-cleanup duties of namespaces that stop being written
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:write-path'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
priority: medium
type: bug
ordinal: 62000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A part write whose precommit ended `Uncertain` or `Durable` hands a cleanup duty to the mount and keeps its build sequence active
(`PartWriteTxn::~PartWriteTxn`, `CA/Pool/CasPartWriteTxn.cpp:153-168`). The only drain is `mutateRefsAfterWriterCleanup`
(`CA/Pool/CasPool.h:1163-1167`), taken before the next ref mutation of the SAME namespace (`CA/Pool/CasPool.cpp:1990-2060`).
GC, mount, FSCK and the snapshot publisher never drain; teardown only observes (`CasPool.cpp:1028`, `:1183`).
Consequence: a namespace that is never written again holds its build sequence until process restart. That pins the mount's
`min_active_build_sequence`, and `prefixEligibleUnder` (`CA/Gc/CasOrphanManifestSweep.cpp:538-551`) then refuses the orphan sweep
for every later build of this writer epoch, in every namespace of the mount.
Second defect: the unclean-farewell WARNING (`CA/Pool/CasMountRuntime.cpp:1144`) blames "an unresolved ref-log PUT" even when an
undrained cleanup duty is the cause.

Provenance: BACKLOG/operability-and-introspection.md#writer-cleanup-single-drain (opus review M12); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A cleanup duty on a namespace that receives no further ref mutation is drained within a bounded time, and a test proves the build sequence is then retired
- [ ] #2 After that drain the orphan sweep can reclaim later builds of the same writer epoch
- [ ] #3 The unclean-farewell warning names the actual cause: unresolved ref-log PUT, undrained cleanup duty, or both
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
First recorded: 2026-08-22 (8b9cd77b94e, by 'writer-cleanup-single-drain')
<!-- SECTION:NOTES:END -->
