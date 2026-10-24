---
id: CAS-71
title: >-
  Hot-key lane phase B: combining, GCS spacing, hold clamp, GC erase and `_ckpt`
  in the lane
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:ref-ledger'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:measurement'
dependencies:
  - CAS-70.3
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2343'
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
priority: medium
type: design
ordinal: 91000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ref_catalog` is one pool-global object mutated on every CREATE/DROP; phase B is the roadmap's answer to the remaining write hotspot (issue #2343). Designed to spec revision 26 (`26bde9f9604`), deferred by owner on 2026-09-04, not started.
Phase A fixed the seams so no sub-item reopens callers: `submit`'s signature, `Decide = DecideOnObject`, the engine `WriteResult`, the caller's `Conflict` loop, the queue with the guard as sole remover.
Each subtask has its own gate; none starts on the documentation's say-so.
Rules for the next design round (26 revisions in one day, findings never below eight per round): a review revision only removes or tightens; a new feature costs its own revision; prose slips are MINOR; after three rounds the remaining questions are answered by code and tests.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-b; roadmap §2 'Catalog write hotspot'. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Each subtask is either Done or closed with its gate's measured no-go recorded
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
First recorded: 2026-09-04 (879aadde73a, by 'hot-key-lane-phase-b')

Issue #2343 evidence: `CAS ref catalog 'data/cas/ref_catalog' update: gave up at the policy deadline after zero attempt(s)` — the lane starts the caller's 90 s deadline before queueing (`Backend/CasHotKeys.cpp:62-142`, per the issue), so a waiter behind a stuck holder expires without sending a request. The replication-queue `num_tries` amplifier (charged before `trySchedule`, `ReplicatedMergeTreeQueue.cpp:2019`, unchanged on antalya-26.6) is tracked upstream as https://github.com/ClickHouse/ClickHouse/issues/122106.
<!-- SECTION:NOTES:END -->
