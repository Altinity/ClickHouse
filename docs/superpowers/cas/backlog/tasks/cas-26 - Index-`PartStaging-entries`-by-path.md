---
id: CAS-26
title: 'Index `PartStaging::entries` by path'
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:write-path'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - docs/superpowers/cas/2031-triage.md#cas-116
priority: low
type: chore
ordinal: 32000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`PartStaging::entries` is a plain vector (`R/ContentAddressedTransaction.h:139`); every stage/move upserts with `std::erase_if` over it (`.cpp:766`, `:999`, `:1644`), O(F^2) path compares per part.
Real but small: a 3000-file part costs single-digit ms of string compares against 3000 blob PUTs. Worth doing only if files per part grow by an order of magnitude.

Provenance: BACKLOG/performance.md#staging-vector-quadratic-path-scans (2031-triage CAS-116). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Upserts are O(1) or O(log F) by path
- [ ] #2 Manifest entry order in the staged manifest is unchanged (existing gtests pass)
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
First recorded: 2026-08-21 (10bf7f7162f, by 'staging-vector-quadratic-path-scans')
<!-- SECTION:NOTES:END -->
