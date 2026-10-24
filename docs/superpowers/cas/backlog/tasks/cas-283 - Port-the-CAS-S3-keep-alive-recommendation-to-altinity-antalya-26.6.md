---
id: CAS-283
title: Port the CAS S3 keep-alive recommendation to altinity/antalya-26.6
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:backend'
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:issue'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-0
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2243'
  - docs/en/antalya/cas/configuration.md
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
priority: high
type: chore
ordinal: 350000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`05e7fc2bdee` (cas-gc-rebuild) recommends `http_keep_alive_timeout = 30` and `http_keep_alive_max_requests = 10000` on every CAS S3 disk: in `docs/en/antalya/cas/configuration.md`, in the stateless-lane CAS storage policy and in every `test_cas_*` disk config. It is not on `altinity/antalya-26.6`.
With the generic S3 default of 100 requests per connection, a connection is torn down every ~100 CAS requests and its port cycles through `TIME_WAIT`; under sustained load that exhausts ephemeral ports and starves the mount-lease renewal (commit message of `05e7fc2bdee`, issue #2243). Audit F27 measured one new TLS connection per ~98 requests, 109k per day, on the otel.demo stand.
The recommendation is operator config, not a code default; CAS-74 decides whether it becomes one.

Provenance: BACKLOG.md#inbox [cas-keep-alive-recommendation-backport], queued by the u11-specs-c grooming unit; verified 2026-09-26: `05e7fc2bdee` is not an ancestor of altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The docs section and the disk-config changes of `05e7fc2bdee` (or an equivalent) are on altinity/antalya-26.6
- [ ] #2 Every S3-backed CAS disk definition in the test and stateless-lane configs of altinity/antalya-26.6 sets both settings
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

Identifier trace: the earliest docs mention of `test_cas_*` is 2026-06-16 (518d2a1e67d); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
