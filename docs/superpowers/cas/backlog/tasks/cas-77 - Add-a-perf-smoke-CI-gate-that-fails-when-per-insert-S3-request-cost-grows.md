---
id: CAS-77
title: Add a perf-smoke CI gate that fails when per-insert S3 request cost grows
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:ci'
  - 'area:testing'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - CAS-17
  - ci/defs/job_configs.py
  - 'theme:parity'
dependencies:
  - CAS-17
references:
  - ci/defs/job_configs.py
priority: low
type: feature
ordinal: 102000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Each write-path cost was measured and accepted alone (`_ckpt` +4 round trips, O(namespaces) frontier probes per GC round, `HEAD`-before-`PUT` 44% of round requests), but the combined cost of one insert is not tracked as a number that can regress. The 1.59x stage-1 baseline predates `_ckpt`.
Proposed: a small lane (single insert, wide insert, quiet-GC-round request budget) run as a gate that flags "cost grew N% since the last baseline", counting S3 requests per operation rather than wall time.

Provenance: BACKLOG/performance.md#perf-smoke-cost-regression-gate (source doc random/retrospection-archeology.md deleted by u22). Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A CI job records S3 requests per operation for single insert, wide insert and a quiet GC round
- [ ] #2 The job fails when any figure exceeds its stored baseline by a configured percentage
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
First recorded: 2026-09-26 (93022c446eb, by 'perf-smoke-cost-regression-gate')
<!-- SECTION:NOTES:END -->
