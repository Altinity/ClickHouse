---
id: CAS-122
title: Name the errno and operation in the soak driver's `TRANSPORT FAILURE` verdict
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
dependencies: []
references:
  - utils/ca-soak/soak/run.py
  - 'https://github.com/Altinity/ClickHouse/issues/2233'
priority: low
type: enhancement
ordinal: 160000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`utils/ca-soak/soak/run.py:1720-1729` and `:2026-2035` label every `OSError` "TRANSPORT FAILURE", naming a subsystem nothing
diagnosed (the same triage misdirection #2219 complains about). Phase 1 makes one attempt (`transport_resilient=False`,
`:251`, `:357`), and its checkpoints do not wait for HTTP health (`wait_for_healthy` is gated on `phase2`, `:1947-1948`), so
any transient `OSError` becomes issue #2233's exact headline.

Provenance: BACKLOG/operability-and-introspection.md#issue-2233-followups item (2); verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The failure verdict names the errno, the replica and the operation that failed
- [ ] #2 Phase-1 checkpoints wait for HTTP health, or the report states why not
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

Identifier trace: the earliest docs mention of `OSError` is 2026-08-03 (19c3018767a); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
