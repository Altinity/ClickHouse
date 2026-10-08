---
id: CAS-295
title: >-
  Split `Cas::Pool::open` into explicit create, open read-write and open
  read-only
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:plausible'
  - 'needs:decision'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
documentation:
  - docs/en/antalya/cas/configuration.md
  - docs/en/antalya/cas/quick-start.md
priority: medium
type: design
ordinal: 372000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The roadmap's `Store::open` is now `Cas::Pool::open` (`CA/Pool/CasPool.cpp:383`). One call does three jobs: on an empty prefix it mints
a fresh `_pool_meta` (`BootstrapResidual::EmptyOrProbeOnly`), on an existing pool it mounts writable, and a flag makes it read-only
(`PoolConfig::read_only`, `CA/Pool/CasPool.h:224`). `openForDecommission` (`CasPool.h:426`) is a fourth entry.
The risk is implicit creation: a mistyped `pool_prefix` on a writable disk silently creates a new empty pool, and the data looks gone.
DRAFT-51 wants a further `reader` mode.
Open question for the owner: how creation is requested (a disk setting, a `SYSTEM CAS` verb, or first-mount only with an explicit flag).

Provenance: umbrella-roadmap.md section 5 bullet '`Store::open` modes'; filed 2026-09-26 (user decision). Related: DRAFT-51. Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner picks how a pool is created, and the choice is recorded
- [ ] #2 A writable open of an empty prefix without the create request fails with a message naming the prefix
- [ ] #3 Create, read-write and read-only are separate entry points with their own tests, and existing pools open unchanged
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `_pool_meta` is 2026-06-02 (cafc256906e); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
