---
id: CAS-317
title: >-
  Add the thread-allocation-fault soak leg that S42 delegated to a card that was
  never built, and a failpoint for S42's post-PUT window
status: To Do
assignee: []
created_date: '2026-09-26 14:47'
labels:
  - 'area:soak'
  - 'area:testing'
  - 'area:ref-ledger'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s42_alloc_faults.py
  - utils/ca-soak/scenarios/README.md
  - src/Common/FailPoint.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h
documentation:
  - >-
    docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md
priority: medium
type: task
ordinal: 396000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The S42 allocation-fault card exists (`c44cb6cbe44`, verdict rework `402a85c4a64`) and passes at dev and ci. It covers only query-thread faults (`memory_tracker_fault_probability`). The requirement it serves is the user's: everything stays consistent despite allocation errors.
Leg B, thread-allocation faults through `cannot_allocate_thread_fault_injection_probability`, was sent to "scenario S43" (`cards/s42_alloc_faults.py:21-25`). S43 became the same-uuid recreation card, and no card sets that setting. These faults reach thread-creating paths the query knob cannot reach: the background snapshot dispatcher, GC scheduler and merge pools. The setting is applied on config reload (`Server.cpp`), not by `SYSTEM START THREAD FUZZER`.
The targeted signal is structurally zero. The post-durable install seam is the gtest-only `setInstallRegionProbeForTest` (`CA/Pool/CasPool.h:1055-1056`) with no `src/Common/FailPoint.cpp` registration. So no soak run can prove that a fault landed between a durable ref-log PUT and its in-memory apply.
Leg C's restart oracle alone misses a snapshot published from a poisoned cache. It also needs a replay from the last pre-fault snapshot plus raw tail logs, and a check that no snapshot advanced across a poisoned transaction.
The README is stale: it says S42 can never be a conclusive green (`scenarios/README.md:524-528`) and that thread faults "live in S43" (:507).
Optional stronger mode from the source: run S42 on a DEBUG image, where `DENY_ALLOCATIONS_IN_SCOPE` regions are enforced.

Provenance: utils/ca-soak/scenarios/BACKLOG.md:3012 {#s42-allocation-fault-soak} (card built; legs B, the targeted guard and the DEBUG mode are the residuals). Verified 2026-09-26 against 56bf63c9fa7 (cas-gc-rebuild): `git grep cannot_allocate_thread_fault_injection utils/ca-soak` hits only the S42 docstring and BACKLOG.md.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A card, or an S42 leg, arms `cannot_allocate_thread_fault_injection_probability` through config reload, requires a nonzero injected-failure count, and passes S42's consistency oracle
- [ ] #2 A registered `FailPoint` throws after the ref-log PUT and before the apply, and one soak run shows it fired with fsck clean and the post-restart view equal to the pre-restart view
- [ ] #3 README's S42 section and the card docstring name where leg B lives, and match the 2026-07-25 green-means-consistency decision
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
