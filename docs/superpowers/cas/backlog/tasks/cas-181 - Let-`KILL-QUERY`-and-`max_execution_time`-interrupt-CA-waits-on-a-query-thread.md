---
id: CAS-181
title: Let `KILL QUERY` and `max_execution_time` interrupt CA waits on a query thread
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:read-path'
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - R/Parts/PartFolderAccess.cpp
  - R/Pool/CasRefLedger.cpp
priority: medium
type: enhancement
ordinal: 238000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every CA wait (ref-lane single flight, leader election, namespace recovery, part-folder single flight) is bounded by the I/O under it: 16 attempts / 90 s per request, 120 s `recovery_retry_budget_ms`, or a fence loss.
None polls query cancellation: `grep isCancelled|QueryStatus|checkTimeLimit` over the CA tree is empty on both branches. A query parked behind a slow leader cannot be killed, and several bounded waits in a row add up to minutes.
Fix: thread the standard cancellation poll into waits that run on a query thread; read path and single flight first, ledger waits after.

Provenance: BACKLOG/ref-protocol.md#no-query-cancellation-checks (2031-triage CAS-015). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A query blocked in part-folder single flight or a ref-lane wait returns `QUERY_WAS_CANCELLED` within one poll interval of `KILL QUERY` (integration or gtest)
- [ ] #2 Background (non-query) waits keep their current bounds
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
First recorded: 2026-08-21 (7fd5127203d, by 'no-query-cancellation-checks')
<!-- SECTION:NOTES:END -->
