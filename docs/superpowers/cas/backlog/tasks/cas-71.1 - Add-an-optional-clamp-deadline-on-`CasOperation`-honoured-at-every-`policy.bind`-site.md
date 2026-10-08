---
id: CAS-71.1
title: >-
  Add an optional clamp deadline on `CasOperation` honoured at every
  `policy.bind` site
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:solid'
dependencies:
  - CAS-69
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
parent_task_id: CAS-71
priority: low
type: feature
ordinal: 92000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Reads nested inside a lane hold run under their own policy and can outlive the holder's window.
Design (spec rev.26): one optional clamp deadline on `CasOperation`, honoured as a minimum at its twelve `policy.bind` sites, set by the lane for the hold and by a leader for a member's `decide`, cleared after; a read the clamp refuses gives up as at its own deadline with `GaveUp::Source::Policy`.
Required by combining and by the GC erase in the lane; for phase A the caller-side freeze (`drop-namespace-freeze-policy`) is enough.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-b item 3. Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every `policy.bind` site honours the clamp; a gtest shows a nested read inside a hold gives up at the clamp with `Source::Policy`
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
First recorded: 2026-09-26 (b1c34d03479, by 'hot-key-lane-phase-b item 3')
<!-- SECTION:NOTES:END -->
