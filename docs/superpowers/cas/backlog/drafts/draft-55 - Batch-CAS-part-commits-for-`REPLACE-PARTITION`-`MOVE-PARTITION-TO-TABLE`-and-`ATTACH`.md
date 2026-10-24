---
id: DRAFT-55
title: >-
  Batch CAS part commits for `REPLACE PARTITION`, `MOVE PARTITION TO TABLE` and
  `ATTACH`
status: Draft
assignee: []
created_date: '2026-09-26 14:47'
labels:
  - 'area:write-path'
  - 'area:upstream'
  - 'complexity:large'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - src/Storages/StorageMergeTree.cpp
  - src/Storages/MergeTree/IDataPartStorage.h
  - src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp
priority: low
type: enhancement
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Bulk partition operations commit one disk transaction per cloned part, so each part pays its manifest PUT and ref round trips serially. INSERT batching does not help them.
`REPLACE PARTITION` and `MOVE PARTITION TO TABLE` pass only a MergeTree transaction (`src/Storages/StorageMergeTree.cpp:3046,3230`). `cloneAndLoadDataPart`/`freeze` commit their own disk transaction before `renameParts`. The `external_transaction` path that could carry one shared N-part transaction is used only by detached-part cloning (`src/Storages/MergeTree/IMergeTreeDataPart.cpp:2554`).
Options: (1) thread a shared `external_transaction` through the bulk clone paths (upstream-heavy); (2) let bulk callers hand uncommitted per-part transactions to the INSERT commit seam, which requires `freeze` to stop committing; (3) CAS-only: run bulk clones on a pool so the ref lane batches concurrent commits.
Revisit only when bulk-partition latency on CAS is measured as a problem. INSERT was the measured case. The upstream-code options need the same go-ahead DRAFT-2 is waiting for.

Provenance: utils/ca-soak/scenarios/BACKLOG.md:2985 (KNOWN ISSUE bulk partition operations, 2026-07-22). Related: DRAFT-2 (concurrent commitPart), CAS-14 (covering part outside DataPartsLock), CAS-116 (disk moves at scale). Verified 2026-09-26 against 56bf63c9fa7 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A measurement of `REPLACE PARTITION` with at least 500 parts on CAS versus plain S3 decides whether this opens
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
