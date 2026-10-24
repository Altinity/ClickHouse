---
id: CAS-70.3
title: Measure the hot-key lane on ten minutes of the parallel CA-s3 stateless suite
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
dependencies:
  - CAS-70.2
references:
  - docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md
documentation:
  - docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md#task-7
parent_task_id: CAS-70
priority: medium
type: research
ordinal: 87000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Task 7 of the phase A plan is open: no after-measurement exists, and it is the gate for phase B (`hot-key-phase-b`).
Before-numbers (2026-09-04, ~10 jobs, pre-lane): `DROP TABLE` n=1436 p50 2.4 s p90 11.9 s max 34.7 s; `CREATE TABLE` p50 184 ms p90 391 ms; `INSERT` p50 253 ms p90 541 ms. `trace_log` Real: 44% of query-thread wait in `InterpreterDropQuery::executeToTable`; the server was not CPU-bound (~280 CPU vs 630k Real samples).
Run the RCA's workload on the merged tree and record `DROP`/`CREATE` percentiles, `PreconditionFailed` on `ref_catalog`, the `CASHotKey*`, `CASRequestConflictPause` and new catalog-pause deltas, and S3 operations per dropped table.
Phase B combining pays only if `CASHotKeyQueueWaitMicroseconds` per submission, not the write, dominates at p90.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (Task 7) and #stateless-lane-wall-time-is-drop-table (measurement and target 1). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A dated report records before/after `DROP TABLE` and `CREATE TABLE` percentiles and `ref_catalog` `PreconditionFailed` counts on the same lane and job count
- [ ] #2 The report states the p90 queue-wait versus write-time split per submission and the resulting go/no-go for combining
- [ ] #3 The report names S3 operations and catalog conflicts per dropped table
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
First recorded: 2026-09-04 (c1b1e01a5e1, by 'stateless-lane-wall-time-is-drop-table')
<!-- SECTION:NOTES:END -->
