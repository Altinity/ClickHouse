---
id: CAS-192
title: >-
  Benchmark `RefTableState` copies across precommit counts before any new
  copy-on-write container
status: To Do
assignee: []
created_date: '2026-07-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - R/Pool/CasRefProtocol.h
  - R/benchmarks/benchmark_cas_ref_protocol.cpp
priority: low
type: research
ordinal: 249000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`RefTableState::precommits` is a plain `std::set` (`R/Pool/CasRefProtocol.h:254`), deep-copied per scratch copy; it is bounded only by the ~64 MiB admission byte budget, not the 1,000-op cap.
Every "O(1) ~58 ns copy" benchmark used a one-precommit fixture (`benchmark_cas_ref_protocol.cpp:151-178`); only row-count sweeps exist.
Run a precommit-count sweep (1, 100, 10,000) first. CAS-78 (`CAS-78`) lists copy-on-write as an option and needs this number.

Provenance: BACKLOG/ref-protocol.md#ref-ledger-consult-followups-2026-07-21 (precommits std::set). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `benchmark_cas_ref_protocol.cpp` has a precommit-count sweep and its results are recorded
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
First recorded: 2026-07-21 (cf79e86b970, by 'ref-ledger-consult-followups-2026-07-21')
<!-- SECTION:NOTES:END -->
