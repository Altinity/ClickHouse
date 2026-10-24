---
id: CAS-113
title: >-
  Record a durable publish in `publishStaging` before any post-commit work that
  can throw
status: To Do
assignee: []
created_date: '2026-07-26'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
priority: low
type: bug
ordinal: 151000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CachedPartFolderAccess::promoteBuild` records the commit in an allocation-free region and exposes `commit_recorded`
(`CA/Parts/PartFolderAccess.cpp:297-330`, from `8e6fe6ef0af`), but then runs `eraseView`, which can allocate
(`:277-284`, `recordDecision` `:692`). `ContentAddressedTransaction::publishStaging` calls `promoteBuild` without
`commit_recorded` and assigns `out_slot` only on return (`CA/ContentAddressedTransaction.cpp:473-474`). A
`MEMORY_LIMIT_EXCEEDED` in `eraseView` therefore reports a committed publish as failed. Only allocation failure reaches it.
Fix: extend the no-throw-after-commit rule one frame outward (pass `commit_recorded`, or make `eraseView` non-throwing).

Provenance: BACKLOG/operability-and-introspection.md#partb-review-findings (2026-07-25 publish-confirm review residual); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A post-commit allocation failure injected via `setPostCommitProbeForTest` leaves `publishStaging`'s `out_slot` set to the committed outcome
- [ ] #2 The other post-commit `eraseView` callers (`dropRef`, `repointRef`) either cannot throw after the commit or are documented as replay-safe
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
First recorded: 2026-07-26 (ba45217f307, by 'partb-review-findings')
<!-- SECTION:NOTES:END -->
