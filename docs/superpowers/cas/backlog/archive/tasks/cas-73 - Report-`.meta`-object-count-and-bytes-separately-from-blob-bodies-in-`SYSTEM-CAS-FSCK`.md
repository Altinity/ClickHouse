---
id: CAS-73
title: >-
  Report `.meta` object count and bytes separately from blob bodies in `SYSTEM
  CAS FSCK`
status: To Do
assignee: []
created_date: '2026-09-26 07:10'
updated_date: '2026-09-26 08:26'
labels:
  - 'area:fsck'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - docs/superpowers/cas/2031-triage.md#cas-117
priority: low
type: enhancement
ordinal: 98000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every fresh or adopted blob has a `.meta` freshness sibling, so `.bin`/`.mrk*`/`primary.idx` cost two objects each, and each body carries a padded envelope, a large relative inflation for tiny mark files.
`FsckReport` (`R/Tools/CasFsck.h:95-162`) has pairing advisories (`meta_without_body`, `body_without_meta`) and `physical_bytes`, but no count or byte split between bodies and `.meta`; the partition already happens at `CasFsck.cpp:723-736`.
The split is the number the `.meta` fold decision (`blob-meta-marker-fold`) needs.

Provenance: BACKLOG/performance.md#per-blob-meta-sibling-object-count ('cheap now' half; 2031-triage CAS-117). Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 FSCK output shows body count, body bytes, `.meta` count and `.meta` bytes
- [ ] #2 A gtest on a small pool asserts the four values
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
Duplicate of CAS-11 (bytes per object class in fsck): its body/`.meta` split and gtest were merged there as an acceptance criterion; DRAFT-8 now depends on CAS-11. Archived in the Backlog.md final pass.
<!-- SECTION:NOTES:END -->
