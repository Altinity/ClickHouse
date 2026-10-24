---
id: CAS-291
title: >-
  Reduce and rename the columns of `system.cas_mounts`, `system.cas_gc_log` and
  `system.cas_log`, and document each table with example queries
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:observability'
  - 'area:docs'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:decision'
milestone: m-7
dependencies: []
references:
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - src/Interpreters/ContentAddressedGarbageCollectionLog.cpp
  - src/Interpreters/ContentAddressedLog.cpp
documentation:
  - docs/en/antalya/cas/operations/monitoring.md
priority: medium
type: enhancement
ordinal: 365000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`system.cas_mounts` has 20 columns (`src/Storages/System/StorageSystemContentAddressedMounts.cpp`), `system.cas_gc_log` 29
(`src/Interpreters/ContentAddressedGarbageCollectionLog.cpp`), `system.cas_log` 19 (`src/Interpreters/ContentAddressedLog.cpp`).
Several names describe internals rather than what an operator reads (`min_active_build_sequence`, `gc_fenced`, `renewal_sequence`,
`at_version`). The schema is cheaper to change before the first deployment than after.
Column-level fixes already filed are inputs, not duplicates: CAS-1, CAS-1.1, CAS-1.3, CAS-132, CAS-133, CAS-159.

Provenance: umbrella-roadmap.md section 3 bullet 'Simplify the system tables'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written proposal lists every column of the three tables as keep, rename or drop, with the reason, and the owner approves it
- [ ] #2 The approved schema is implemented; the changed log tables rotate on upgrade in the usual system-log way
- [ ] #3 Each table has a docs page section with at least two example queries that answer an operator question
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

Identifier trace: the earliest docs mention of `ContentAddressedGarbageCollectionLog` is 2026-06-17 (27eef9df8fc); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
