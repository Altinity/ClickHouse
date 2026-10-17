---
id: DRAFT-23
title: >-
  Add a CI check that fails when an `IStorage` virtual is neither forwarded by
  `StorageProxy` nor allowlisted
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:ci'
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies:
  - DRAFT-18
references:
  - src/Storages/StorageProxy.h
  - src/Storages/IStorage.h
priority: low
type: feature
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If the feature stays. The audit found no compile-time guard for "new `IStorage` virtual not forwarded", which is how the
class keeps recurring. Proposal: a Clang-AST check comparing virtual `IStorage` declarations with `StorageProxy` overrides,
with an allowlist that requires a rationale per omission. Not present in `utils/check-style` or CI on either branch.

Provenance: BACKLOG/operability-and-introspection.md#lazy-load-tables-decision-2026-07-21 ([storageproxy-ast-interface-guard]); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Adding a virtual to `IStorage` without a forward or an allowlist entry fails CI
- [ ] #2 Every current omission is allowlisted with a one-line reason
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
First recorded: 2026-08-04 (f08734d17df, by 'storageproxy-ast-interface-guard')
<!-- SECTION:NOTES:END -->
