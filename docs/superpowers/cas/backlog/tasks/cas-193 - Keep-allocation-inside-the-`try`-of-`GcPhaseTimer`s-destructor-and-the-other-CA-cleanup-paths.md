---
id: CAS-193
title: >-
  Keep allocation inside the `try` of `GcPhaseTimer`'s destructor and the other
  CA cleanup paths
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:gc'
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - R/Gc/CasGcPhaseTimer.h
  - R/Backend/CasProbe.cpp
  - R/Pool/CasMountRuntime.cpp
priority: low
type: chore
ordinal: 250000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`~GcPhaseTimer` (`R/Gc/CasGcPhaseTimer.h:54-75`) builds a `GcPhaseRecord`, moves a `std::map` and emplaces per-counter `String`s outside its one `try`, which wraps only the sink call.
The same class exists in `CasProbe.cpp`'s cleanup lambdas, `CasMountRuntime.cpp`, and a fail-closed branch of `CasRefLedger.cpp`.
Wrap or pre-size; no behaviour change. The sibling with a real terminate path is `noexcept-ref-drop-erase-view-inside-try`.

Provenance: BACKLOG/ref-protocol.md#noexcept-allocation-hardening residual (2031-triage CAS-018). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No allocation in these destructors and cleanup lambdas runs outside a `try`
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
First recorded: 2026-08-21 (d8c5e8de79a, by 'noexcept-allocation-hardening')
<!-- SECTION:NOTES:END -->
