---
id: CAS-218
title: >-
  Make S01's scratch high-water verdict able to fail and sample densely during
  `OPTIMIZE FINAL`
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s01_s02_huge_blob.py
  - utils/ca-soak/scenarios/framework/sampler.py
priority: low
type: bug
ordinal: 276000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S01's "scratch high-water" verdict is hard-coded `"pass"` whenever any sample exists (`utils/ca-soak/scenarios/cards/s01_s02_huge_blob.py:168-174`),
and the sampler runs every 30 s by default (`scenarios/framework/sampler.py:33`), so the `OPTIMIZE FINAL` spike can fall between samples.
A green that cannot go red is not evidence (AGENTS.md §2.11).

Provenance: BACKLOG/testing-and-ci.md [soak-harness-minors] (S01 sub-point); verified 2026-09-26 against 8b87aa15d21. Related: CAS-100.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The verdict compares the peak against a bound derived from the blob size and fails above it
- [ ] #2 Sampling during the merge is dense enough that a synthetic scratch spike of the merge's length is caught
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
First recorded: 2026-09-26 (5245fbc7a76, by 'soak-harness-minors')
<!-- SECTION:NOTES:END -->
