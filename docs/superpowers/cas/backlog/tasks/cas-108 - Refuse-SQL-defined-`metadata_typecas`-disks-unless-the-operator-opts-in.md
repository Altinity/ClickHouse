---
id: CAS-108
title: Refuse SQL-defined `metadata_type=cas` disks unless the operator opts in
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-0
dependencies: []
references:
  - src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp
documentation:
  - docs/superpowers/specs/2026-08-25-dynamic-cas-disk-gate-design.md
  - docs/superpowers/plans/2026-08-25-dynamic-cas-disk-gate.md
priority: high
type: bug
ordinal: 146000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`disk(type=object_storage, metadata_type=cas, ...)` in `CREATE TABLE` builds a CAS disk through `DiskFromAST` with no gate:
any user with `CREATE TABLE` joins a process-wide shared pool, may use server credentials, gets a pool-wide view of other
namespaces and starts background work, all without a `SYSTEM CAS` grant or an operator-declared `storage_configuration`.
The disk creator ignores its `custom_disk` flag (`src/Disks/DiskObjectStorage/RegisterDiskObjectStorage.cpp:31-37`, unnamed
`bool, bool`), and the `"cas"` factory (`MetadataStorageFactory.cpp:219`) has none either. Template one file over: the
`use_fake_transaction` rejection (`RegisterDiskObjectStorage.cpp:97-100`). Design and TDD plan are written: server setting
`cas_allow_unsafe_dynamic_disks` (default `false`), checked before any disk-construction side effect; server-configured
disks are unaffected. Open on both branches. Originally fable umbrella-review B1 (P1).

Provenance: BACKLOG/operability-and-introspection.md#dynamic-cas-disk-no-gate and #cas-custom-disk-privilege-bypass (final-checks-todo.md 9.B3); verified 2026-09-26 against dd0ed2f263a (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Related: docs-and-cleanup.md#pool-trust-boundary-undocumented (docs only).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `CREATE TABLE ... SETTINGS disk = disk(metadata_type='cas', ...)` fails with a clear error when `cas_allow_unsafe_dynamic_disks` is false, before any S3 request or pool registration
- [ ] #2 With the setting true the dynamic disk works as today, and a disk from `storage_configuration` is unaffected either way
- [ ] #3 The design doc records whether a dedicated grant is also required, and the choice is implemented or explicitly rejected
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
First recorded: 2026-09-26 (eff9336f676, by 'cas-custom-disk-privilege-bypass')
<!-- SECTION:NOTES:END -->
