---
id: CAS-7
title: Prove `cas_log` manifest-delete rows match `cas_gc_log.manifests_deleted`
status: To Do
assignee: []
created_date: '2026-08-31'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - tests/integration/test_cas_gc_bulk_delete/test.py
priority: low
type: task
ordinal: 13000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
In the full-scale S10 run (2026-08-31) `ch1`'s `cas_gc_log` summed 517 `manifests_deleted` while its `cas_log` held one
`manifest_delete` row, so manifest reclaim was invisible in the audit log. The delete phase was since rewritten to batch
write-once deletes and now emits one event per recorded deletion in the same loop (`CA/Gc/CasGc.cpp:1136-1150`), which
removes the originally described emit/count mismatch. No test asserts the parity, and the other candidate cause, the system
log queue dropping rows under a burst, was never ruled out. Until this is proved, the S10 residual-manifest finding
(`performance.md#s10-manifest-residual`) must be derived from `cas_gc_log` and fsck, not `cas_log`.

Provenance: BACKLOG/operability-and-introspection.md#ca-event-log-loses-manifest-deletes (S10 audit 2026-08-31); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An integration test deletes a burst of manifests in one round and asserts the `manifest_delete` row count in `system.cas_log` equals the round's `manifests_deleted`
- [ ] #2 If rows are dropped, the cause is identified and the drop is either fixed or surfaced as a counter
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
First recorded: 2026-08-31 (d6986f799f4, by 'ca-event-log-loses-gc-manifest-deletes')
<!-- SECTION:NOTES:END -->
