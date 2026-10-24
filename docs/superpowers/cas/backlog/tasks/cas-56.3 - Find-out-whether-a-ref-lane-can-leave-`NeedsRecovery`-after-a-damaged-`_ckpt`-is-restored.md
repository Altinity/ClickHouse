---
id: CAS-56.3
title: >-
  Find out whether a ref lane can leave `NeedsRecovery` after a damaged `_ckpt`
  is restored
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
parent_task_id: CAS-56
priority: medium
type: spike
ordinal: 69000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
In the T8 criterion-4 injection the lane stayed in `CASRefNeedsRecovery` for ~20 minutes, including after the byte-identical original `_ckpt`
was restored. `NeedsRecovery` is a hard lane fence (`CA/Pool/CasRefLedger.cpp:1439-1443`). One observation only: it may be a wedge,
a remount-only exit, or an artifact of the injected shape. No test found restores the object and asserts the lane recovers.
If the only exit is a remount, the runbook must say so and recovery should retry on its own; if it is a wedge, file a bug.

Provenance: BACKLOG/operability-and-introspection.md#damaged-object-repair item 3; verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A deterministic test damages a `_ckpt` under a live writer, restores it, and records whether and how the lane leaves `NeedsRecovery`
- [ ] #2 The outcome is written down: self-heals, needs remount, or a new bug task with the repro
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

Identifier trace: the earliest docs mention of `CasRefLedger` is 2026-07-15 (416c982b5d6); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
