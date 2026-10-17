---
id: CAS-128
title: Expose the remaining CAS pacing and budget knobs as disk settings
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies:
  - CAS-66
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
priority: low
type: enhancement
ordinal: 166000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ContentAddressedSettings` has 31 keys; these remain struct defaults reachable only from gtests: `recovery_retry_budget_ms`
and its two backoffs (`CA/Backend/CasRequestBudget.h`), `snapshot_log_count_threshold`/`snapshot_log_bytes_threshold` and the
publish/precommit-sweep backoffs (`CA/Pool/CasPool.h:290-309`, a per-workload PUT-versus-cold-GET dial),
`gc_fold_threshold`/`gc_fold_max_defer_rounds` (`:160-163`), `rebuild_edge_budget` (`:170`). Excluded on purpose:
`gc_stuck_removal_rounds` (test seam) and `gc_frontier_probe_budget` (a cap converts into a GC stop). The old
`operation_deadline_ms`/`max_attempts` knobs no longer exist. Not a gate: soaks ran on these defaults.

Provenance: BACKLOG/operability-and-introspection.md#pool-pacing-knobs-no-config-surface (2031-triage CAS-105); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792. `ref_table_cache_bytes` is u05-perf-b's.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each listed knob is a `DECLARE`d disk setting wired through `openPoolView` into `Cas::PoolConfig`
- [ ] #2 `validate` checks the mount-lease inequality against configured values, and `Pool::open` still refuses an inconsistent budget
- [ ] #3 `configuration.md` documents each new setting and its default
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
First recorded: 2026-08-21 (2583e3427aa, by 'pool-pacing-knobs-no-config-surface')
<!-- SECTION:NOTES:END -->
