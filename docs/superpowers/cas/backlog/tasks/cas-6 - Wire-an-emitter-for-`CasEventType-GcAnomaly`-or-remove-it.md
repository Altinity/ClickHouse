---
id: CAS-6
title: 'Wire an emitter for `CasEventType::GcAnomaly` or remove it'
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEvent.cpp
priority: low
type: chore
ordinal: 12000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`GcAnomaly` exists only in `CA/Primitives/CasEvent.h:29` and its string mapping `CasEvent.cpp:46` on both branches; no call
site constructs one. A reader of `system.cas_log` looking for anomaly rows will never find any, which is the trap S16 fell
into with `BlobReuseResurrect`. GC anomalies (fold clamps) are already counted in `cas_gc_log.anomalies`, so emitting one row
per anomaly is optional. Decide which, then make the vocabulary honest.

Provenance: BACKLOG/operability-and-introspection.md#gc-anomaly-never-emitted (deep-verification batch-006); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Either every GC anomaly produces a `gc_anomaly` row in `system.cas_log`, covered by a test, or the enum member and its mapping are gone
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
First recorded: 2026-08-04 (967849c785b, by 'gc-anomaly-never-emitted')
<!-- SECTION:NOTES:END -->
