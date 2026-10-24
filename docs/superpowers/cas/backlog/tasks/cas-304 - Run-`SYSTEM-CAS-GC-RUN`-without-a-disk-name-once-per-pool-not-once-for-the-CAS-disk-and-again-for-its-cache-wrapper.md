---
id: CAS-304
title: >-
  Run `SYSTEM CAS GC RUN` without a disk name once per pool, not once for the
  CAS disk and again for its cache wrapper
status: To Do
assignee: []
created_date: '2026-09-26 12:54'
labels:
  - 'area:gc'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
  - src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - src/Interpreters/ServerAsynchronousMetrics.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2031'
documentation:
  - docs/en/sql-reference/statements/system.md
priority: medium
type: bug
ordinal: 383000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`DiskObjectStorage::wrapWithCache` gives the cache disk the CA metadata storage itself (`src/Disks/DiskObjectStorage/DiskObjectStorageCache.cpp:29-31`, since `3ed0e5f5030`), so `ContentAddressedMetadataStorage::tryFromDisk` matches both the raw CAS disk and its cache disk.
The no-disk branch of `InterpreterSystemQuery::runContentAddressedGcRun` (`src/Interpreters/InterpreterSystemQuery.cpp:2550-2560`) loops over `getDisksMap` and runs one synchronous round per match.
That makes two sequential rounds on the same metadata storage and two result rows. On otel.demo a round's global LIST alone took ~1575 s (audit F19), so the second round doubles the operator's wait for no reclaim.
The same per-disk fan-out gives the cache disk its own `system.cas_mounts` rows (`src/Storages/System/StorageSystemContentAddressedMounts.cpp:122`) and its own `CASGC*_<disk>` asynchronous metrics (`src/Interpreters/ServerAsynchronousMetrics.cpp:381`), which duplicate the base disk's and double-count in alert sums.
Fix: deduplicate by metadata-storage identity, or skip disks that are cache layers of another listed disk (`getCacheLayersNames`); report the round under the base disk's name. Identical code on antalya-26.6.

Provenance: issue #2031 'New this round' CAS-136 (absent from docs/superpowers/cas/2031-triage.md); 2031-triage CAS-136; verified 2026-09-26 against cae9288ee65 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6). Found by code reading, so capped at Medium.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With a cache disk configured over a CAS disk, `SYSTEM CAS GC RUN` without a disk name runs exactly one round and returns one row for that pool
- [ ] #2 `system.cas_mounts` and the `CASGC*` asynchronous metrics show the pool once, not once per cache layer
- [ ] #3 `SYSTEM CAS GC RUN <cache disk>` either runs the base disk's round or refuses with a message naming the base disk; the chosen behaviour is documented
- [ ] #4 An integration test with a `<type>cache</type>` disk over a CAS disk covers the three points above
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
