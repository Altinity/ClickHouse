---
id: CAS-70.6
title: Make the weakly discriminating hot-key lane tests assert their deciding deltas
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - src/Disks/tests/gtest_cas_hot_keys.cpp
  - src/Disks/tests/gtest_cas_ref_catalog.cpp
parent_task_id: CAS-70
priority: low
type: chore
ordinal: 90000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Each has a deterministic sibling, so none is a false-pass risk today; each alone cannot fail for the bug it names.
- `CleanConflictsBeforeAFault...`: probabilistic; assert the `ConflictPause`/`Reissue` deltas.
- `AConflictThatSettledAFault...`: the sleep bound does not discriminate; assert the `Reissue` delta.
- `ABaseReadThatFails...` (`gtest_cas_hot_keys.cpp:285`): pins only `sent_any` absolutely.
- The corruption test's "break again" arm; `AFailedEnqueue...` (`:185`) checks only the `inserted == true` arm.
- The GC-erase test says "exactly" in prose and asserts "at most" in code.
- The "nothing of ours landed" assertion: give the external writer a distinct `removal_started_round`.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (deferred item 7). `ACachedStartPastTheDeadlineSendsNothing` was already closed by the final review. Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each listed test fails when the mechanism it names is disabled (checked once by a local mutation)
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `Reissue` is 2026-07-18 (4420b5a3498); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
