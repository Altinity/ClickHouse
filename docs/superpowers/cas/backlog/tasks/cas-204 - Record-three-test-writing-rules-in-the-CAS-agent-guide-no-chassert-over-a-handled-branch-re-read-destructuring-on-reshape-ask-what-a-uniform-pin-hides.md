---
id: CAS-204
title: >-
  Record three test-writing rules in the CAS agent guide: no chassert over a
  handled branch, re-read destructuring on reshape, ask what a uniform pin hides
status: To Do
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:docs'
  - 'area:testing'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - docs/superpowers/cas/AGENTS.md
priority: low
type: docs
ordinal: 261000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three standing rules live only in the backlog file being retired; `docs/superpowers/cas/AGENTS.md` has none of them.
1. A reachable state that the code below handles must not also be `chassert`ed: picking both makes a branch exist in only half the builds
   (originating fix `16cb681aadf`, pinned by `CASAnomalyPolicy.NonReadyAtNewIdAllocationFaultsAndFailsClosed`).
2. When a function's return shape changes, grep every `auto & [` / `auto [` call site and read it; a reshape gets no compiler error.
3. After a uniform pin (for example the uniform catalog-admission pin), ask what the pin makes unconstructible; that case needs its own
   test built through the real path (example: `CASGCShardIncarnation.UncatalogedStreamLifeDefersWithoutInventingNamespace`).

Provenance: BACKLOG/testing-and-ci.md#rule-no-chassert-over-handled-branch, #rule-structured-binding-silent-rebind, #uniform-pin-removed-testability (rule); verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `AGENTS.md` §5 or §3 states each rule in one line with its example
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
First recorded: 2026-07-30 (ef195461a7a, by 'rule-no-chassert-over-handled-branch')
<!-- SECTION:NOTES:END -->
