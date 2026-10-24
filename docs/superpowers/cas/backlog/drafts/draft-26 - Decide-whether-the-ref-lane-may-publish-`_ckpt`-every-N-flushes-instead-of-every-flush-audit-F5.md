---
id: DRAFT-26
title: >-
  Decide whether the ref lane may publish `_ckpt` every N flushes instead of
  every flush (audit F5)
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:ref-ledger'
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-2
dependencies:
  - CAS-176
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCkpt.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f5
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#open-questions
priority: high
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CASRefCheckpointPublished == CASRefBatchFlushes` (18.7k/h each on otel.demo): 477k `_ckpt` PUTs/day, 21% of all PUTs.
That PUT is one of the two on each flush's critical path, and the lane is the insert bottleneck (748 ms of an 887 ms
insert is queue wait, audit conclusion 1). A checkpoint every N flushes or T seconds, plus on snapshot publish,
lengthens the recovery walk by at most N logs. Estimate: up to 20% fewer PUTs and ~40% less per-flush lane latency.
This changes a protocol step, so it needs the owner's explicit approval (AGENTS.md invariant 5; spec open question 4).
It is not covered by decision-1, which is about `HEAD` before a blob `PUT`, and it writes no new object kind (decision-4 not triggered).
F31 item 2 (optimistic `_ckpt` write from a cached etag) removes the `_ckpt` GET first; decide this after it.

Provenance: BACKLOG/gc.md#otel-demo-s3-budget-audit-2026-09-25 (F5; stated only in this pointer entry and the spec's open question 4, no BACKLOG item). Related: CAS-71.5. Verified 2026-09-26: no unit carries F5.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner decides N/T and the recovery bound, recorded with `backlog decision create`
- [ ] #2 If approved: a crash-recovery gtest shows recovery walks at most N logs and loses no committed ref
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
First recorded: 2026-09-26 (83949645ce7, by 'audit F5')
<!-- SECTION:NOTES:END -->
