---
id: CAS-41
title: Rewrite the three ack-floor soak cards for the current floor and run them
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 14:36'
labels:
  - 'area:soak'
  - 'area:testing'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-0
dependencies: []
references:
  - utils/ca-soak/scenarios/BACKLOG.md
  - utils/ca-soak/scenarios/RUN_HISTORY.md
  - CA/Formats/CasServerRootFormats.h
priority: medium
type: task
ordinal: 51000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Three proposed cards (SIGSTOP a writer holds the floor then releases it; hard-KILL mid-burst, fence-out, fsck clean;
O(delta)+O(servers) request-count guard) are still only proposals in `utils/ca-soak/scenarios/BACKLOG.md:674-705`, listed
there as release-gate items. They reference removed symbols (`observed_gc_round`, `CasGcFloorHeldByStaleAck`); the floor is
now `min_active_build_sequence` plus `gc_fenced` in the mount lease (`CA/Formats/CasServerRootFormats.h:20-57`).
Unit- and TLA-covered, never soak-validated on either branch (`RUN_HISTORY.md` has no entry).

Provenance: BACKLOG/gc.md [ack-floor soak validation]; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The three cards exist as runnable scenarios against the current floor fields and events
- [ ] #2 Each card has one recorded run in `utils/ca-soak/scenarios/RUN_HISTORY.md` with its verdict
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'ack-floor soak validation')

Merged from u19a-soak (Rewrite the three ack-floor soak cards for the current floor and run them): Facts to add to CAS-41 (verified 2026-09-26 against 66087be0ffb):
The SIGSTOP card's premise is gone, not only its symbols. Graduation now paces on GC rounds (`new_round`), not on heartbeat acks (`CA/Gc/CasGc.cpp:483-486`). A paused writer therefore cannot hold the floor. It is fenced out once its write-token stays unchanged for `mountObservationThresholdMs` = TTL + TTL/20 + renewal period on the leader's monotonic clock (`CA/Pool/CasServerRoot.cpp:935`).
Re-derive that card as: SIGSTOP a writer past the threshold, then assert one fence-out, then SIGCONT and assert its next write is refused and it self-remounts with no dangling ref in fsck.
The kill-mid-burst card can assert `RoundReport::fence_outs` (`CA/Gc/CasGc.h:167`, set at `CA/Gc/CasGc.cpp:502`), the `CASGCHeartbeatFenceOuts` ProfileEvent and the per-srid `GcFenceOut` audit row. No card under `utils/ca-soak/scenarios/cards/` asserts a fence-out today; `framework/observe.py:416` only reads the column.
The request-budget card overlaps CAS-77 (perf-smoke CI gate on per-insert S3 cost) and the CAS-29 acceptance numbers; one per-round request-count guard can serve both.
Source text: utils/ca-soak/scenarios/BACKLOG.md:674-709. The 2026-07-03 header sweep lists them as release-gate items.
<!-- SECTION:NOTES:END -->
