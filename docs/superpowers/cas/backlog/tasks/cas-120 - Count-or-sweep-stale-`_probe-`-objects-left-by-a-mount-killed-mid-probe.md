---
id: CAS-120
title: Count or sweep stale `_probe/` objects left by a mount killed mid-probe
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:fsck'
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasSentinelProbe.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.cpp
priority: low
type: enhancement
ordinal: 158000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`runCapabilityProbe` deletes its two keys on every exit path, so debris appears only after a hard kill mid-probe: two tiny
objects under `<pool>/_probe/<u128hex>/`, bounded by crashed mounts. Nothing reclaims or reports them: the bootstrap
residual scan skips `_probe/` on purpose (`CA/Backend/CasSentinelProbe.cpp:17-37`; `CA/Pool/CasPool.cpp:408-409`), and fsck
classifies only blob-plane keys. Bytes are negligible; the class is simply invisible.

Provenance: BACKLOG/operability-and-introspection.md#mpu-and-probe-debris-unaccounted (probe half; 2031-triage CAS-082); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 fsck reports a `probe_debris` count, or `Pool::open` deletes `_probe/` entries older than a fixed threshold
- [ ] #2 A test kills a probe midway and shows the leftover is counted or removed on the next mount
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

Identifier trace: the earliest docs mention of `runCapabilityProbe` is 2026-07-12 (e1e76106f40); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
