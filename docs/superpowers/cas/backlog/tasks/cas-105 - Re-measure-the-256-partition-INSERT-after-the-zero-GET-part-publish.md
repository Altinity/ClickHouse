---
id: CAS-105
title: Re-measure the 256-partition INSERT after the zero-GET part publish
status: To Do
assignee: []
created_date: '2026-07-06'
updated_date: '2026-09-26 14:38'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:soak'
milestone: m-2
dependencies:
  - CAS-176
references:
  - src/Storages/MergeTree/ReplicatedMergeTreeSink.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
priority: low
type: research
ordinal: 143000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A 256-partition INSERT took ~10 s (~40 ms per part), because each partition's part is published serially.
Concurrent per-partition commit (stage 2) is postponed by user decision (`DRAFT-2`); the zero-GET publish (decision-5, audit F31) cuts per-part latency and is scheduled.
Measure what is left after it before reopening stage 2.

Provenance: BACKLOG/performance.md#scale-findings [partitioned-INSERT O(partitions)]. Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Wall time and per-part publish latency of a 256-partition INSERT are recorded before and after the zero-GET publish
- [ ] #2 The result is attached to the stage-2 draft as its go/no-go input
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
First recorded: 2026-07-06 (4abf5b743ed, by 'partitioned-INSERT O')

Merged from u19b-soak (merge into CAS-105): From the soak ledger's partitioned-INSERT entry (2026-07-06, S11 full, 256 buckets): 4-5 INSERTs per minute. Fix direction recorded then: when the parts of one INSERT share a namespace, publish them in one combined flush; check whether the ref-lane combine window (CAS-287) already covers multi-partition INSERTs before reopening stage 2.
<!-- SECTION:NOTES:END -->
