---
id: CAS-161
title: >-
  Map `CLOCK_BOOTTIME` per platform in `base/base/time.h` so CAS builds on
  Darwin
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:mounts'
  - 'complexity:trivial'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-4
dependencies: []
references:
  - base/base/time.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
priority: low
type: chore
ordinal: 207000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two unconditional `clock_gettime(CLOCK_BOOTTIME, ...)` reads (`CA/Pool/CasMountRuntime.cpp:78`, `CA/Pool/CasServerRoot.cpp:95`)
compile everywhere because `dbms` sources are not `OS_LINUX`-guarded. `CLOCK_BOOTTIME` is Linux-only; FreeBSD aliases it to
`CLOCK_UPTIME`, Darwin has no drop-in (`CLOCK_UPTIME_RAW` excludes sleep). Compile error on Darwin, zero runtime exposure.
Fix: extend the existing `CLOCK_MONOTONIC_COARSE` shim in `base/base/time.h` with a `CLOCK_BOOTTIME` mapping and note that
Darwin's substitute weakens, not breaks, the suspend argument (`CasMountRuntime.h:125`). Roadmap §5 "Portability".

Provenance: BACKLOG/mounts-and-lifecycle.md#boottime-not-portable; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A Darwin build compiles the CAS mount code
- [ ] #2 Linux behaviour is unchanged
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
First recorded: 2026-08-21 (b03f1c765f8, by 'boottime-not-portable')

2031-triage CAS-092: the only surviving half of the audit finding; 'different clocks for fence and request' stays refuted.
<!-- SECTION:NOTES:END -->
