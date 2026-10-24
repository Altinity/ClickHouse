---
id: CAS-71.2
title: Combine queued same-key submissions into one write in the hot-key lane
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:ref-ledger'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:measurement'
dependencies:
  - CAS-70.3
  - CAS-71.1
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
parent_task_id: CAS-71
priority: low
type: feature
ordinal: 93000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Gate: the acceptance run shows queue wait, not the write, dominating a submission at p90.
Eligible members: same `CasRequests`, not `single_attempt`, no `Liveness` closure, bound deadline not earlier than the leader's. `taken` is set in the same critical section as allocation; "a leader takes this item" and "its caller leaves" are decided under one mutex (a use-after-free was found here in review round 17).
Chain: each member's `gate(reservedFor(0, 2))`, then `decide` on the candidate; a decline stops the chain; held exceptions are delivered only from a landed batch (as-if-serial). Settle mapping per outcome is in spec rev.26 (invariants HK4, HK7, HK8; tests 1-4, 12, 16).
A `decide`'s other reads must not observe a key a batch member may write.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-b item 1. Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Spec rev.26 tests 1-4, 12 and 16 pass as gtests
- [ ] #2 On the acceptance workload, `PUT` rate on `ref_catalog` and p90 submission latency drop versus phase A
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
First recorded: 2026-09-26 (b1c34d03479, by 'hot-key-lane-phase-b item 1')
<!-- SECTION:NOTES:END -->
