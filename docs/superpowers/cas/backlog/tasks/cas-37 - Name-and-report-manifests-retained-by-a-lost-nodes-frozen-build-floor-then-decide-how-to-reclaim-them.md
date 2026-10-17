---
id: CAS-37
title: >-
  Name and report manifests retained by a lost node's frozen build floor, then
  decide how to reclaim them
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'area:fsck'
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - CA/Gc/CasOrphanManifestSweep.cpp
  - CA/Tools/CasFsck.cpp
  - CA/Tools/CasDecommission.h
priority: low
type: task
ordinal: 47000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Orphan-sweep eligibility comes only from the victim's own mount lease (`floorForNamespace`, `prefixEligibleUnder`,
`CA/Gc/CasOrphanManifestSweep.cpp:42-68,539-552`). Losing a node freezes its floor instead of retiring it, so manifest
bodies in flight at the loss, and their blobs, are retained until `SYSTEM CAS DROP POOL MEMBER` sets the retired
sentinel. Bytes are part-sized, bounded by in-flight concurrency at loss. `cas-fsck` mislabels them
`in-flight-pre-precommit` (`CA/Tools/CasFsck.cpp:1121`). Owed: a named retain class in fsck and GC reports, then an owner
decision on reclamation (protocol-adjacent).

Provenance: BACKLOG/gc.md#dead-member-frozen-build-floor (2031-triage CAS-077); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck and the sweep report name this retain class distinctly from genuine in-flight builds
- [ ] #2 The owner records a reclaim decision (decommission only, or an automatic rule)
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-077')
<!-- SECTION:NOTES:END -->
