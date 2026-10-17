---
id: CAS-151
title: >-
  Add full jitter to the self-remount backoff so nodes that lost the lease
  together do not retry in lockstep
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
documentation:
  - docs/superpowers/cas/2026-09-04-gcs-soak-15min.md
priority: low
type: enhancement
ordinal: 194000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CasMountRuntime::remountLoop` waits 1 s doubling to a 30 s cap with no jitter (`CA/Pool/CasMountRuntime.cpp:752`, `:805`).
In the real-GCS soak (2026-09-04) a DNS outage of `storage.googleapis.com` (11:07:01-11:08:49Z) fenced both nodes within 1.4 s,
and their attempts ran at the same second: gaps 1.5, 2.5, 4.5, 8.5, 16.5 s, then 72 s until attempt 7 succeeded. Harmless with
two nodes; with N nodes it is a thundering herd. Fix: draw the wait from the engine's full-jitter schedule (`Retry::backoff`).

Provenance: BACKLOG/mounts-and-lifecycle.md#remount-backoff-no-jitter; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Remount waits are drawn with full jitter under the same cap, shown by a unit test on the schedule
- [ ] #2 Two runtimes fenced at the same instant do not retry at the same instants in a test
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
First recorded: 2026-09-26 (2d670967bfa, by 'remount-backoff-no-jitter')
<!-- SECTION:NOTES:END -->
