---
id: CAS-95.3
title: >-
  Answer the `detached` and table-dir existence probes without two fresh catalog
  GETs
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:read-path'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
dependencies:
  - CAS-176
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp
  - src/Storages/MergeTree/MergeTreeData.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-95
priority: high
type: enhancement
ordinal: 133000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`MergeTreeData::getDetachedParts` calls `existsDirectory(<table>/detached)` for every table on every CAS disk of its policy. The `DetachedContainer` branch calls `hasAnyRefWithPrefix` (`R/ContentAddressedMetadataStorage.cpp:1686-1688`, `R/Pool/CasRefLedger.cpp:391`), whose cold runtime path reads the catalog twice.
Audit F21: 1,300-2,800 such queries per day from the operator user, ~0.9 s each, ~150-200k catalog GETs per day; `namespaceStillLogicallyPresent` from `clearOldTemporaryDirectories` adds ~37k more.
Fix direction (audit): answer a probe on a namespace this node holds no runtime for from a per-node catalog snapshot with etag revalidation. Align with the node-owned catalog reasoning of decision-5.

Provenance: audit #f21 via BACKLOG/performance.md#scale-findings [startup O(refs)]. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SELECT * FROM system.detached_parts` on a warm node issues at most one catalog read per disk, not two per table
- [ ] #2 `clearOldTemporaryDirectories` issues no catalog GET per table per minute on a warm node
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
First recorded: 2026-09-26 (27654231df2, by 'scale-findings [startup O')
<!-- SECTION:NOTES:END -->
