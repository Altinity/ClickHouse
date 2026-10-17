---
id: CAS-20
title: Bound concurrent ref-snapshot publishes pool-wide
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - docs/superpowers/cas/2031-triage.md#cas-051
priority: low
type: enhancement
ordinal: 26000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The single-in-flight gate on background snapshot publishes is per table: `CasRefLedger::admitSnapshotPublishUnderStateLock` (`R/Pool/CasRefLedger.cpp:4140`) checks `pending_snapshot_publishes == 0` on one runtime. Each admitted publish spawns a detached `ThreadFromGlobalPool` (`Pool::tryDispatchDetached`, `R/Pool/CasPool.cpp:1038`).
An ingest wave crossing the trigger (256 log entries / 1 MiB) on N tables starts N concurrent whole-namespace re-encodes at once. Fail-soft (retried on the next trigger, per-table backoff), not a correctness item.
A claimed pending-count leak here does not exist (closed by `829ad698ef6`).

Provenance: BACKLOG/performance.md#snapshot-publish-fanout-unbounded (2031-triage CAS-051). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A pool-wide limiter under the per-table gate caps concurrent snapshot publishes at a configured number
- [ ] #2 A gtest with N tables over threshold observes at most the cap in flight and all tables eventually published
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
First recorded: 2026-08-21 (b2a38f0b611, by 'snapshot-publish-fanout-unbounded')
<!-- SECTION:NOTES:END -->
