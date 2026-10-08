---
id: CAS-123
title: >-
  Count relink-confirm outcomes on the receiver and tell refusal from transport
  failure
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:replication'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-7
dependencies: []
references:
  - src/Storages/MergeTree/DataPartsExchange.cpp
  - src/Common/ProfileEvents.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2233'
priority: medium
type: enhancement
ordinal: 161000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The sender now counts refusals per reason (`CASRelinkConfirmRefused*`, `src/Common/ProfileEvents.cpp:817-821`, `5740d2a2953`),
but there is no counter for proven confirms, and the receiver collapses a refusal, a transport failure and a timeout into one
`NO_REPLICA_HAS_PART` (`src/Storages/MergeTree/DataPartsExchange.cpp:1611-1633`); the sender logs the reason at Debug
(`:294`, `:308`). Issue #2233's logs could not distinguish them even in principle; with these counters its adjudication would
have taken minutes. Keep `DataPartsExchange.cpp` changes minimal (shared file).

Provenance: BACKLOG/operability-and-introspection.md#issue-2233-followups item (4), carrying [relink-confirm-busy-lane] (b)/(c); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Receiver-side ProfileEvents count proven, refused and undelivered confirms separately
- [ ] #2 The receiver's `NO_REPLICA_HAS_PART` message says whether the source refused or the confirm was not delivered
- [ ] #3 `operations/monitoring.md` lists the new events
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
First recorded: 2026-07-29 (a7bb8510f36, by 'relink-confirm-busy-lane')
<!-- SECTION:NOTES:END -->
