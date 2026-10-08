---
id: CAS-272
title: >-
  Fall back to a byte fetch when a relinked manifest fails with
  `UNKNOWN_FORMAT_VERSION`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:replication'
  - 'area:formats'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
priority: low
type: bug
ordinal: 337000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`prepareAdoptFromManifest` (`CA/ContentAddressedMetadataStorage.cpp:2344`) degrades to a byte fetch only for `CORRUPTED_DATA`
(`:2395`) and rethrows `UNKNOWN_FORMAT_VERSION`, which `decodePartManifest`'s header gate and the critical-key rule
(`CA/Formats/CasTextFormat.cpp:320-322`) both raise. The sibling `CA/Pool/CasRefLedger.cpp:191` accepts both.
Not reachable today: relink needs the same pool UUID and `_pool_meta` is exact-generation gated. It becomes reachable when a
release admits a mixed-generation pool, so land it before the format-version rollout (CAS-117).

Provenance: BACKLOG/replication.md#relink-fallback-unknown-format-version (2031-triage CAS-043); should precede CAS-117 (CAS-117). Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The catch accepts `UNKNOWN_FORMAT_VERSION` alongside `CORRUPTED_DATA`
- [ ] #2 A test feeds a manifest with an unknown critical key and sees the byte-fetch fallback
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
First recorded: 2026-08-21 (3ff0301f261, by 'relink-fallback-unknown-format-version')
<!-- SECTION:NOTES:END -->
