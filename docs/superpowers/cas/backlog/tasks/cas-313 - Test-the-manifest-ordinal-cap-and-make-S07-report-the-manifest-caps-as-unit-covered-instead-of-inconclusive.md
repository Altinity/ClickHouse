---
id: CAS-313
title: >-
  Test the manifest ordinal cap and make S07 report the manifest caps as
  unit-covered instead of inconclusive
status: To Do
assignee: []
created_date: '2026-09-26 14:44'
labels:
  - 'area:testing'
  - 'area:soak'
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - src/Disks/tests/gtest_cas_part_write.cpp
  - utils/ca-soak/scenarios/cards/s06_s08_manifest_parts.py
priority: low
type: chore
ordinal: 392000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S07's direct manifest-cap trip is unreachable through SQL: the cap is orders of magnitude above dev reach, and the full-scale
20,000-column probe did not finish in 20 minutes. Every S07 row in `RUN_HISTORY.md` is therefore `inconclusive` (last 2026-09-01).
The 256 MiB encoded-bytes cap is covered at unit level since `81b40ae0df2`
(`CASPartWriteTxn.ManifestCapEncodedBytesOverThrowsBeforeBodyWrite`, `src/Disks/tests/gtest_cas_part_write.cpp:2324`).
The ordinal cap (`kMaxManifestOrdinal`, checked at `CA/Pool/CasPartWriteTxn.cpp:619-621`) has no test: it needs about 1e6
real `stageManifest` calls and has no injection point.

Provenance: utils/ca-soak/scenarios/BACKLOG.md#S07 manifest-cap fail-close (commit 81b40ae0df2) remaining sub-gap, and S01/S07 ci/full-scale attempt 2026-07-11; verified 2026-09-26 against 66087be0ffb (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6). S07's full-scale connection cost is CAS-104; the pre-encode cap check is CAS-258.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A gtest drives `stageManifest` past `kMaxManifestOrdinal` through a test seam and asserts it throws before any body write
- [ ] #2 S07's direct-cap verdict names the gtests that cover both caps instead of recording inconclusive on every run
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
