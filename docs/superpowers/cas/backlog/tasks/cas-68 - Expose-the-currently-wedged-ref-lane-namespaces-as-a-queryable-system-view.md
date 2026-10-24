---
id: CAS-68
title: Expose the currently wedged ref-lane namespaces as a queryable system view
status: To Do
assignee: []
created_date: '2026-08-03'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:observability'
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - docs/en/antalya/cas/operations/debugging.md
priority: medium
type: enhancement
ordinal: 82000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`content_addressed_mounts.wedged_namespace_count` is only an aggregate (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:55`, from `wedgedRefLaneCount`, `Pool/CasRefLedger.cpp:1958-1975`), although the ledger's own map is keyed by namespace and the writer's error at wedge time already names namespace and txn id (`Pool/CasRefLedger.cpp:2358-2362,2371-2375`, `CASRefAppendWedged`). An operator who sees the count rise cannot list which namespaces are wedged without grepping logs. Add a per-namespace row (namespace, wedge reason, txn id, since) to a system table, either a new `system.cas_wedged_namespaces` or as a detail of the future per-part/ref views (DRAFT-1).

Provenance: BACKLOG/operability-and-introspection.md #cas-inspect (queryable list of wedged namespaces), split out of DRAFT-1 per review.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A SQL query lists every currently wedged namespace with reason, txn id and wedge time
- [ ] #2 The count in `content_addressed_mounts.wedged_namespace_count` equals the number of rows
- [ ] #3 Documented in the operations debugging guide next to the `cas-fsck --detail` recipe
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
First recorded: 2026-08-03 (09151538323, by 'cas-inspect')
<!-- SECTION:NOTES:END -->
