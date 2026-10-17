---
id: CAS-191
title: Build `RefTableState` from the decoded snapshot without re-encoding it
status: To Do
assignee: []
created_date: '2026-07-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - R/Pool/CasRefProtocol.cpp
  - R/benchmarks/benchmark_cas_ref_protocol.cpp
priority: low
type: enhancement
ordinal: 248000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`stateFromSnapshot` (`R/Pool/CasRefProtocol.cpp:431-434`) encodes the snapshot and decodes it again as a hand-built defense, although `decodeRefTableSnapshot` already produces a validated value.
Per-row size helpers then re-encode each row a third time. Measured estimate: a 2-3x cut of recovery and GC-rebuild decode time.
Accept the validated-witness type instead of round-tripping.

Provenance: BACKLOG/ref-protocol.md#ref-ledger-consult-followups-2026-07-21 (codec passes). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792 (encode at :433 there).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `stateFromSnapshot` performs no encode; recovery benchmarks show the saving
- [ ] #2 Existing snapshot corruption tests still reject the same inputs
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
