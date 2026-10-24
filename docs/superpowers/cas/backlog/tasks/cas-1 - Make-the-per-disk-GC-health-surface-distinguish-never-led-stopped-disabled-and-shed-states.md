---
id: CAS-1
title: >-
  Make the per-disk GC-health surface distinguish never-led, stopped, disabled
  and shed states
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:35'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - src/Interpreters/ServerAsynchronousMetrics.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - docs/superpowers/cas/umbrella-roadmap.md
  - docs/en/antalya/cas/operations/monitoring.md
priority: critical
type: enhancement
ordinal: 1000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The GC-health snapshot (`CasGcScheduler::gcHealth`, `CA/Gc/CasGcScheduler.cpp:469`) feeds `system.cas_mounts`
(`src/Storages/System/StorageSystemContentAddressedMounts.cpp:52-55`, `:201-214`) and the `CASGC*_<disk>` asynchronous
metrics (`src/Interpreters/ServerAsynchronousMetrics.cpp:385-391`). Three of its four readings are ambiguous or wrong,
and our own operator docs misread two columns. The first deployment blocks on alerts being in place, and a
GC-staleness alert built on these values today can never fire on a disk whose GC never succeeded.
Audit 2026-09-25 (F13, F25) confirmed both the negative backlog and the stuck-at-zero age live on otel.demo.
Roadmap §3 "Fix misleading metrics" carries the same ask.

Provenance: BACKLOG/operability-and-introspection.md#gc-health-zero-is-ambiguous (2031-triage CAS-098); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 All four subtasks are done
- [ ] #2 `operations/monitoring.md` documents each GC-health column and metric with the states it can express
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
First recorded: 2026-08-21 (20329364650, by 'gc-health-zero-is-ambiguous')
<!-- SECTION:NOTES:END -->
