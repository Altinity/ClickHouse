---
id: CAS-71.5
title: Route `_ckpt` and `gc/state` writes through the hot-key lane
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:decision'
dependencies:
  - CAS-176
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f31
parent_task_id: CAS-71
priority: low
type: feature
ordinal: 96000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Both still use `readModifyWrite`: `publishCkpt` (`R/Pool/CasRefCkpt.cpp:259`) and `Gc::acquireOrRenewLease` (`R/Gc/CasGc.cpp:4741`). Per site the change is `readModifyWrite` to `submit` in a `Conflict` loop with the pause rule.
Gate: `publishCkptContribution`'s `decide` counts its runs and records its decline reason in captured locals the caller reads, so a re-run on a fresh base double-reports; move both out of the closure first.
Go/no-go for leaving `_ckpt` on `readModifyWrite`: 40·M requests per second per contended key for M writers, and 429/`SlowDown` not above today's.
Owner decision F31 item 2 (`u11-refproto:#part-publish-zero-gets`) rewrites `publishCkpt` as an optimistic write from a cached etag; decide this subtask after that lands.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-b item 5. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `publishCkptContribution`'s `decide` has no captured side-effect state; a re-run test proves it
- [ ] #2 Recorded decision whether `_ckpt` and `gc/state` go through the lane, with the measured request rate per contended key
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
First recorded: 2026-09-26 (b1c34d03479, by 'hot-key-lane-phase-b item 5')
<!-- SECTION:NOTES:END -->
