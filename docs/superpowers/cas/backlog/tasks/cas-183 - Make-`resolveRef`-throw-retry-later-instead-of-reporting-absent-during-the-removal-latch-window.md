---
id: CAS-183
title: >-
  Make `resolveRef` throw retry-later instead of reporting absent during the
  removal-latch window
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:ref-ledger'
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-8
dependencies: []
references:
  - R/Pool/CasRefLedger.cpp
priority: medium
type: bug
ordinal: 240000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Inside the removal-latch window `acquireReadableRefTableRuntime` returns `nullptr` (`R/Pool/CasRefLedger.cpp:621-623`, when `removal_admission_closed`), and `resolveRef` renders that as "no such ref" (`:293-299`).
`appendRefOps` throws the retry-later class for the same state. A reader sees "absent" for "temporarily not admitting".
Distinguish the latch from a namespace the catalog does not name, which should keep returning absent.

Provenance: BACKLOG/ref-protocol.md#lane-residuals-2031-cas-017 residual 1. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792 (latch check at :627 there).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A resolve during the latch window throws the retry-later class; a resolve of an unknown namespace still returns absent (gtest for both)
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
First recorded: 2026-08-21 (7a9b839cd49, by 'lane-residuals-2031-cas-017')

2031-triage CAS-017: residual 1 (read path answers absent instead of retry-later while `dropNamespace` holds the admission latch).
<!-- SECTION:NOTES:END -->
