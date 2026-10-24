---
id: CAS-1.1
title: Stop reporting `last_success_age_seconds = 0` when no GC round ever succeeded
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:35'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:small'
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
parent_task_id: CAS-1
priority: critical
type: bug
ordinal: 2000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`last_success_age_seconds = 0` means both "never led a round" and "succeeded within the last second".
`gcHealth` already computes `ever_succeeded` (`CA/Gc/CasGcScheduler.cpp:475`, field `CasGcScheduler.h:143`), but only two
gtests read it (`src/Disks/tests/gtest_cas_gc_log.cpp:733,742`). The local-row insert is unconditional
(`StorageSystemContentAddressedMounts.cpp:205`, the column is already `Nullable`), and so is the metric
(`ServerAsynchronousMetrics.cpp:389`). Consequence: `CASGCLastSuccessAgeSeconds_<disk> > threshold` can never fire for a
disk whose GC never succeeded. Audit F25 saw it read 0 for over three hours after a restart with no successful round.
Two options: render NULL and skip the metric when `!ever_succeeded` (one line), or derive the age from the durable
`gc/state` timestamp as roadmap §3 proposes, which also survives restarts. Pick one; do not ship both.

Provenance: BACKLOG/operability-and-introspection.md#gc-health-zero-is-ambiguous item 1 and #ever-succeeded-unused (opus review M4); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 On a disk whose GC has not succeeded since the process started, `system.cas_mounts.last_success_age_seconds` is not 0 and `CASGCLastSuccessAgeSeconds_<disk>` does not report 0
- [ ] #2 After a successful round the column and metric report the elapsed seconds as before
- [ ] #3 A test covers both the never-succeeded and the just-succeeded case through the SQL surface
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
First recorded: 2026-08-22 (f07a9ed680d, by 'ever-succeeded-unused')
<!-- SECTION:NOTES:END -->
