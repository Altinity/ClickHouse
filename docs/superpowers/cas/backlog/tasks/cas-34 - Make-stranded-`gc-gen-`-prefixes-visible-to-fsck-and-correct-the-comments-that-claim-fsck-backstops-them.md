---
id: CAS-34
title: >-
  Make stranded `gc/gen/` prefixes visible to fsck and correct the comments that
  claim fsck backstops them
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:fsck'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Tools/CasFsck.cpp
  - CA/Pool/CasServerRoot.h
  - src/Disks/tests/gtest_cas_s3_staging.cpp
priority: medium
type: bug
ordinal: 44000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three comments in `runRegularRound`'s hand-off block (`CA/Gc/CasGc.cpp:1031-1032,1052,1072`) say a skipped or crashed
generation prefix is left to fsck, but `runFsck` lists only namespace roots, blobs and manifests
(`CA/Tools/CasFsck.cpp:529,730,915`) and never `gc/`. Such prefixes (whole snapshot runs, O(edges) bytes) appear in no
counter and no size. Owed: fix the prose, add a bounded advisory count of `gc/gen/` generations below
`snap_pruned_through`, then decide on a reclaimer. Same sweep: the false "GC lists `blobs/`" claims at
`CA/Pool/CasServerRoot.h:622` and `src/Disks/tests/gtest_cas_s3_staging.cpp:961`.

Provenance: BACKLOG/gc.md#stranded-generation-prefix-invisible-to-fsck (2031-triage CAS-074); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck reports the count and bytes of `gc/gen/` generations below the prune cursor, bounded in requests
- [ ] #2 No comment claims fsck or GC does something it does not do
- [ ] #3 A test leaves one stranded generation and sees it in the fsck report
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
u03-gc-c merged the hand-off single-crash leak here (no separate task).

First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-074')
<!-- SECTION:NOTES:END -->
