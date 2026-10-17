---
id: CAS-208
title: >-
  Run up to 20 production mutants against the `CAS*` gate and turn each survivor
  into a test
status: To Do
assignee: []
created_date: '2026-09-03'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:testing'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - utils/cas-gate/run_cas_gate_per_suite.sh
documentation:
  - docs/superpowers/cas/2026-09-03-test-vacuity-audit.md
  - docs/superpowers/specs/2026-09-02-cas-backend-token-contract-design.md
priority: low
type: task
ordinal: 265000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The request-contract migration rewrote most CAS unit tests. The static vacuity audit (`docs/superpowers/cas/2026-09-03-test-vacuity-audit.md`)
ranked tests DISCRIMINATING / WEAK / TAUTOLOGICAL with a killing mutant each; the dynamic half was never run (deferred by the user 2026-09-03).
A mutant no test kills is a hole in the suite. Procedure: one-line production mutants only, one incremental `build_debug` build and one
`CAS*` run each, revert and check `git status --porcelain -- src` is empty before the next. Cost: ~3-4 hours of a cheap agent, sequential.
An abort in the debug build counts as killed by an assertion.

Provenance: BACKLOG/testing-and-ci.md#cas-unit-test-mutation-battery; verified 2026-09-26 (no battery report exists).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A table records each mutant, the tests expected to go red, the actual reds, and KILLED / KILLED-BY-OTHERS / SURVIVED
- [ ] #2 Each SURVIVED mutant has a new test that kills it
- [ ] #3 The report confirms or refutes each TAUTOLOGICAL or WEAK verdict of the static audit, and a final clean `CAS*` gate proves the tree is restored
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
First recorded: 2026-09-03 (9add32a2e9f, by 'cas-unit-test-mutation-battery')
<!-- SECTION:NOTES:END -->
