---
id: CAS-29.4
title: >-
  Read the graduation gate's condemn markers through the fold read-ahead (spec
  A1)
status: To Do
assignee: []
created_date: '2026-09-03'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-29.2
  - CAS-29.3
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGcReadAhead.h
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#a1-graduation-gate
  - docs/superpowers/worklogs/2026-09-04-cas-gc-fold-read-ahead-measurement.md
parent_task_id: CAS-29
priority: high
type: enhancement
ordinal: 110000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`confirm_condemned_marker` (`CA/Gc/CasGc.cpp:1927`, inside `Gc::fold`) issues one synchronous `loadMeta` per carried
condemned entry without an in-process confirmation. It is the last serial read of `fold_reduce`: with the zero-in-degree
HEADs read ahead, `fold_reduce` improves only ~1.2x against a fixed per-request latency (2026-09-04 read-ahead measurement).
Candidates are the condemned rows of the adopted run, streamed by the merge in hash order, so the hint site is a lookahead
on the run cursor: keep the next `window()` meta keys hinted and let the gate `takeRead` instead of `op.readUnder`.
`condemnMarkerConfirmedInProcess` stays a shortcut, never required (a new leader or a multi-node executor has no memo).
Missing, unreadable or non-`Condemned` meta is handled exactly as today (`CASGCCondemnMarkerUnconfirmedCarry`, marker retry, no graduation).

Provenance: BACKLOG/gc.md#gc-reduce-confirm-marker-read-ahead. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A fresh `Gc` over a run with N condemned rows: meta reads overlap at concurrency 4 and peak at 1 at concurrency 1
- [ ] #2 Outcomes equal the sequential run's; an unreadable meta yields `CASGCCondemnMarkerUnconfirmedCarry` and a marker rewrite, never a graduation
- [ ] #3 On a stand with at least 100k condemned rows and an empty memo, `fold_reduce` wall time scales with `cas_gc_concurrency`
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
First recorded: 2026-09-03 (db5bb3485c8, by 'gc-reduce-confirm-marker-read-ahead')
<!-- SECTION:NOTES:END -->
