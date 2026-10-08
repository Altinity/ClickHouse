---
id: CAS-273
title: >-
  Let `MOVE` and TTL moves onto a CAS disk relink from a replica that already
  holds the part
status: To Do
assignee: []
created_date: '2026-09-03'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:replication'
  - 'complexity:large'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:spec'
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreePartsMover.cpp
  - src/Storages/MergeTree/DataPartsExchange.cpp
priority: medium
type: feature
ordinal: 338000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Zero-copy `MergeTreePartsMover::clonePart` tries `tryToFetchIfShared` before copying (`src/Storages/MergeTree/MergeTreePartsMover.cpp:241-263`).
A CA destination never takes that branch (`supportZeroCopyReplication()` is false for CAS, `DiskObjectStorage.h:54-58`), so
when every replica hits `TTL ... TO VOLUME 'cas'` at about the same time, each one reads, hashes and uploads the same part;
the losers pay the full cost and dedup away. Wanted: a CA hook at the same seam that reuses the fetch path's
`getRelinkOffer`/`confirmExactRef` exchange (`src/Storages/MergeTree/DataPartsExchange.cpp:282`, `:432`) and adopts through
`prepareAdoptFromManifest`, with a byte-clone fallback.
Open: replica discovery (no cross-server index of ref holders), and whether the N-replica race needs an upload stagger.
Builds on `b794a1517dd`. `same-pool-move-carry-refs` is the local leg.

Provenance: BACKLOG/replication.md#move-to-ca-relink-from-replica; listed as known-missing by #zero-copy-parity-audit. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A short design names how a mover finds a replica that holds the part and what happens when none does
- [ ] #2 An integration test with two replicas and a TTL move to CAS shows one replica uploads and the other relinks (zero new blob PUTs)
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
First recorded: 2026-09-03 (9be5befc3ec, by 'move-to-ca-relink-from-replica')
<!-- SECTION:NOTES:END -->
