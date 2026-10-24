---
id: CAS-18
title: Record a matched before/after benchmark of the mandatory blob HEAD
status: To Do
assignee: []
created_date: '2026-08-23'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:review'
dependencies:
  - CAS-17
references:
  - >-
    docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md
documentation:
  - >-
    docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md
priority: low
type: research
ordinal: 24000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The blob-publication protocol is fixed (2026-08-23, decision-1): one HEAD before every publish decision, no conditional creation, no metadata GET on a fresh miss. Its performance acceptance is still open: the only measurement is target-only, not a matched pair.
The otel.demo audit counts HEAD misses (F10: 7.8k/h, equal to `CASBlobHeadMiss`) but does not attribute blob HEAD/PUT latency (F30: fan-out threads unattributed).
This is an acceptance record only; the result cannot reopen the protocol.

Provenance: BACKLOG/performance.md#mandatory-blob-head-cost. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792. Must not contradict decision-1. Dependency on the wide-insert remeasurement is a shared-harness judgment call (same CA-default stateless harness and baseline binary), not stated in the source.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 One benchmark run on the pre-`940b1685bf96` binary and one on HEAD, same workload and backend, with wall time and request counts
- [ ] #2 The result is accepted by the owner and linked from the report
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
First recorded: 2026-08-23 (f8b6e8a3870, by 'mandatory-blob-head-cost')
<!-- SECTION:NOTES:END -->
