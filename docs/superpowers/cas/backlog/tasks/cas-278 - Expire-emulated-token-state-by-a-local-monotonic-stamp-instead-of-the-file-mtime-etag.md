---
id: CAS-278
title: >-
  Expire emulated token state by a local monotonic stamp instead of the
  file-mtime etag
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
  - src/Disks/tests/gtest_cas_backend.cpp
priority: low
type: bug
ordinal: 345000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`etagComfortablyInThePast` (`CA/Backend/CasObjectStorageBackend.cpp:391`, used `:462`, `:999`) compares an etag derived from
the file's mtime against the process's `system_clock`. On a shared mount or across an NTP step the entry never expires, leaking
about 100 bytes per deleted key; a later recreate mints a disambiguated `etag#N`, still unique and fail-closed. Emulated mode only.
The recorded `queued_at_ns` is a local stamp that can drive expiry; a size cap is a backstop. `gtest_cas_backend.cpp:847-919`
has the injectable clock and etag doubles for a failing-first test.

Provenance: BACKLOG/formats-and-storage.md#emu-token-state-clock-skew-leak (2031-triage CAS-067); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Expiry uses a locally generated monotonic stamp, and a skewed-clock gtest shows the entry expires
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
First recorded: 2026-08-21 (b80ba4f80fb, by 'emu-token-state-clock-skew-leak')
<!-- SECTION:NOTES:END -->
