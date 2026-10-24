---
id: CAS-127
title: 'Find why CA-S3 lanes hold over 10,000 concurrent Disk-group HTTP sessions'
status: To Do
assignee: []
created_date: '2026-07-27'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:backend'
  - 'area:ci'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - src/Common/HTTPConnectionPool.cpp
priority: low
type: research
ordinal: 165000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On the ASan CA-s3 lane (`e2d04bfe37e`), `00149_quantiles_timing_distributed` failed on a leaked warning
`ConnectionGroup: Too many active sessions in group Disk, count 10400, warning limit 8000` (`src/Common/HTTPConnectionPool.cpp:251`).
Output was correct and reruns passed, but 10k concurrent sessions is a pressure signal. Distinct from connection churn
(audit F27, a creation rate, `u05-perf-b` connection-churn task): this is a concurrent count. Check whether CAS holds sessions
across retry backoffs before raising any limit.

Provenance: BACKLOG/operability-and-introspection.md#ca-s3-disk-session-pressure; verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A CA-s3 stateless run records the peak Disk-group session count and compares it with the plain-S3 lane
- [ ] #2 A recorded conclusion says whether CAS holds sessions longer than needed, with the call site if so
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
First recorded: 2026-07-27 (1258b8c2a30, by 'CA-s3 Disk session pressure')
<!-- SECTION:NOTES:END -->
