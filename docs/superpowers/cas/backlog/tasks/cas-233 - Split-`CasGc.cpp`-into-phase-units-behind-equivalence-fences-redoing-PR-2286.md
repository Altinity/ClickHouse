---
id: CAS-233
title: 'Split `CasGc.cpp` into phase units behind equivalence fences, redoing PR #2286'
status: In Progress
assignee:
  - '@k-morozov'
created_date: '2026-07-13'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - 'https://github.com/Altinity/ClickHouse/pull/2286'
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: chore
ordinal: 298000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CA/Gc/CasGc.cpp` is 4861 lines (same on both branches): an 18-phase round in one file, the fold alone ~1,700 lines.
With `CasRefLedger.cpp` it is 17.2% of the 59,489 CA lines. Roadmap §5: mechanical extraction of contiguous regions into
explicit phase inputs/results and a durable cleanup queue; wire protocol untouched; no rewrite. PR #2286 (open) is the
earlier attempt; redo it on that basis. Keep `Gc` as orchestration only.
CAS-29 (rounds in minutes) is rewriting the same phases now; land the split between its stages, not across them.

Provenance: BACKLOG/docs-and-cleanup.md#refactoring [refactor: CasGc split] and #refactor-candidates-from-defects item 3; verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Golden-output equivalence tests for the round phases exist and pass before any code moves
- [ ] #2 `CasGc.cpp` holds only round orchestration; scan, reachability, deletion, cursor and budget live in their own units
- [ ] #3 The `CAS*` gtest gate and the equivalence tests are green on the committed tree after the move, with no on-S3 format change
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'refactor: CasGc split')

PR #2286 (open), branch cas/separate_phase_during_round, base antalya-26.6, +278/-209 in `CasGc.cpp`/`CasGc.h` only, no equivalence tests yet (AC #1).
<!-- SECTION:NOTES:END -->
