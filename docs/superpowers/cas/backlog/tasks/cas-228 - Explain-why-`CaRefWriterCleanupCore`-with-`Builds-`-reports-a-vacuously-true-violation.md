---
id: CAS-228
title: >-
  Explain why `CaRefWriterCleanupCore` with `Builds = {}` reports a vacuously
  true violation
status: To Do
assignee: []
created_date: '2026-07-30'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:review'
dependencies: []
references:
  - docs/superpowers/models/CaRefWriterCleanupCore.tla
documentation:
  - docs/superpowers/models/2026-07-30-empty-set-survey.md
priority: low
type: research
ordinal: 286000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The empty-entity-set survey reproduced a violation in `CaRefWriterCleanupCore` with `Builds = {}` whose counter-example is vacuously true.
The mechanism, most likely how `Spec`'s fairness composes when every fair action is permanently disabled, is not identified.
An unexplained violation in a gate model is a false red waiting to recur. Note for future gate models: eleven models have no entity set at all,
so "zero namespaces" is inexpressible there; a gate model needs a namespace set that none of the catalog/ref models has.

Provenance: BACKLOG/testing-and-ci.md#empty-set-survey-residues; verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The mechanism is identified and written into the survey document
- [ ] #2 The model either `ASSUME`s non-empty `Builds` or passes with it empty
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
First recorded: 2026-07-30 (d0d73d46835, by 'empty-set-survey-residues')
<!-- SECTION:NOTES:END -->
