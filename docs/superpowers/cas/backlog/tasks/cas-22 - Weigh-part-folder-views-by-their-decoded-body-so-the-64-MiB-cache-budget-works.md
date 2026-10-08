---
id: CAS-22
title: Weigh part-folder views by their decoded body so the 64 MiB cache budget works
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
  - docs/superpowers/cas/2031-triage.md#cas-045
priority: medium
type: bug
ordinal: 28000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`PartFolderView::estimatedBytes` returns `256 + manifest_size` (`R/Parts/PartFolderAccess.cpp:132-136`), and both producers pass `.manifest_size = 0` (`R/Pool/CasRefLedger.cpp:353`, `:385`). Every view weighs 256 bytes.
The 64 MiB byte budget degenerates to a 262,144-entry cap, above the real 10,000-entry cap. `CASPartFolderCacheBytes` reports the same fiction, and the oversized-entry bypass (`PartFolderAccess.cpp:197`) can never fire, so `CASPartFolderViewOversizedBypasses` is always zero.
The unit test hand-feeds `manifest_size=1000` (`gtest_cas_part_folder_view.cpp:50`), which hid the bug. Memory accounting only, no correctness impact.

Provenance: BACKLOG/performance.md#part-folder-cache-weight-always-256 (2031-triage CAS-045). Same family as formats-and-storage.md#manifest-inline-budget-no-spill. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 View weight derives from the decoded body; the dead `manifest_size` field is removed
- [ ] #2 A gtest asserts a manifest with a large inline body weighs more than an empty one, and an oversized view bypasses the cache and increments the counter
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
First recorded: 2026-08-21 (c2af49103dc, by 'part-folder-cache-weight-always-256')
<!-- SECTION:NOTES:END -->
