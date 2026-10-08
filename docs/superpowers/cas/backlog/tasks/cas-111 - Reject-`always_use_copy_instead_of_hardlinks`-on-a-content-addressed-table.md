---
id: CAS-111
title: Reject `always_use_copy_instead_of_hardlinks` on a content-addressed table
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreeData.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: medium
type: bug
ordinal: 149000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Nothing rejects `always_use_copy_instead_of_hardlinks` (`src/Storages/MergeTree/MergeTreeSettings.cpp:1922`) on a CAS table
at `CREATE` or `ALTER ... MODIFY SETTING`. Once on, mutations (`MutateTask.cpp:2599`, `:2622`), the unchanged-part clone
(`:3418`) and same-disk `ATTACH/REPLACE/MOVE PARTITION` (`StorageMergeTree.cpp:3231`) take the copy branch, which reaches
`ContentAddressedTransaction::generateObjectKeyForPath`, a `notYet` throw (`CA/ContentAddressedTransaction.cpp:578-581`).
Fail-closed `NOT_IMPLEMENTED`, no corruption, but a mutation entry retries forever and the message blames a wrapping layer.
Not affected: `ALTER TABLE FREEZE`, BACKUP/RESTORE, zero-copy (dead on CAS). Fix: reject like the `SUPPORT_IS_DISABLED` gates
in `MergeTreeData::checkAlterIsPossible`, or serve `copyFile` as a manifest carry-forward like `createHardLink`.

Provenance: BACKLOG/operability-and-introspection.md#always-copy-instead-of-hardlinks-no-gate (2031-triage CAS-085); line numbers re-derived; verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `CREATE` and `ALTER ... MODIFY SETTING always_use_copy_instead_of_hardlinks = 1` on a CAS storage policy fail with a clear error, or the copy path works and a mutation succeeds
- [ ] #2 A stateless test covers the chosen behaviour
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
First recorded: 2026-08-21 (b03f1c765f8, by 'always-copy-instead-of-hardlinks-no-gate')
<!-- SECTION:NOTES:END -->
