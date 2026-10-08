---
id: CAS-97
title: Decide S23's idle-memory verdict from a 60-minute fresh-boot curve
status: To Do
assignee: []
created_date: '2026-08-31'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:soak'
dependencies:
  - CAS-96
references:
  - utils/ca-soak/scenarios/cards/s23_s27_misc.py
  - utils/ca-soak/configs/profiling.xml
  - utils/ca-soak/configs/memory.xml
priority: low
type: research
ordinal: 135000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S23 at `--scale full` (2026-08-31) failed "memory flat over idle window" on an empty pool: RSS 677 -> 1,178 MB on `ch1`, `mem_tracking` 337 -> 666 MB, with 11 GC rounds deleting nothing.
A second 15-minute probe started at 1,102 MB after a write workload and did not grow (+19 MB inside +-80 MB oscillation). Its background allocations were system-log inserts (~1,055 inserts into eight log tables, ~176 MB/hour), partly caused by the rig's own 10 ms query profiler and memory sampling. No CAS frame was in the top.
The two runs reach the same plateau by different routes; whether the fresh-boot climb stops there is unshown. The verdict still gates on `MemoryTracking` growth <= 64 MiB (`utils/ca-soak/scenarios/cards/s23_s27_misc.py:214-231`).
Do not file a leak, or record warm-up, on the 15-minute number.

Provenance: BACKLOG/performance.md#s23-idle-rss-growth (alias s23-idle-baseline-measures-telemetry). Items 1-2 done in 427f30ec27c. Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A fresh-boot idle run of at least 60 minutes records RSS and `MemoryTracking` per minute, and the curve is attached to the run
- [ ] #2 The result is classified as plateau or continued growth; growth comes with the owning `Memory` stacks from the idle window
- [ ] #3 S23's verdict either accounts for system-log churn or quiets the profilers during the idle window, and a rerun passes or fails for the classified reason
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
First recorded: 2026-08-31 (b34ad0c9597, by 's23-idle-baseline-measures-telemetry')
<!-- SECTION:NOTES:END -->
