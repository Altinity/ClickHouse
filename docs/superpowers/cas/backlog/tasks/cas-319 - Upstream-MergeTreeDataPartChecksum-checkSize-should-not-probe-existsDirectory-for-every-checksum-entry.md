---
id: CAS-319
title: >-
  Upstream: MergeTreeDataPartChecksum::checkSize should not probe
  existsDirectory for every checksum entry
status: To Do
assignee: []
created_date: '2026-09-27 07:37'
labels:
  - 'area:upstream'
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
priority: medium
ordinal: 398000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
checkSize (src/Storages/MergeTree/MergeTreeDataPartChecksum.cpp:66-80) starts with 'if (storage.existsDirectory(name)) return;' (comment: 'This is a projection, no need to check its size') and is called from checkConsistencyBase for every entry of checksums.txt of every part at load (IMergeTreeDataPart.cpp:2646-2666). The only directory-shaped checksum entries are projections, and they are always written with the .proj suffix (MergedBlockOutputStream.cpp:224-229, DataPartsExchange.cpp:537-550); checkConsistencyBase already relies on that suffix to mark a projection broken (:2654-2660). So the directory probe per file duplicates what the name says: one stat per part file on a local disk, one metadata lookup per file on object-storage disks, and on a CAS disk one S3 LIST of the table's _files/ prefix per file (the restart storm of https://github.com/Altinity/ClickHouse/issues/2439: 77k LISTs in 3 minutes for 1,672 parts).

Proposed change: decide by name first, 'if (name.ends_with(".proj")) return;' (or probe existsDirectory only for such names), before any disk access. Upstream master already takes the same shape for gin files (isGinFile early return) ahead of the probe. Complementary to CAS-95.1 (the CAS-side PartFile shape, which also covers checkDataPart, load diagnostics and recursive part copy); with both, a restart never reaches a directory probe. Proposed by codex round 9 of spec 2026-09-26-cas-directory-probes-no-list-design.md (review_r9.md, 'Simpler alternatives').

Shape: a self-contained upstream PR to ClickHouse/ClickHouse with the motivation 'avoid one directory probe per part file at load on every disk', carried in the fork until merged.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every source of a directory-shaped checksum entry is enumerated and shown to carry the .proj suffix (old part formats, CLEAR PROJECTION, fetched parts, projections in compact/wide parts)
- [ ] #2 checkSize decides by name before touching the disk; a stateless test with projections (load, fetch, broken projection marked as before) stays green
- [ ] #3 Upstream PR opened with the per-disk motivation; fork patch applied to antalya until it merges
- [ ] #4 On a CAS disk a restart with N parts issues no _files/ LIST from checkSize (measured with CASRootList, together with or independently of CAS-95.1)
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
