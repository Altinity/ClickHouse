---
id: CAS-219
title: >-
  Capture Information-level `text_log` rows of the CAS loggers in
  `predown_dump.sh`, bounded by window and row cap
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'area:tooling'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-7
dependencies: []
references:
  - utils/ca-soak/scripts/predown_dump.sh
documentation:
  - docs/superpowers/cas/2026-08-03-stage-b-RESULTS.md
priority: low
type: chore
ordinal: 277000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`predown_dump.sh` extracts only error-shaped `text_log` rows (`utils/ca-soak/scripts/predown_dump.sh:164-177`). In the Stage-B soak the GC round's
Information line explaining why destructive work was suppressed was lost at teardown; only the structured phase rows survived.
An unbounded dump would become the next oversized artifact, so bound it.

Provenance: BACKLOG/testing-and-ci.md#soak-predown-textlog-scope; verified 2026-09-26 against 8b87aa15d21. Related: CAS-96 (window).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The dump contains Information-level rows from the CAS loggers inside the scenario window, capped by a row limit
- [ ] #2 The cap and window are recorded in the dump's `manifest.txt`
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
First recorded: 2026-08-04 (3039d720dcf, by 'soak-predown-textlog-scope')
<!-- SECTION:NOTES:END -->
