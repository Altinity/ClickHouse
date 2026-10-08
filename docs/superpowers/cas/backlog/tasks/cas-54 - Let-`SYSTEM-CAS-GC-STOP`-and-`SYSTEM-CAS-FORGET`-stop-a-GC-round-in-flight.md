---
id: CAS-54
title: Let `SYSTEM CAS GC STOP` and `SYSTEM CAS FORGET` stop a GC round in flight
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
  - docs/superpowers/cas/history/2026-09-04-cas-gc-teardown-stop-design.md
priority: medium
type: enhancement
ordinal: 64000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CasGcScheduler::stop` sets `stopping`, notifies and then joins (`CA/Gc/CasGcScheduler.cpp:101-119`): a round in flight runs to completion,
and the verb waits for it. The round's destructive work is count-capped (`GcRoundWorkBudget`, `CA/Gc/CasBlobInDegree.h:267`) but has no time
budget, so against a slow bucket the wait is whatever the bucket makes it.
Server shutdown and the storage destructor are already bounded (teardown flag, `history/2026-09-04-cas-gc-teardown-stop-design.md` rev.5).
Neither verb may reuse that pool-wide flag: `GC STOP` must not refuse unrelated `cas_mounts` queries or a running FSCK, and FORGET cannot arm it
because a latched self-remount's identity probe is admitted on the same plane. Both need a round-scoped stop token.
Spec stage C1 (`cas_gc_round_deadline_sec`, `remaining()` checks before each window) gives the natural cut points; the token can make `remaining()` zero.
Stopping mid-round is safe: every step is crash-safe, so a stopped round equals a crashed leader.

Provenance: BACKLOG/operability-and-introspection.md#lifecycle-verbs-wait-out-uncancellable-scans, GC-round half (2031-triage CAS-049); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). Easiest after spec stage C1 lands.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SYSTEM CAS GC STOP` returns within one S3 request of the current phase boundary while a round is in flight, proven by a test with a slowed backend
- [ ] #2 `SYSTEM CAS FORGET` does the same without arming the pool-wide teardown flag
- [ ] #3 A running FSCK and `system.cas_mounts` queries are unaffected by either verb
- [ ] #4 The next round after `GC START` completes normally and fsck reports no new findings
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

Identifier trace: the earliest docs mention of `CasGcScheduler` is 2026-06-14 (169f255b9a5); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
