---
id: CAS-246
title: >-
  Remove the inert `allow_stale` parameter of `resolveRef` and its stale doc
  comments
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:ref-ledger'
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.h
priority: low
type: chore
ordinal: 311000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CasRefLedger::resolveRef` ignores the parameter (`CA/Pool/CasRefLedger.cpp:285`, `bool /*allow_stale*/`, explained at `:289`):
one authoritative cached `RefTableState` per mounted writer, nothing to be stale against. It is still declared
(`CasRefLedger.h:138`, `CasPool.h:588`), forwarded (`CasPool.cpp:1973-1975`) and passed (`CA/Parts/PartFolderAccess.cpp:289,579`),
and two comments still describe it: `PartFolderAccess.h:61` (`CachedForLoad`, "allow_stale=true") and
`ContentAddressedTransaction.cpp:1239`. `Freshness` itself stays: `ForceFresh`/`StrictValidate` gate `getView`'s manifest proof.

Provenance: BACKLOG/docs-and-cleanup.md#resolve-ref-allow-stale-inert-parameter (2031-triage CAS-110); verified 2026-09-26 against 6eb16e1cc56 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 No declaration, caller or comment mentions `allow_stale`
- [ ] #2 The `CAS*` gtest gate is green with no behavior change
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
First recorded: 2026-08-21 (2583e3427aa, by 'resolve-ref-allow-stale-inert-parameter')
<!-- SECTION:NOTES:END -->
