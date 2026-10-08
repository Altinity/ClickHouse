---
id: DRAFT-24
title: >-
  Design an expedited delete that erases a blob without waiting for GC
  graduation
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:speculative'
  - 'needs:spec'
  - 'needs:decision'
  - 'origin:review'
dependencies: []
documentation:
  - docs/superpowers/cas/AGENTS.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Right-to-erasure: under the GC lease, confirm no live ref names the blob, then delete it bypassing the two-phase graduation
delay; no layout change. Not implemented on either branch and not on the roadmap. It shortcuts the GC safety window, so any
design must show it cannot delete an object a committed reference still names (AGENTS invariant 1) and respect decision-1.

Provenance: BACKLOG/operability-and-introspection.md#b14-gdpr-erasure ([B14]); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An owner decides whether erasure is in scope
- [ ] #2 If yes, a spec proves the no-live-ref check is sound against in-flight writers and relinks
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
First recorded: 2026-09-26 (4afa7f68ee1, by 'b14-gdpr-erasure')
<!-- SECTION:NOTES:END -->
