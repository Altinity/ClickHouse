---
id: CAS-217
title: >-
  Aim S27's LIST anomalies at the janitor and orphan-sweep prefixes and assert
  GC deleted nothing reachable
status: To Do
assignee: []
created_date: '2026-08-31'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s23_s27_misc.py
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
priority: medium
type: task
ordinal: 275000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`d6986f799f4` re-aimed S27 from the retired `cas/refs/` to `cas/ns/stream/` (`utils/ca-soak/scenarios/cards/s23_s27_misc.py:584-589`, `:649`),
the prefix of GC discovery. Still uncovered: `CasNamespaceJanitor` pages `namespaceRootPrefix()` and `CasOrphanManifestSweep` pages
`casManifestsPrefix()`; pagination ambiguity there is the hazard S27 exists for. The card asserts only that LISTs were perturbed, not GC's outcome.
Spec B1 removes the global `cas/ns/stream/` LIST, which would retire S27's target a second time. Per decision-2 a LIST lie may delay reclamation, never authorize a delete.

Provenance: BACKLOG/testing-and-ci.md#s27-list-anomaly-aimed-at-a-retired-path (fix direction not fully met by d6986f799f4); verified 2026-09-26 against 8b87aa15d21. Related: CAS-29.1. S27's full-scale run (subtask s27-full-scale-run of the scenario sweep) waits for this re-aim.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S27 perturbs LISTs on the janitor and orphan-sweep prefixes, with the not-vacuous guard per prefix
- [ ] #2 The verdict checks that no object a complete listing shows as reachable was deleted
- [ ] #3 After spec B1 lands the card still has a live LIST target, or fails its guard
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
First recorded: 2026-08-31 (a4390780578, by 's27-list-anomaly-aimed-at-a-retired-path')
<!-- SECTION:NOTES:END -->
