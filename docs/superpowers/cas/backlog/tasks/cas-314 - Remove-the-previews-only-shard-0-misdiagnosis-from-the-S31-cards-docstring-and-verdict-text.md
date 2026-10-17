---
id: CAS-314
title: >-
  Remove the 'previews only shard 0' misdiagnosis from the S31 card's docstring
  and verdict text
status: To Do
assignee: []
created_date: '2026-09-26 14:44'
labels:
  - 'area:soak'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s28_s33_corner.py
priority: low
type: chore
ordinal: 393000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`previewDeletes` scans every blob-target shard (`CA/Gc/CasGc.cpp:4629-4636`, since `5f5fa5f7906`), and the S31 card's own
2026-07-18 RCA calls "previews only shard 0" a misdiagnosis (`utils/ca-soak/scenarios/cards/s28_s33_corner.py:593-600`).
The card still states the old claim in its module docstring (`:14-16`), in the "scale used" verdict's expectation (`:515`) and
in the comment before the dry run (`:555-556`), so a reader of a report sees a product defect that does not exist.

Provenance: utils/ca-soak/scenarios/BACKLOG.md#S31-20260713T174441-1 and #S31-20260718T001753-1 (both fixed; this is the leftover prose); verified 2026-09-26 against 66087be0ffb (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No text in `s28_s33_corner.py` says `previewDeletes` or `cas-gc-dryrun` covers only shard 0
- [ ] #2 The S31 verdict text states the same-instant comparison the card actually makes
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
