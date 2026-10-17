---
id: CAS-179
title: Earn the clean mount farewell only when no snapshot publish is in flight
status: To Do
assignee: []
created_date: '2026-08-24'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:ref-ledger'
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - R/Pool/CasRefLedger.cpp
  - R/Pool/CasPool.cpp
  - src/Disks/tests/gtest_cas_detached_work.cpp
priority: medium
type: bug
ordinal: 236000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The clean farewell certifies that no in-flight conditional write of this incarnation can land after it (comment at `R/Pool/CasPool.cpp:1011-1017`).
`drainRefLanesForShutdown` (`R/Pool/CasRefLedger.cpp:1979-2037`) waits only on `pending` and `leader_active`, never on `pending_snapshot_publishes`, and a publisher writes a snapshot and `_ckpt`.
`~Pool` (`CasPool.cpp:1026`) and the FORGET path (`:1181-1190`) earn the farewell from that drain alone.
The storage teardown drains detached work first, but proceeds on timeout (`R/ContentAddressedMetadataStorage.cpp:1018-1026`), and FORGET does not drain it at all.
The old lifetime hazard (a detached publisher as the last `Pool` owner) is fixed by `e69b4d3c26f`; this is what remains.
The wait to transplant exists twice in the same file (`:1838`, `:4319`); bound it by the same `wait_budget_ms`.

Provenance: BACKLOG/ref-protocol.md#shutdown-drain-misses-snapshot-publishes (opus review NV-9), rewritten after e69b4d3c26f. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A drain with a publish in flight returns not-drained until the publish settles or the budget expires (gtest)
- [ ] #2 An expired budget with a publish in flight writes no clean farewell
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
First recorded: 2026-08-24 (45efac389eb, by 'shutdown-drain-misses-snapshot-publishes')
<!-- SECTION:NOTES:END -->
