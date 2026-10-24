---
id: CAS-85
title: >-
  Dump `system.cas_gc_log` into CI artifacts so round and phase durations are
  visible per run
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:ci'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
  - 'theme:parity'
milestone: m-7
dependencies: []
references:
  - ci/jobs/scripts/clickhouse_proc.py
  - 'https://github.com/Altinity/ClickHouse/issues/2298'
priority: medium
type: chore
ordinal: 120000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The GC backlog runaway on the msan/tsan CAS lanes (#2298) was diagnosed from a local rig because CI keeps no GC history:
`dump_system_tables` exports `query_log`, `trace_log`, `metric_log`, `part_log` and others but not `cas_gc_log`
(`ci/jobs/scripts/clickhouse_proc.py:1260-1272`, the `TABLES` list). The dump already opens CAS disks read-only.
Add `cas_gc_log` (and consider `cas_log` within the row limit) on CAS lanes only.

Provenance: BACKLOG/gc.md#janitor-page-hardcoded ask 5. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (no cas_gc_log in the list on either).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A CAS stateless lane run's artifacts contain `cas_gc_log` with round and phase rows
- [ ] #2 Non-CAS lanes and the master binary's config are unaffected (unknown table skipped, not a failure)
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

Identifier trace: the earliest docs mention of `query_log` is 2026-06-05 (2c762c70d33); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
