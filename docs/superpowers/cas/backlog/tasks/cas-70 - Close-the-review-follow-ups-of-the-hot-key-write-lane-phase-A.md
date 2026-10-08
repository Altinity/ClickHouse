---
id: CAS-70
title: 'Close the review follow-ups of the hot-key write lane, phase A'
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp
  - src/Disks/tests/gtest_cas_hot_keys.cpp
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
  - docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md
  - docs/superpowers/cas/2026-09-04-hot-key-lane-phase-a-rulings.md
  - docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md
priority: medium
type: chore
ordinal: 84000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The hot-key lane (`R/Backend/CasHotKeys.{h,cpp}`, `4ec755474fb`) serializes a process's own writes to hot control objects; it fixed the `ref_catalog` starvation on the CA-s3 stateless lane (`CREATE TABLE` gave up after 78.9 s) and is on both branches since 2026-09-05.
Its reviews deferred a sanitizer run, the acceptance measurement (the gate for phase B), one observability counter, a reentrancy guard, and a set of hygiene and test-strength items. None blocked the merge. The subtasks carry them.
Two items of the lane's `task-1-review.md` are lost: the SDD workspace that held them no longer exists.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (alias #ref-catalog-cas-starvation). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every subtask is Done or explicitly dropped with a reason
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
Correction (post-import review): the two task-1-review.md items previously called lost were recovered from tmp/task-1-review.md and imported as CAS-81 (spec overstates hot-key-lane coverage) and CAS-82 (flat conflict pause raises the request ceiling on five off-lane read-modify-write sites).

First recorded: 2026-09-04 (16fd9bc01cd, by 'hot-key-lane-phase-a-followups')
<!-- SECTION:NOTES:END -->
