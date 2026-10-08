---
id: CAS-299
title: >-
  Count S3 requests that failed after all retries separately from
  retried-and-recovered ones (#2397)
status: To Do
assignee: []
created_date: '2026-09-26 12:37'
updated_date: '2026-09-30 22:10'
labels:
  - 'area:observability'
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:issue'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2397'
  - src/IO/ReadBufferFromS3.cpp
  - src/Common/ProfileEvents.cpp
priority: high
type: upstream
ordinal: 378000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On a healthy 2-replica production cluster (AWS us-east-1, 24.6 h) `system.errors` showed 118,692 / 31,494 `S3_ERROR` (code 499). A
review read them as lost compare-and-swap races; the real lost-race count was 784 / 1,390 (0.04-0.09%), 23x fewer.
`S3_ERROR` tracks `ReadBufferFromS3RequestsErrors` (118,389 / 28,610); `S3SingleAttemptRetryConsultations` equals the read errors, so
they were retried and recovered. `system.errors` keeps only `last_error_message` (`PreconditionFailed` on one replica, `Timeout` on the
other) and `text_log` has no matching lines, so nothing an operator can read separates "hiccup" from "failed".
Ask (core, not CAS): a counter for S3 requests that ultimately failed after retries, per verb, and a documented meaning for
`S3_ERROR` in `system.errors`. `S3ReadRequestsThrottling` (1,637 on one replica) was a real throttling signal buried in the same mass.
Explicitly not requested: lowering log levels. The CAS half (which counter shows contention) is an AC on CAS-2.

Provenance: GitHub issue #2397 requests 1-2 (open), reconciled by u17-github-cas 2026-09-26. Request 3 (CAS contention signal) → add_ac on CAS-2; 412/409 log noise → CAS-168, CAS-175. Reporter's cluster is a real deployment (Boris), so origin:canary and the High floor apply.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A ProfileEvent counts S3 requests that failed after the retry strategy gave up, separately from per-attempt errors, for reads and writes
- [ ] #2 A test with transient 503s that recover leaves the new counter at zero while the per-attempt error counters move
- [ ] #3 The `system.errors` documentation states that `S3_ERROR` counts occurrences including recovered attempts
- [ ] #4 The change is shaped as an upstream pull request with a motivation outside CAS
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
Blocks CAS-289.3 (CAS Grafana dashboard v2): see docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md §8.
<!-- SECTION:NOTES:END -->
