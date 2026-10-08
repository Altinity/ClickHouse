---
id: CAS-260
title: Refuse a second `FREEZE WITH NAME` into an existing shadow ref on a CAS disk
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:2031-triage'
milestone: m-3
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
priority: medium
type: bug
ordinal: 325000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ContentAddressedMetadataStorage::isDirectoryEmpty` answers `true` for every part-shaped directory, shadow parts included
(`CA/ContentAddressedMetadataStorage.cpp:1928-1947`), so `DiskObjectStorage::removeDirectory` reaches the ref-unlink. The side
effect: the `DIRECTORY_ALREADY_EXISTS` guard that makes a repeated `ALTER TABLE ... FREEZE WITH NAME 'x'` fail upstream never
fires, the second freeze stages into the same shadow ref, and `publishStaging` merges old and new entries.
Harmless for an unchanged part. After `REPLACE PARTITION` or `ATTACH PARTITION FROM` reuses a part name (e.g. `all_1_1_0`),
the frozen ref silently mixes files from two parts. Cross-disk `ATTACH PARTITION FROM` into CAS works since `cfe9a6a3615`,
which widens reachability.
Fix options: keep the listing-based answer for shadow part dirs, or refuse an existing shadow ref on the FREEZE staging path.

Provenance: BACKLOG/formats-and-storage.md#freeze-name-reuse-merges-shadow-ref (2031-triage CAS-086); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A stateless test freezing twice with the same name fails the second time, as on a local disk
- [ ] #2 A gtest next to the existing shadow-shape cases pins the chosen fix; part removal still reaches the ref-unlink
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
First recorded: 2026-08-21 (c459c34d897, by 'freeze-name-reuse-merges-shadow-ref')
<!-- SECTION:NOTES:END -->
