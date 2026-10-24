---
id: CAS-35
title: >-
  Defer, not drop, the hand-off reclaim of a generation when the round is
  suppressed
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
milestone: m-8
dependencies:
  - CAS-34
references:
  - CA/Gc/CasGc.cpp
  - src/Disks/tests/gtest_cas_gc_frontier_gate.cpp
priority: low
type: bug
ordinal: 45000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The hand-off is a one-shot difference between the parent seal's runs and the new seal's. A suppressed round still folds,
so the ref moves off the old generation and nothing revisits it: `handoff_candidates = suppress_destructive ? kNoRuns :
parent_seal_runs` (`CA/Gc/CasGc.cpp:1057-1059`), and `snap_pruned_through` is already past it. The prefix leaks to
fsck, which does not see `gc/` today. Rarer since `kDefault = Authoritative`, but still fires on an unproven frontier,
a lost CAS or a held namespace. Spec C1 keeps the hand-off uncut, so this is not covered there.
Pinned by `CASGCFrontierGate.TheHandOffReclaimIsInertUnderSuppression`.

Provenance: BACKLOG/gc.md#suppressed-handoff-consumption; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A generation skipped by a suppressed round is reclaimed by a later unsuppressed round
- [ ] #2 The existing inertness test still passes: nothing is deleted on the suppressed round itself
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
First recorded: 2026-07-29 (113bb4e0966, by 'SUPPRESSED-HANDOFF-CONSUMPTION')
<!-- SECTION:NOTES:END -->
