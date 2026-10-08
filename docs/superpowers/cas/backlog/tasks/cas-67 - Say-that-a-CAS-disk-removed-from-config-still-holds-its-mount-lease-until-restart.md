---
id: CAS-67
title: >-
  Say that a CAS disk removed from config still holds its mount lease until
  restart
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:mounts'
  - 'area:observability'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskSelector.cpp
priority: low
type: enhancement
ordinal: 81000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Removing a CAS disk from `storage_configuration` and reloading produces only the generic "disappeared from configuration, this change will be
applied after restart" warning (`src/Disks/DiskSelector.cpp:217`); the disk is not shut down. For a CAS disk the mount lease keeps being
heartbeaten, so its `server_root_id` slot cannot be taken over by another server before a restart. Same disk-registry-caches-forever class
as `mounts-and-lifecycle.md#disk-lifecycle-rev8-closure`, bounded by the restart. Short-term fix: a CAS-specific line naming the retained lease
and the `SYSTEM CAS FORGET` verb.

Provenance: BACKLOG/operability-and-introspection.md#cas-settings-not-reloadable-silently, removed-disk half; verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Superseded if the disk-lifecycle redesign (UNMOUNT ejects) lands first.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Removing a CAS disk from config and reloading logs a line naming the disk, the retained lease and `SYSTEM CAS FORGET`
- [ ] #2 `docs/en/antalya/cas/configuration.md` states that removing a CAS disk from config takes effect only after restart or `SYSTEM CAS FORGET`
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

2031-triage CAS-107: the removed-disk half (mount lease renewed until restart); reload half is CAS-66. Audit ranks CAS-107 #5 in 'Where to start'.

Identifier trace: the earliest docs mention of `server_root_id` is 2026-06-28 (2243419abe5); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
