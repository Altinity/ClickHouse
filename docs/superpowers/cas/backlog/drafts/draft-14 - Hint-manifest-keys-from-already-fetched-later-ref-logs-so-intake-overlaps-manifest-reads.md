---
id: DRAFT-14
title: >-
  Hint manifest keys from already-fetched later ref logs so intake overlaps
  manifest reads
status: Draft
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:spec'
  - 'origin:soak'
  - 'origin:canary'
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGcReadAhead.h
documentation:
  - docs/superpowers/worklogs/2026-09-04-cas-gc-fold-read-ahead-measurement.md
priority: high
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
With the fold read-ahead on, `fold_ref_intake` gains ~2.4x and stops: one manifest GET per ref log (83 against 83 in the
measured round) cannot overlap, because a manifest key is known only after its log is read and decoded.
Fix idea: a non-consuming peek on the read-ahead plus a speculative decode of an already-fetched later log, used only to
hint its manifest keys; the real decode, order and decisions stay where they are, and a failed speculative decode is discarded.
Not sized. Spec A4 (per-round manifest body memo) and the repoint elision cut the same GETs first; decide after both.

Provenance: BACKLOG/gc.md#gc-intake-manifest-edge-serial-chain.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 After A4 and the repoint elision land, a measurement shows whether manifest GETs still dominate intake; if not, close
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
First recorded: 2026-09-04 (1019b57c810, by 'gc-intake-manifest-edge-serial-chain')
<!-- SECTION:NOTES:END -->
