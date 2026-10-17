---
id: CAS-94
title: >-
  Bound restart time spent loading Outdated parts of system log tables on a CAS
  disk
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:soak'
  - 'origin:canary'
milestone: m-0
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2439'
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f1
  - docs/en/antalya/cas/configuration.md
priority: high
type: research
ordinal: 129000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A 6/40 soak restart took 178.9 s against a 180 s gate, 138.1 s of it reloading CAS log tables' Outdated parts.
On otel.demo `system.*` tables are 86% of all parts (audit F1). Log tables stay on CAS on the stand by decision-6 and on
the CI lanes by lane config, so the cost is real for any deployment that uses CAS as the default disk.
Directions: TTL or partitioning of `cas_log`, bounded churn, lazy load of Outdated parts, and a docs recommendation of a
local storage policy for `system.*` logs (roadmap §3 docs for operators).

Provenance: BACKLOG/gc.md#ca-log-tables-restart-cost; also cited by operability-and-introspection.md and performance.md#standalone-write-scratch-manifest-cost.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A measurement splits restart time by table and part state on a soak or stand node
- [ ] #2 The operator docs recommend a storage policy for `system.*` logs with the measured cost as the argument
- [ ] #3 If a code change is chosen, restart time on the same specimen drops below 60 s
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
First recorded: 2026-07-29 (e614330d78f, by 'CA-LOG-TABLES-RESTART-COST')
<!-- SECTION:NOTES:END -->
