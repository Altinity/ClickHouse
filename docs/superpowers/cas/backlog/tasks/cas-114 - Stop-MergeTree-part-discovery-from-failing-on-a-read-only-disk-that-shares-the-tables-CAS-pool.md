---
id: CAS-114
title: >-
  Stop MergeTree part discovery from failing on a read-only disk that shares the
  table's CAS pool
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-7
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreeData.cpp
  - utils/ca-soak/configs/fsck_only_ca.xml
  - utils/ca-soak/soak/run.py
priority: medium
type: bug
ordinal: 152000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An operator who adds a `<readonly>` CAS disk on the same pool (for fsck or inspection) cannot restart: `loadDataParts`'s
orphaned-parts scan finds every part again on that undefined disk and throws `UNKNOWN_DISK`
(`src/Storages/MergeTree/MergeTreeData.cpp:2459-2490`). Eligibility ignores read-only and same-pool
(`isDiskEligibleForOrphanedPartsSearch`, `:2405-2412`); default `search_orphaned_parts_disks = any`
(`MergeTreeSettings.cpp:2248`). Workarounds today: per-table `search_orphaned_parts_disks='local'`
(`utils/ca-soak/soak/run.py:114-135`) and a standalone `clickhouse-disks -C` fsck config. Fix options: skip read-only disks of
the same pool in the scan, or a `hidden` disk flag. The shared `MergeTreeData.cpp` change needs an upstream-coupling review.

Provenance: BACKLOG/operability-and-introspection.md#f1-prod-ca-ro-shadow-disk ([F1-prod], harness B143); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A server with a read-only same-pool CAS disk in `config.d` restarts with CAS tables loaded and no `UNKNOWN_DISK`
- [ ] #2 An integration test covers it without `search_orphaned_parts_disks` overrides
- [ ] #3 The fsck-only config workaround is described in the operations docs or removed as unnecessary
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'F1-prod')
<!-- SECTION:NOTES:END -->
