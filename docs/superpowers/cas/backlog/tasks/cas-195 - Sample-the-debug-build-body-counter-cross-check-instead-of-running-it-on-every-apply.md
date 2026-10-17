---
id: CAS-195
title: >-
  Sample the debug-build body-counter cross-check instead of running it on every
  apply
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - R/Pool/CasRefProtocol.cpp
priority: low
type: chore
ordinal: 252000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`RefTableState::debugAssertBodyCounters` recomputes body totals and `owned_manifests` from scratch on every `applyTxnInPlace` (`R/Pool/CasRefProtocol.cpp:585`) and every `admits` preview (`:730`) under `DEBUG_OR_SANITIZER_BUILD`.
That turns the in-place apply's O(K+N) back into O(K*N) in exactly the builds soak and correctness runs use. Release builds are unaffected.
Sample it: every `admits` preview, and `applyTxnInPlace` only on the first apply after an install or under a test-only flag. Do not delete the assert.

Provenance: BACKLOG/ref-protocol.md#debug-body-counter-assert-on-replay (2031-triage CAS-054). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A K-transaction replay in a debug build no longer runs the full cross-check per transaction; a test-only flag re-enables it
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
First recorded: 2026-08-21 (6e464e597ed, by 'debug-body-counter-assert-on-replay')
<!-- SECTION:NOTES:END -->
