---
id: CAS-159
title: >-
  Show in `system.cas_mounts` why a slot has no live row: listing failed, or a
  member is mid-retirement
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
milestone: m-7
dependencies: []
references:
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
priority: low
type: enhancement
ordinal: 205000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two causes read the same as an empty pool:
- When `Cas::listMounts` throws, the synthesized row leaves `state` empty via `insertDefault`
  (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:161-177`, `:227-245`), so a throttled LIST looks like no members.
- A decommission crash can leave a member with `mount` and `epoch` deleted and the owner anchor not yet tombstoned;
  `listMounts` lists `mount` keys only (`CA/Pool/CasServerRoot.cpp:1195-1216`), so that slot has no row. It is resumable
  (`Pool::openForDecommission`, `EpochMintPolicy::DecommissionRecovery`; pinned by `gtest_cas_decommission.cpp`
  `SuccessorReclaimAfterEpochDeleteKeepsOwnerAnchor`, `MidRetirementCrashResumesViaMountLeaseFallback`).
Owed: a `state` word such as `unknown`/`retiring`, or a nullable `mount_list_error` column.

Provenance: BACKLOG/mounts-and-lifecycle.md#owner-only-slot-invisible-in-mounts (merged [mounts-list-failure-indistinguishable]); verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A failed mount listing is distinguishable from an empty pool in `system.cas_mounts`, shown by a test with an injected LIST failure
- [ ] #2 An owner-only slot appears with a state that says it is retiring
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
First recorded: 2026-08-21 (71adf002454, by 'mounts-list-failure-indistinguishable')
<!-- SECTION:NOTES:END -->
