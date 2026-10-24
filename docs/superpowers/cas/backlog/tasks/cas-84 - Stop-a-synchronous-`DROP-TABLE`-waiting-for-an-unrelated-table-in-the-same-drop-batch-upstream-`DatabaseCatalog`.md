---
id: CAS-84
title: >-
  Stop a synchronous `DROP TABLE` waiting for an unrelated table in the same
  drop batch (upstream `DatabaseCatalog`)
status: To Do
assignee: []
created_date: '2026-09-16'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-5
dependencies: []
references:
  - src/Interpreters/DatabaseCatalog.cpp
documentation:
  - docs/superpowers/cas/2026-09-16-msan-cas-s3-shard-budget-rca.md
priority: medium
type: upstream
ordinal: 119000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`DatabaseCatalog::dropTableDataTask` (`src/Interpreters/DatabaseCatalog.cpp:1649`) takes the current batch, runs
`dropTablesParallel` (`:1591`), waits for the whole batch (`runner.waitForAllToFinish`), and only then reschedules. A table
enqueued while a batch runs waits for the batch's slowest drop whatever `database_catalog_drop_table_concurrency` allows.
With `database_atomic_wait_for_drop_and_detach_synchronously = 1` every `DROP TABLE` blocks on that. Measured on the msan
CAS shard: `tab_00718` waited 6 min for another table's 379 s drop; a `File` table with nothing to delete waited 5 min 27 s;
13 per-test timeouts had `DROP TABLE` as the slowest statement (25-564 s). Faster per-table drop does not help the next table.
Fix shape: reschedule the task while a batch is in flight, or drop per table from `enqueueDroppedTableCleanup` when
`ignore_delay`. Portable and useful outside CAS; grade as an upstream patch (compact, motivated, portable).

Part-commit round trips (from performance.md `#stateless-lane-wall-time-is-drop-table`, target 2): on the 2026-09-04 CA-s3 lane, query-thread Real samples were 25% in `TaskTracker::waitAll` under `fanOutBlobUploads`/`commit`/`moveDirectory` and 14% in `finalizeConditionalWrite`; background drop workers sat in a futex in `DatabaseCatalog::dropTableDataTask`, not in S3. S3 calls in 10 min: GET 11,095, HEAD 2,966, PUT 1,597, LIST 206, DELETE 25.

Provenance: BACKLOG/gc.md#drop-path-head-of-line-and-repoint-ramp item 1. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (identical).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test: a synchronous drop of table B enqueued while table A's slow drop runs returns without waiting for A
- [ ] #2 Drop concurrency limits and retry-on-error behaviour are unchanged
- [ ] #3 The patch is prepared against upstream master
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
First recorded: 2026-09-16 (cbf3fe14e36, by 'drop-path-head-of-line-and-repoint-ramp')
<!-- SECTION:NOTES:END -->
