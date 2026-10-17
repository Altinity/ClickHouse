---
id: CAS-74
title: >-
  Redo the S3 keep-alive A/B spike over a non-loopback path at the #2243
  reporter's rate
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-10-01 17:03'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:issue'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2243'
  - src/IO/S3Defines.h
  - docs/en/antalya/cas/configuration.md
documentation:
  - docs/superpowers/specs/2026-09-05-cas-connection-churn-design.md#open-points
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f27
priority: medium
type: spike
ordinal: 99000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The 2026-09-05 spike found the 100-request keep-alive rotation dominant and `http_keep_alive_max_requests=10000` cut `DiskConnectionsCreated` ~100x, but its baseline did not reproduce port pressure: zero `EADDRNOTAVAIL`, TIME_WAIT peak ≤ 11.9% of the range, RustFS on loopback with `tcp_tw_reuse=2`, one to two orders of magnitude below the reporter's ~430 GET/s.
The recommendation (`http_keep_alive_timeout=30`, `http_keep_alive_max_requests=10000`) shipped as operator config in `docs/en/antalya/cas/configuration.md`, not as a code default; the S3 default stays 100 (`src/IO/S3Defines.h:13`).
Audit F27 on the otel stand: one new TLS connection per ~98 requests, 109k per day, matching the default of 100. Check the stand's disk config first; it may simply lack the recommendation.
A valid redo settles `mounts-and-lifecycle.md#issue-2243-port-exhaustion-lease`.

Provenance: BACKLOG/performance.md#cas-connection-churn-spike-redo; roadmap §2 'Connection churn (verify)'. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The otel stand's effective `http_keep_alive_max_requests` is recorded, and F27 is attributed to the client cap or to S3 `Connection: close`
- [ ] #2 A non-loopback run at ~430 GET/s reproduces port pressure at the default and shows whether 10000 removes it
- [ ] #3 The result says whether the recommendation becomes a code default for CAS disks
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
2026-10-01: occurrence on PR #2463 (cas_selects release + aarch64): 2620/2632 'Cannot assign requested address' on S3 PUTs, fenced after 67 attempts; tmp/ci-2463/rca.md
<!-- SECTION:NOTES:END -->
