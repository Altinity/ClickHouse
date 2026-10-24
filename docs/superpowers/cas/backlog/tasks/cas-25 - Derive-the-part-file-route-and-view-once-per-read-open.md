---
id: CAS-25
title: Derive the part-file route and view once per read open
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
  - CAS-16
  - src/Disks/DiskObjectStorage/DiskObjectStorage.cpp
  - '>-'
  - docs/superpowers/cas/2031-triage.md#cas-118
  - 'theme:parity'
dependencies:
  - CAS-16
references:
  - src/Disks/DiskObjectStorage/DiskObjectStorage.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - docs/superpowers/cas/2031-triage.md#cas-118
priority: low
type: enhancement
ordinal: 31000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`DiskObjectStorage::prepareRead` (`src/Disks/DiskObjectStorage/DiskObjectStorage.cpp:812`) calls `prepareInManifestRead` and then `getBlobViewPlan` (`:826-829`), and file size is resolved separately; each re-parses the path, re-routes and re-looks-up the view, up to three times per open.
CPU and lock traffic only: every repeat hits the warm in-memory cache, no request reaches object storage.
Fix: derive one view/route once and pass it to both callees. Schedule after the request-count items. Adjacent to the F31 view-seeding change, which cuts rebuild frequency, not per-open re-derivation.

Provenance: BACKLOG/performance.md#read-path-repeated-view-lookup-per-open (2031-triage CAS-118). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 One part-file open performs one path parse and one view lookup, asserted by a counter or gtest
- [ ] #2 No change in read results across the CA stateless suite
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
First recorded: 2026-08-21 (b9a1774ba4e, by 'read-path-repeated-view-lookup-per-open')
<!-- SECTION:NOTES:END -->
