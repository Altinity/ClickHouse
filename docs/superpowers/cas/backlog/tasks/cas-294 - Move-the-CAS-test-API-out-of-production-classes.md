---
id: CAS-294
title: Move the CAS test API out of production classes
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:testing'
  - 'area:tooling'
  - 'complexity:epic'
  - 'risk:medium'
  - 'confidence:solid'
milestone: m-4
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h
priority: low
type: chore
ordinal: 368000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Production CAS code carries 556 `ForTest`/`_for_test`/hook mentions at HEAD (486 on antalya-26.6). Top files: `CA/Pool/CasPool.h` 99,
`CA/Pool/CasRefLedger.h` 88, `CA/Pool/CasRefLedger.cpp` 75, `CA/Pool/CasPool.cpp` 66.
`PoolConfig` (`CA/Pool/CasPool.h:50`) is one flat struct that mixes operator settings with test hooks (35 `std::function` members in
`CasPool.h`); by-reference captures in those hooks caused the ASan failures CAS-224 still tracks.
Target: a `CasTestControl` adapter reached from tests only, injectable `Clock`, `Sleeper`, `Executor` and `FaultInjector`, a test factory,
and typed sub-configs instead of a flat `PoolConfig`. Move code, do not rewrite.
Schedule with the splits CAS-233, CAS-234, CAS-235 to avoid moving the same code twice. Neighbours: CAS-131, CAS-205, CAS-224.

Provenance: umbrella-roadmap.md section 5 bullet 'Test API out of production classes'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Production headers under `CA/` hold no `ForTest` members or hook `std::function`s outside the named seams
- [ ] #2 The `CAS*` gtest gate passes with the same test count before and after
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `PoolConfig` is 2026-06-11 (99466809c92); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
