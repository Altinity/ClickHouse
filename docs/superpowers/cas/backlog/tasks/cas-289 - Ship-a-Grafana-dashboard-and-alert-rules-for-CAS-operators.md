---
id: CAS-289
title: Ship a Grafana dashboard and alert rules for CAS operators
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:observability'
  - 'complexity:epic'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-0
dependencies: []
references:
  - src/Storages/System/StorageSystemDashboards.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f25
  - docs/en/antalya/cas/operations/monitoring.md
priority: high
type: feature
ordinal: 361000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The first deployment blocks on monitoring and alerts (roadmap section 1). No dashboard exists in the repo; the only generic assets are
`programs/server/dashboard.html` and `system.dashboards` (`src/Storages/System/StorageSystemDashboards.cpp`).
Audit F25 lists the signals worth a panel from a week of canary data and the ones that mislead: lane wait and mutations per flush,
GC stage flow (condemned, graduated, redeleted), LIST rate, the memory saw-tooth per GC round, mount renewals, conditional-write
unresolved rate. It also names what not to use: `S3ReadRequestsErrors` counts the protocol's HEAD-before-PUT misses.
Two parts: the board and the alert rules. CAS-4 maps the alert signals onto existing surfaces.

Provenance: umbrella-roadmap.md section 3 bullet 'Grafana dashboard'; filed 2026-09-26 (user decision). Related: CAS-4, CAS-2. Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A dashboard JSON and alert rules are in the repo and load in Grafana against the ClickHouse data source
- [ ] #2 `operations/monitoring.md` links both and explains each panel and alert
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

Identifier trace: the earliest docs mention of `S3ReadRequestsErrors` is 2026-06-15 (6e28ecaaed5); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
