---
id: CAS-1.3
title: >-
  Compute `pending_reclaim` from the adopted fold seal instead of a
  process-local counter
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-30 22:10'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:2031-triage'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-1
priority: high
type: bug
ordinal: 4000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`pending_reclaim` accumulates `condemned - redeleted` per process (`CA/Gc/CasGcScheduler.cpp:248`). Entries that leave as
`spared` or `replaced` (`CA/Gc/CasGc.h:160-161`) are never subtracted, so it drifts upward on a healthy pool; after a
restart the new process deletes a backlog it never condemned, and audit F13 measured -388,242 on otel.demo.
It is the only backlog number an operator has. Spec `#c5-observability` commits to computing it from the adopted seal's
`CondemnedSummary` and leaves open where those totals reach `cas_mounts` without a new read. The authoritative per-round
gauges `pending_candidates`/`pending_condemned`/`pending_retired` (`CA/Gc/CasGc.h:177-179`) are rendered only by
`SYSTEM CAS GC RUN` (`src/Interpreters/InterpreterSystemQuery.cpp:2380-2382`).
Audit F25 notes `CASGCRetiredCondemned - CASGCRetiredRedeleted` never goes negative and can serve as an interim dashboard line.
If the spec-C5 work is imported as its own task, merge this subtask into it.

Provenance: BACKLOG/operability-and-introspection.md#gc-health-zero-is-ambiguous item 3; implements spec #c5-observability bullet 3; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `system.cas_mounts.pending_reclaim` and `CASGCPendingReclaim_<disk>` equal the adopted seal's condemned-minus-deleted totals
- [ ] #2 The value is unchanged across a server restart with no GC round in between
- [ ] #3 The value never goes negative, and a test covers the restart case
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

Identifier trace: the earliest docs mention of `spared` is 2026-06-02 (c0a7046a3a7); the finding itself has no record before this migration, so the creation date stays 2026-09-26.

Blocks CAS-289.3 (CAS Grafana dashboard v2): see docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md §8.
<!-- SECTION:NOTES:END -->
