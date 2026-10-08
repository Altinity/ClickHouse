---
id: CAS-264
title: >-
  Reclaim this mount's S3 staging debris periodically and show it in
  introspection
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:gc'
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
priority: low
type: enhancement
ordinal: 329000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
With opt-in `staging_backend=s3`, an aborted transaction leaves its staging objects in place on purpose (they are re-readable
publication sources). `sweepOwnMountStaging` (`CA/Pool/CasServerRoot.cpp:1804`) has one call site, at mount start
(`CA/ContentAddressedMetadataStorage.cpp:893`), so debris of every killed INSERT, failed mutation and aborted MOVE accumulates
for the whole uptime. GC never lists `staging/`, and fsck uses the same prefix, so it is not reported as `unaccounted`.
A dead member's staging is drained by `SYSTEM CAS DROP POOL MEMBER`. Off by default; every restart clears it.

Provenance: BACKLOG/formats-and-storage.md#s3-staging-reclaim-only-at-mount-start (2031-triage CAS-081); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A periodic sweep of `staging/<server_root_id>/` runs with an age filter that cannot reach an in-flight staging object
- [ ] #2 Staging object count and bytes appear in a system table or fsck output
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
First recorded: 2026-08-21 (55c650b8635, by 's3-staging-reclaim-only-at-mount-start')
<!-- SECTION:NOTES:END -->
