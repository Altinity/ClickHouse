---
id: CAS-1.2
title: >-
  Expose whether this node's GC is running, stopped by an operator, or disabled
  by config
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-30 22:10'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - src/Interpreters/InterpreterSystemQuery.cpp
documentation:
  - docs/en/antalya/cas/operations/monitoring.md
parent_task_id: CAS-1
priority: medium
type: enhancement
ordinal: 3000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`SYSTEM CAS GC STOP` is stop-in-place: `ContentAddressedMetadataStorage::gcStop` (`CA/ContentAddressedMetadataStorage.cpp:1090`)
keeps the scheduler, so a stopped node shows `is_leader = 0` exactly like a follower. The only trace is one `LOG_INFO`
in `InterpreterSystemQuery::contentAddressedGcStop` (`src/Interpreters/InterpreterSystemQuery.cpp:2681`).
With `gc_enabled=false` (`CA/ContentAddressedSettings.cpp:57`) no scheduler is built at all (`ContentAddressedMetadataStorage.cpp:909`)
and nothing ever says so; if no other node leads GC, the pool accumulates garbage silently.
Node-local, non-durable STOP is intended (same as `SYSTEM STOP MERGES`); the gap is observability only.

Provenance: BACKLOG/operability-and-introspection.md#gc-health-zero-is-ambiguous item 2 and #gc-enabled-false-silent (user settings-policy direction); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `system.cas_mounts` has a column on the local row that distinguishes running, stopped by `SYSTEM CAS GC STOP`, and disabled by `gc_enabled=false`
- [ ] #2 A per-disk asynchronous metric carries the same state so an alert can key on it
- [ ] #3 While GC is disabled or stopped, the server logs a rate-limited periodic warning naming the disk
- [ ] #4 A test drives STOP and START and checks the column value after each
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
First recorded: 2026-08-04 (967849c785b, by 'gc-enabled-false-silent')

Blocks CAS-289.3 (CAS Grafana dashboard v2): see docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md §8.

Expose the state through system.metrics (CurrentMetrics), not asynchronous metrics: the transition is exact and cheap (user decision 2026-09-30, same reasoning as CAS-289.4).
<!-- SECTION:NOTES:END -->
