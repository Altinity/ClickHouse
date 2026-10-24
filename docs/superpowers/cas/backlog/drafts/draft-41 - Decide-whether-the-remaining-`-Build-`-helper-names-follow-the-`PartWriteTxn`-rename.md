---
id: DRAFT-41
title: >-
  Decide whether the remaining `*Build*` helper names follow the `PartWriteTxn`
  rename
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:write-path'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartFolderAccess.cpp
priority: low
type: chore
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
After `Build` became `PartWriteTxn`, three helpers still say "Build": `promoteBuild`, `registerInflightBuild`,
`cancelInflightBuildsForNamespace` (e.g. `CA/Parts/PartFolderAccess.cpp:297`). The other three named in the source are gone.
They arguably name the protocol's `inflight_builds` concept, which the spec keeps. No correctness impact.

Provenance: BACKLOG/docs-and-cleanup.md#source-layout-residue [source-layout-build-naming]; verified 2026-09-26 against 6eb16e1cc56.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision records rename or keep for each of the three names
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
First recorded: 2026-08-04 (0f266066bef, by 'source-layout-build-naming')
<!-- SECTION:NOTES:END -->
