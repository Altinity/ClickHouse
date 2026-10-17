---
id: CAS-213
title: >-
  Cover pool-member decommission under load in ca-soak: SQL verb, live workload,
  and a kill at each phase
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s45_decommission_hidden_removing.py
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
priority: medium
type: task
ordinal: 270000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S45 (`utils/ca-soak/scenarios/cards/s45_decommission_hidden_removing.py`, validated live 2026-08-03) decommissions a killed victim with the
`cas-drop-member` CLI (`:83-91`) on an idle cluster. No scenario uses `SYSTEM CAS DROP POOL MEMBER`, none runs a workload during the
decommission, and none kills the decommission mid-run. Also uncovered: the fail-closed refusal of a mid-retirement victim with namespace
debris until GC namespace cleanup catches up.

Provenance: BACKLOG/testing-and-ci.md [b200-decommission-under-load]; verified 2026-09-26 against 8b87aa15d21. Related: CAS-156.3.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A scenario runs `SYSTEM CAS DROP POOL MEMBER` against a dead member while the survivors insert and merge, and ends with no victim rows and a clean fsck
- [ ] #2 A chaos variant kills the decommission at each phase and a re-run converges
- [ ] #3 The mid-retirement refusal is observed and then clears once GC catches up
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
First recorded: 2026-09-26 (5245fbc7a76, by 'b200-decommission-under-load')
<!-- SECTION:NOTES:END -->
