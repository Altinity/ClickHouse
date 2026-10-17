---
id: CAS-145
title: >-
  Add a soak card that loses `gc/state`, sees the guard refuse, runs `GC
  REBUILD`, and converges
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:soak'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies:
  - CAS-141
references:
  - utils/ca-soak/scenarios/cards
  - src/Disks/tests/gtest_cas_gc_rebuild.cpp
priority: medium
type: task
ordinal: 187000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
No soak card exercises the disaster-recovery path end to end. None of `utils/ca-soak/scenarios/cards/*.py` removes or
damages `gc/state`. The gtests cover rebuild pieces on the in-memory backend (`src/Disks/tests/gtest_cas_gc_rebuild.cpp`).
The rebuild deliberately over-protects: an unowned alive manifest's edges are kept
(`CASGCRebuild.UnownedAliveManifestOverProtected`, `:579`). The leak this leaves is bounded and fsck-visible only in
principle; nobody has measured it on a real pool under load.

Provenance: BACKLOG/gc.md [gc-rebuild follow-ups] parts (b) and (c). Verified 2026-09-26 against d4be7f7045a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A card deletes `gc/state` under a live workload, observes the guard refusing rounds, runs `GC REBUILD`, and reaches the GC fixpoint with `dangling = 0`
- [ ] #2 A variant writes garbage bytes into `gc/state` and recovers the same way
- [ ] #3 The card reports the over-protected residue (count and bytes) after the fixpoint and fails above a stated bound
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'gc-rebuild follow-ups')
<!-- SECTION:NOTES:END -->
