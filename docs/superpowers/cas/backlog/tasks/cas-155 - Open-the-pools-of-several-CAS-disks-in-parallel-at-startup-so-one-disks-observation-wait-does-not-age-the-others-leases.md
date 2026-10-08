---
id: CAS-155
title: >-
  Open the pools of several CAS disks in parallel at startup so one disk's
  observation wait does not age the others' leases
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:spec'
milestone: m-8
dependencies: []
references:
  - src/Disks/DiskSelector.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
priority: low
type: enhancement
ordinal: 198000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
One disk's token-stability observation (about TTL + TTL/20 + poll) blocks the startup thread while leases of disks mounted
earlier age; renewers start per disk and eagerly, so the exposure is the serial `DiskSelector::initialize` -> `disk->startup()`
-> `Pool::open` chain (`src/Disks/DiskSelector.cpp:106-125`). Ordinary restarts no longer trigger it since the farewell-window
fix; a hard kill of a server with several CAS disks still does.
Direction: run `Pool::open` per disk on a background task and collect the futures after the disk loop. Touches
`DiskSelector`, so it needs an upstream-coupling verdict and a spec.

Provenance: BACKLOG/mounts-and-lifecycle.md#mount-protocols-serial-startup; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An integration test hard-kills a server with two CAS disks and both remount without either lease expiring
- [ ] #2 Disk startup failures still surface as they do today
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
First recorded: 2026-09-26 (43b3a043935, by 'mount-protocols-serial-startup')
<!-- SECTION:NOTES:END -->
