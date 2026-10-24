---
id: CAS-43
title: Count on the writer side each dedup-adopt that raced a GC condemn
status: To Do
assignee: []
created_date: '2026-07-24'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:observability'
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - CA/Pool/CasPartWriteTxn.cpp
  - CA/Gc/CasBlobInDegree.cpp
  - src/Common/ProfileEvents.cpp
priority: low
type: enhancement
ordinal: 53000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The GC half is done: the recovered-in-degree message is Debug and counted by `CASGCRetiredSparedByReref`
(`5ed8513597d`, `CA/Gc/CasBlobInDegree.cpp:456-467`); ~56 per run, 28 hot hashes in run 30019911967.
The writer still has no signal that it adopted a token GC was condemning, so the race can only be attributed from the
GC side. Add a `BlobAdoptRacedCondemn` ProfileEvent at the adopt site in `PartWriteTxn::ensureBlobPresent`.

Provenance: BACKLOG/gc.md [RECOVERED-INDEGREE-ATTRIBUTION] (residual half; Debug downgrade done in 5ed8513597d); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test that forces the adopt/condemn race increments the new writer counter once and the GC spare counter once
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
First recorded: 2026-07-24 (d5be10d4645, by 'RECOVERED-INDEGREE-ATTRIBUTION')
<!-- SECTION:NOTES:END -->
