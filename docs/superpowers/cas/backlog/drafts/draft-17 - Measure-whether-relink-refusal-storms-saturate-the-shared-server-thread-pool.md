---
id: DRAFT-17
title: Measure whether relink refusal storms saturate the shared server thread pool
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:replication'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:measurement'
  - 'needs:repro'
  - 'origin:issue'
dependencies:
  - CAS-123
references:
  - base/poco/Net/src/TCPServerDispatcher.cpp
  - src/Common/AsynchronousMetrics.cpp
priority: low
type: research
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
One `Poco::ThreadPool` capped at `max_connections` serves 8123, HTTPS, native and 9009, while `_currentThreads` is
per-dispatcher, so a pool saturated by long interserver byte fetches can make the 8123 dispatcher drop accepted sockets
with no ClickHouse error (upstream comment, `base/poco/Net/src/TCPServerDispatcher.cpp:152-160`). Relink refusals force byte
fetches, so a refusal storm converts cheap transfers into thread-holding ones. The rejected counts are already exposed
(`HTTPRejectedConnections`, `InterserverRejectedConnections`, `src/Common/AsynchronousMetrics.cpp:2536-2566`). Upstream file:
consult first before any change.

Provenance: BACKLOG/operability-and-introspection.md#issue-2233-followups item (3); the expose-counts ask is implemented upstream (243edcc8aa6); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A soak with a forced refusal storm records the rejected-connection metrics and interserver thread counts
- [ ] #2 A recorded conclusion says whether an alert or an upstream change is needed
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

Identifier trace: the earliest docs mention of `max_connections` is 2026-07-02 (3a054b9ffe6); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
