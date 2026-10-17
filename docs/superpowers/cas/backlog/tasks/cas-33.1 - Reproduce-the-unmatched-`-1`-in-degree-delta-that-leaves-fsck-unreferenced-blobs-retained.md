---
id: CAS-33.1
title: >-
  Reproduce the unmatched `-1` in-degree delta that leaves fsck-unreferenced
  blobs retained
status: To Do
assignee: []
created_date: '2026-07-25'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'area:fsck'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasBlobInDegree.cpp
  - src/Common/ProfileEvents.cpp
parent_task_id: CAS-33
priority: low
type: research
ordinal: 41000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
56 blobs (104,755 bytes) are fsck-unreferenced while GC keeps in-degree 1, flat across 1000+ rounds (2026-07-25).
An unmatched `-1` (owning manifest gone) is a per-key no-op by design and is counted by `CASGCUnmatchedRemoveDeltas`
(`CA/Gc/CasBlobInDegree.cpp:652`). Safety holds (debris, not loss). Open: why the `-1` arrives unmatched. One traced case
is a 43 ms `tmp-fetch_*` publish/drop whose `+1` folded ~3 min later (unverified fold-order-inversion hypothesis).
Needs a targeted repro, not more log reading.

Provenance: BACKLOG/gc.md#fsck-gc-indegree-disagreement-2026-07-25; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A deterministic test reproduces an unmatched `-1` followed by a retained `+1`, or the hypothesis is refuted with evidence
- [ ] #2 If reproduced, a fix proposal names the ordering that must hold between the `+1` and `-1` folds
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
First recorded: 2026-07-25 (51455d6b521, by 'fsck-gc-indegree-disagreement-2026-07-25')
<!-- SECTION:NOTES:END -->
