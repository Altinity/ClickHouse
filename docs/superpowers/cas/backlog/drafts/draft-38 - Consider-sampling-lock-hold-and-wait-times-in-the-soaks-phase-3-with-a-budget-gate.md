---
id: DRAFT-38
title: >-
  Consider sampling lock hold and wait times in the soak's phase 3 with a budget
  gate
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'origin:soak'
dependencies: []
references:
  - utils/ca-soak/scenarios/framework/sampler.py
priority: low
type: enhancement
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The phase-3 sampler records memory, pool size and container samples only (`utils/ca-soak/scenarios/framework/sampler.py:15-16`).
A lock-hold/wait series with a budget gate was proposed, but no lock was named and no measured need exists. Promote once a lock-contention
question arises that the existing profiles cannot answer.

Provenance: BACKLOG/testing-and-ci.md [soak-lock-hold-wait-metric]; verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The locks to sample and the source of the numbers are named before this is promoted
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
First recorded: 2026-08-04 (f08734d17df, by 'soak-lock-hold-wait-metric')
<!-- SECTION:NOTES:END -->
