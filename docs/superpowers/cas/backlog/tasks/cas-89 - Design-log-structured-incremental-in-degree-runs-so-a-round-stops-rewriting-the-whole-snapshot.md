---
id: CAS-89
title: >-
  Design log-structured incremental in-degree runs so a round stops rewriting
  the whole snapshot
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:gc'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:plausible'
  - 'needs:spec'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
dependencies:
  - CAS-29.10
references:
  - CA/Gc/CasBlobInDegree.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f29
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#c4-not-in-c
priority: high
type: design
ordinal: 124000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every round rewrites and re-reads the in-degree run, O(universe): 11-12 MiB per round on otel.demo, 125 MiB with a backlog,
1.0 to 2.6 GiB/day of snapshot writes (audit F29). It is the dominant remaining byte cost; streaming reads and
reference-parent runs are done, log-structured incremental runs are not. Stage C's short rounds pay it many more times per day,
and at 100M blobs it becomes the round's floor. Spec C4 defers a graduation-cursor variant.
A new run layout is an on-S3 format change: it needs a new format version and a compatibility path (decision-4).

Provenance: BACKLOG/gc.md#gc-snapshot-log-structured-runs. Roadmap §2 GC 'Log-structured snapshot runs (later)'.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec gives the run layout, compaction rule, recovery and the format-version upgrade path, reviewed to no MAJOR
- [ ] #2 The spec states the per-round byte cost as a function of delta size and the measured break-even pool size
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
First recorded: 2026-08-04 (b4420fe512a, by 'gc-snapshot-log-structured-runs')
<!-- SECTION:NOTES:END -->
