---
id: CAS-224
title: >-
  Remove the last 36 test-frame by-reference captures from `PoolConfig` hooks in
  the pool and mount gtests
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-8
dependencies: []
references:
  - src/Disks/tests/gtest_cas_pool.cpp
  - src/Disks/tests/gtest_cas_mount.cpp
priority: high
type: bug
ordinal: 282000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
ASan caught a use-after-free in `gtest_cas_ref_writer.cpp` (fixed `a726e933419`): a `Pool` outlives its test frame through a background publish's
`shared_from_this()`, and deferred teardown calls a hook that captured a dead local by reference. Of the original 92 `_fn = [&` sites in 10 files,
36 remain: 18 in `T/gtest_cas_pool.cpp` and 18 in `T/gtest_cas_mount.cpp`, same on antalya. A known sanitizer defect class is fixed at every sibling site, not backlogged.
Each site needs a read: over half of the ref-writer sites needed shared state, not a by-value copy.
Alternative that removes the class: `Pool` teardown stops calling config hooks and snapshots the clock values it needs at construction.

Provenance: BACKLOG/testing-and-ci.md#poolconfig-hooks-capture-by-reference; counts re-measured 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No `PoolConfig` hook in CAS gtests captures a test-frame local by reference, or `Pool` teardown no longer calls config hooks
- [ ] #2 `gtest_cas_pool.cpp` and `gtest_cas_mount.cpp` pass 5 consecutive ASan runs
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
First recorded: 2026-09-26 (6cbfb916bcf, by 'poolconfig-hooks-capture-by-reference')
<!-- SECTION:NOTES:END -->
