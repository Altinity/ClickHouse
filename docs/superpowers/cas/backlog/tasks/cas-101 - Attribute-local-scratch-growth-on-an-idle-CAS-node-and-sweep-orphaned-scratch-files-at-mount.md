---
id: CAS-101
title: Attribute local scratch growth on an idle CAS node
status: To Do
assignee: []
created_date: '2026-07-18'
updated_date: '2026-09-26 12:35'
labels:
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'needs:repro'
  - 'origin:soak'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
  - utils/ca-soak/scenarios/framework/sampler.py
priority: low
type: task
ordinal: 139000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The full-scale campaign saw local scratch grow 1 -> 21 MiB over an idle window with no user inserts.
Scratch files are the local-staging spill of `CaContentWriteBuffer` and the inline-overflow spill (`R/ContentAddressedTransaction.cpp:956`, `:1008-1010`). Each is removed by its writer on success or failure, but nothing sweeps the directory at mount, so files of a crashed process stay forever. No other reader of `scratchPath()` exists on either branch.
The idle growth may instead be in-flight spills of system-log inserts, which S23 showed run constantly on an "idle" node.

Provenance: BACKLOG/performance.md#scale-findings [idle-scratch-debris]. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The idle growth is attributed: in-flight spills of named writes, or files that outlive their write
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
The mount-time sweep of orphaned scratch files moved to CAS-125, which carries the same acceptance criterion; this task keeps only the attribution.

First recorded: 2026-07-18 (b22798a24a3, by 'idle-scratch-debris')
<!-- SECTION:NOTES:END -->
