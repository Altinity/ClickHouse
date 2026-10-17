---
id: CAS-45
title: >-
  Make `repointRef` refuse a key that does not resolve instead of counting it as
  a repoint
status: To Do
assignee: []
created_date: '2026-07-15'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:write-path'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
milestone: m-4
dependencies: []
references:
  - CA/Parts/PartFolderAccess.cpp
priority: low
type: chore
ordinal: 55000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CachedPartFolderAccess::repointRef` increments `CASRefRepoint` and logs a repoint even when `resolve(key)` returned
nullopt (`CA/Parts/PartFolderAccess.cpp:527,545`). Unreachable today, so the counter is not wrong in practice.
An explicit precondition makes the contract visible. It must be a `LOGICAL_ERROR` only if truly unreachable, with a
death test; otherwise count it as a publish, not a repoint.

Provenance: BACKLOG/gc.md [repointRef non-resolving-key audit gap]; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6. Revisit together with the delete_tmp repoint elision (audit F2).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The unresolved branch is either an asserted precondition with a death test or no longer counted as a repoint
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
First recorded: 2026-07-15 (6e02e1f77e3, by 'repointRef non-resolving-key audit gap')
<!-- SECTION:NOTES:END -->
