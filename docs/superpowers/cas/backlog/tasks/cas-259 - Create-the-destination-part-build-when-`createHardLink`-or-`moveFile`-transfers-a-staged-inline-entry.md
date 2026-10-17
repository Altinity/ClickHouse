---
id: CAS-259
title: >-
  Create the destination part build when `createHardLink` or `moveFile`
  transfers a staged inline entry
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - src/Disks/tests/gtest_ca_wiring.cpp
priority: low
type: bug
ordinal: 324000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`f7539af045c` made the inline write callback call `buildFor`, but the two transfer paths still do not: the staged-source Inline arm
of `createHardLink` pushes the entry with no `buildFor` (`CA/ContentAddressedTransaction.cpp:1219-1231`), and `moveFile` calls
`buildFor` only for `Blob` (`:1563-1575`). A cross-part transfer of an inline entry then reaches `publishStaging`'s
"staged entries ... without a Build" `LOGICAL_ERROR` (`:445`), an abort under `abort_on_logical_error`, unless the destination
ref is already committed.
Not reachable from MergeTree today: `createHardLink` callers carry forward from committed parts and `moveFile` renames inside one part.

Provenance: BACKLOG/formats-and-storage.md#staged-inline-hardlink-no-dst-buildtxn (2031-triage CAS-128); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Both inline arms call the idempotent `buildFor(*dst, dst_st)`
- [ ] #2 gtests next to `CASWiringWrite.InlineOnlyPartPublishesWithoutBuildCrash` (`src/Disks/tests/gtest_ca_wiring.cpp:936`) publish a cross-part inline hardlink and move
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
First recorded: 2026-08-21 (a41aaca7e24, by 'staged-inline-hardlink-no-dst-buildtxn')
<!-- SECTION:NOTES:END -->
