---
id: CAS-4
title: >-
  Map the GC alert signals onto existing surfaces and add only the uncovered
  ones
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:review'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies:
  - CAS-1
references:
  - src/Interpreters/ContentAddressedGarbageCollectionLog.h
  - docs/superpowers/cas/BACKLOG/gc.md
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - docs/superpowers/cas/umbrella-roadmap.md
  - docs/en/antalya/cas/operations/monitoring.md
priority: high
type: task
ordinal: 10000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Candidate GC signals filed 2026-08-04: heartbeat lag, B170 event classes, retired-list age, and an invariant alert.
Much may already be covered by `system.cas_gc_log`, `system.cas_mounts` and the `CASGC*` metrics; no GC-log column
was added since (`src/Interpreters/ContentAddressedGarbageCollectionLog.h:38-49`). Audit F25 now lists which signals are
worth a dashboard row and which are useless, and roadmap §3 names the alert set (lease lost, renewal retries, GC round
age, backlog growth, conditional-write unresolved rate, S3 5xx; not `S3ReadRequestsErrors`).
Do the overlap check first; only the fields no existing surface can answer are the real ask.
Round-duration watchdog and fold-window events are tracked separately in `gc.md` ("GC round progress observability").

Provenance: BACKLOG/operability-and-introspection.md#gc-observability-field-list (2026-08-04 orphan triage); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A table maps each candidate signal and each roadmap alert to an existing column, metric or event, or marks it uncovered
- [ ] #2 Every uncovered signal that an alert needs is added, with a test that makes it move
- [ ] #3 The mapping is published in `operations/monitoring.md` as the alert recipe
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
First recorded: 2026-08-04 (f08734d17df, by 'gc-observability-field-list')
<!-- SECTION:NOTES:END -->
