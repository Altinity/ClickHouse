---
id: CAS-134
title: >-
  Show a running GC round's phase and elapsed time, and account every
  millisecond of a finished round
status: To Do
assignee: []
created_date: '2026-07-02'
updated_date: '2026-09-30 22:10'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcPhaseTimer.h
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - src/Interpreters/ContentAddressedGarbageCollectionLog.cpp
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#c5-observability
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f28
  - docs/en/antalya/cas/operations/monitoring.md
priority: high
type: enhancement
ordinal: 176000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A wedged or slow round is visible only after the fact: `cas_gc_log` writes phase rows and the `Finish` row when they
end, and `system.cas_mounts` has `last_success_age_seconds` but no round-in-progress, current-phase or phase-start
column. On otel.demo rounds took up to 5 h, and round 1383 was lost after 2 h 38 min of intake and reduce with nothing
in the log (audit F17, F28).
Phase rows do not sum to `duration_ms` ("the round also does untimed bookkeeping between phases",
`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp:64`); coverage was measured at 99.986%, and the remainder has no column.
Spec C5 adds `deadline_hit`/`carry_total` (CAS-29.10) and the audit an Info summary line
(gc-round-info-summary-line); neither shows a round while it runs.
CAS-4 excludes this item and maps the alert set; add the watchdog signal there once it exists.

Provenance: BACKLOG/gc.md#gc-observability [GC round progress observability] and [GC-FULL-TIME-ACCOUNTING] {#round-duration-alarm}. The source's 'name the orphan_sweep epilogue phase' is done (`orphan_sweep` is phase 18/18, CasGc.cpp:1192-1194); the unaccounted remainder is what is left. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `system.cas_mounts` (or a GC view) shows, for a round in flight, its number, current phase and the start time of round and phase
- [ ] #2 The `Finish` row carries `unaccounted_ms` = duration minus the phase sum, and a gtest keeps it under 1% of a synthetic round
- [ ] #3 A phase running longer than a configurable interval logs one progress line per interval with its phase metrics so far
- [ ] #4 A round that aborts inside the fold leaves a record naming the phase it aborted in (`GcFoldBegin` without `GcFoldEnd` is never the only trace)
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
First recorded: 2026-07-02 (c6ed3162fa0, by 'GC round progress observability')

Blocks CAS-289.3 (CAS Grafana dashboard v2): see docs/superpowers/specs/2026-09-30-cas-grafana-dashboards-design.md §8.
<!-- SECTION:NOTES:END -->
