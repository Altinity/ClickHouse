---
id: CAS-286
title: >-
  Measure CAS part-removal batch sizes and lower the parallel-removal threshold
  for CAS disks
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'theme:parity'
milestone: m-2
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreeData.cpp
  - src/Storages/MergeTree/MergeTreeSettings.cpp
  - src/Disks/IDisk.h
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f18
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f2
priority: high
type: enhancement
ordinal: 358000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A part removal on a CAS disk costs two serial ref-lane trips (repoint, drop). On the canary ~100 `Real` samples per profile sat in
`dropRef` waiting on the lane (audit F18), with 196k removals per day (audit section 1).
The threshold that sends removals to the parallel path is already lower for remote disks: `concurrent_part_removal_threshold_for_remote_disk`
defaults to 16 (`src/Storages/MergeTree/MergeTreeSettings.cpp:1771`, commit `706946343ae`, in antalya-26.6), chosen at
`MergeTreeData.cpp:3759-3761`, and a CAS disk counts as remote (`DiskObjectStorage.h:149`). Batches below 16 still run serially.
Parallel removals let the lane combine several mutations into one flush instead of one flush per part.
CAS-83 removes the repoint and calls a threshold of 1 an unapplied stopgap. This task covers the drop trip that remains after CAS-83.
Unknown: the batch-size distribution of removals on a CAS node.

Provenance: umbrella-roadmap.md section 2 bullet 'Parallel part removals'; filed 2026-09-26 (user decision). Related: CAS-83. Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The distribution of `parts_to_remove.size()` per removal call on a CAS node is measured on a soak or the stand
- [ ] #2 CAS disks use a threshold chosen from that measurement, by a CAS-aware default or a documented recommendation
- [ ] #3 Ref-lane mutations per flush during removal bursts rise and removal wall time falls, measured before and after
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

Identifier trace: the earliest docs mention of `MergeTreeData` is 2026-06-02 (c0a7046a3a7); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
