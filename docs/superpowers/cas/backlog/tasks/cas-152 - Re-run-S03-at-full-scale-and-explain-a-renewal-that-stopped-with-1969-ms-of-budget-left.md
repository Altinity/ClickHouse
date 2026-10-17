---
id: CAS-152
title: >-
  Re-run S03 at full scale and explain a renewal that stopped with 1,969 ms of
  budget left
status: To Do
assignee: []
created_date: '2026-09-01'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:mounts'
  - 'area:soak'
  - 'complexity:medium'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s03_s05_scale.py
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestBudget.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2244'
priority: medium
type: research
ordinal: 195000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S03 at `--scale full` (2026-09-01T06:53, `RUN_HISTORY.md:710`) failed with `Code: 210 ... mount lease not held`.
`system.ca_event_log` showed two anomalies the #2244 fix (`7f932d31352`) should prevent:
- ch2 sent 1 attempt and stopped with `remaining_confirmed_budget_ms = 1969` and `stop_cause = continue`, so a justified retry
  was not issued (budget arithmetic, the `now + margin < confirmed_deadline` term, or the loop exit).
- ch1's single attempt took 23,755 ms against a 30 s TTL, far above the 5 s attempt timeout.
Storage was saturated (RustFS 256/256 permits, 503 after ~5 s). The engine migration `37c9bd4356b` and `9a6bcb68aca`
(budget an attempt by its whole envelope, fuse a first attempt hung in connect) may have closed both; no run since.
Does not reproduce at `dev`/`ci`. The later `mount_conflict`/`live_double_start` events are the no-wall-clock-trust rule
working, not the fault.

Provenance: BACKLOG/mounts-and-lifecycle.md#renewal-gives-up-with-budget-left; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S03 at `--scale full` has run on current HEAD and its result is recorded in `RUN_HISTORY.md`
- [ ] #2 If a renewal still stops with confirmed budget left or an attempt exceeds its envelope, a bug task names the code path; otherwise this closes with the run as evidence
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
First recorded: 2026-09-01 (71b002a1047, by 'renewal-gives-up-with-budget-left')

Issue #2431 (open, 2026-09-24) reproduces a renewal giving up with budget left at dev scale on 26.6.4: fenced after 2-2.6 s with ~17 s of `remaining_confirmed_budget_ms`, `classification=external_lease_deadline`. The 'does not reproduce at dev/ci' line no longer holds; the fix is tracked by the u17 task `renewal-fences-with-budget-left`, which this task may close into.
<!-- SECTION:NOTES:END -->
