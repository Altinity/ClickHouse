---
id: CAS-132
title: >-
  Add an `outcome` column (`Success`/`NotALeader`/`Deferred`) to the `SYSTEM CAS
  GC RUN` result row
status: To Do
assignee:
  - '@k-morozov'
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-7
dependencies: []
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
  - src/Interpreters/ContentAddressedGarbageCollectionLog.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2211'
documentation:
  - docs/en/antalya/cas/operations/debugging.md
priority: medium
type: enhancement
ordinal: 171000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`SYSTEM CAS GC RUN` on a follower returns a success row with `acquired_lease=0` and zeros; the operator must infer
"not the leader" (issue #2211). The result schema (`contentAddressedGcRoundColumns`, `src/Interpreters/InterpreterSystemQuery.cpp:2358-2383`)
has `acquired_lease`/`deferred` but no outcome word, while `system.cas_gc_log` already has `outcome` with exactly
this vocabulary (`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:23,42`).
Decided 2026-08-21 (user): keep the quiet idempotent OK, no exception. A throwing follower would fail every
`ON CLUSTER` run under `distributed_ddl_output_mode=throw`, and a node cannot tell it is inside the DDL fan-out.
No-steal on manual runs stays (`74d67b85021`: two manual calls microseconds apart could fake a frozen incumbent).
The docs section `{#sql-gc-run}` (`docs/en/antalya/cas/operations/debugging.md:184`) never mentions the follower case.

Provenance: BACKLOG/gc.md#issue-2211-gc-run-follower-noop (fix bullet 1 and the docs bullet); decision record 2026-08-21 in the same section. The source calls the column `finish`; the log's column is `outcome`, reuse that name. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792; issue #2211 still OPEN.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SYSTEM CAS GC RUN` on a follower returns `outcome = 'NotALeader'`; on the leader `Success` or `Deferred`, using the same words as `cas_gc_log.outcome`
- [ ] #2 A stateless or integration test runs `GC RUN` on two replicas and checks both outcome words
- [ ] #3 `{#sql-gc-run}` states the leadership model in one sentence and lists the new column
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
First recorded: 2026-08-21 (50fa0d52a9f, by 'issue-2211-gc-run-follower-noop')

Issue #2211 is assigned to k-morozov (2026-09-26).
<!-- SECTION:NOTES:END -->
