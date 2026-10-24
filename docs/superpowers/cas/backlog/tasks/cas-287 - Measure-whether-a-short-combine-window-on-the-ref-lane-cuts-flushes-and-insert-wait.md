---
id: CAS-287
title: >-
  Measure whether a short combine window on the ref lane cuts flushes and insert
  wait
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:write-path'
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'theme:parity'
milestone: m-2
dependencies:
  - CAS-176
  - CAS-83
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f25
  - >-
    docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#conclusions
priority: high
type: spike
ordinal: 359000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The ref lane is the canary's insert bottleneck: lane ~100% busy, 748 ms of an 887 ms insert is queue wait, 3.1 mutations per flush
in steady state and up to 34 in removal bursts, at most ~5 flushes per second (audit section 1, F25).
The lane is a leader/follower group commit with no delay: the leader carves every pending item at once
(`CA/Pool/CasRefLedger.cpp:2059` `appendRefOpsOnRuntime`, `:2753` `flushRefBatch`). Each flush costs one `_log` PUT and one `_ckpt` PUT.
A delay-and-combine window of a few milliseconds may raise mutations per flush and cut PUTs, at the cost of latency when the lane is idle.
Measure after CAS-176 (flush from four round trips to two) and CAS-83 (no repoint), which change the flush cost this depends on.

Provenance: umbrella-roadmap.md section 2 bullet 'Ref-lane batching'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A soak or stand run records mutations per flush, flushes per second and insert lane wait with no window and with at least two window sizes
- [ ] #2 The result states whether a window lowers p50 and p99 insert latency and PUTs per part, and the recommendation is recorded in the task
- [ ] #3 If the window helps, a follow-up task with the chosen size is filed; if not, the task closes with the numbers
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

Identifier trace: the earliest docs mention of `flushRefBatch` is 2026-07-12 (e1e76106f40); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
