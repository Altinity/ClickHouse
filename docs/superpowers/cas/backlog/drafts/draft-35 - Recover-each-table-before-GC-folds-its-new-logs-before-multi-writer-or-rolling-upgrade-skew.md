---
id: DRAFT-35
title: >-
  Recover each table before GC folds its new logs, before multi-writer or
  rolling-upgrade skew
status: Draft
assignee: []
created_date: '2026-07-21'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'origin:review'
dependencies: []
references:
  - R/Gc/CasGc.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Defense in depth: run `recoverRefTable(ns)` per table before folding its new logs; `CORRUPTED_DATA` clamps that table (no cursor advance) instead of aborting the round.
Refuted as a live defect: a single lease holder cannot mint the fabricated history this would catch. Today the rebuild path alone calls the recovery (`R/Gc/CasGc.cpp:4384`).
Mandatory before any multi-writer or rolling-upgrade-skew milestone.

Provenance: BACKLOG/ref-protocol.md#ref-ledger-consult-followups-2026-07-21 (GC per-table recovery gate). Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Promoted to a task when a multi-writer or rolling-upgrade milestone is planned
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
First recorded: 2026-07-21 (cf79e86b970, by 'ref-ledger-consult-followups-2026-07-21')
<!-- SECTION:NOTES:END -->
