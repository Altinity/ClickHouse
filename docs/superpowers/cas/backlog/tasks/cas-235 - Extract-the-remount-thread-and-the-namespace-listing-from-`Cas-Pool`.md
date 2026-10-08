---
id: CAS-235
title: 'Extract the remount thread and the namespace listing from `Cas::Pool`'
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: low
type: chore
ordinal: 300000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CA/Pool/CasPool.cpp` + `.h` = 3461 lines. The caches and the ref-log lane already moved to `CasManifestReader` and
`CasRefLedger`; the remount thread is still inline. Unclaimed small candidates: `listNamespaces` / `listMirroredChildren`
(`CasPool.h:618,624`, ~112 lines) and `reportImpossibleInterference` (`CasPool.h:933`; its former partner
`peekForeignRefLogHeader` no longer exists). Low value alone: the composition root is sound, the file stays dominated by
the mount protocol.

Provenance: BACKLOG/docs-and-cleanup.md#refactoring [refactor: Store de-god-classing] (formerly [source-layout-casstore-followups]); verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The remount thread and the namespace listing live outside `CasPool.cpp`, with no behavior change
- [ ] #2 The `CAS*` gtest gate is green on the committed tree after the move
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'refactor: Store de-god-classing')
<!-- SECTION:NOTES:END -->
