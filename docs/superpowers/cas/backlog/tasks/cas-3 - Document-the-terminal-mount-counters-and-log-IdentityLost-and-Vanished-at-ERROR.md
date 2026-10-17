---
id: CAS-3
title: >-
  Document the terminal mount counters and log IdentityLost and Vanished at
  ERROR
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:observability'
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
  - src/Common/ProfileEvents.cpp
documentation:
  - docs/en/antalya/cas/operations/monitoring.md
priority: critical
type: bug
ordinal: 9000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
None of the seven terminal counters appears anywhere in `docs/`. `CASIdentityLost` and `CASDataRootVanished` mark states
the code calls TERMINAL, yet both are logged at `LOG_WARNING` (`CA/Pool/CasMountRuntime.cpp:971-972`, `:1045-1046`),
while the documented alerting example keys on ERROR (`CASMountExclusivityViolation`). The two signals that mean
"this mount is finished" are invisible in the docs and below the severity an operator is told to alert on.
Roadmap §3 "Alerts" needs these to exist before the first deployment.

Provenance: BACKLOG/operability-and-introspection.md#terminal-counters-undocumented-and-warned (opus review M3, P2); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `operations/monitoring.md` lists every terminal counter with the state it marks and the operator action
- [ ] #2 Entering IdentityLost or Vanished logs at ERROR
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
First recorded: 2026-08-22 (f07a9ed680d, by 'terminal-counters-undocumented-and-warned')
<!-- SECTION:NOTES:END -->
