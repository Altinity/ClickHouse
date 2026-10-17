---
id: CAS-215
title: >-
  Size the soak's unreachable-convergence wait by measured fsck time and a
  backlog read taken after GC starts condemning
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/soak/checker.py
  - utils/ca-soak/soak/run.py
  - utils/ca-soak/tests/test_checker_logic.py
priority: medium
type: bug
ordinal: 273000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Real AWS, 8-minute no-chaos smoke `ca_live_20260904_aws_r1`: the checkpoint failed at its 300 s bound while the pool converged on its own
38 minutes later (`unreachable=0 dangling=0`; 4305 condemned = graduated = redeleted). Two defects besides the round-rate bound of CAS-93:
1. `poll_unreachable_to_stable` needs `stable=3` samples (`utils/ca-soak/soak/checker.py:279`) and each is a full fsck (~325 s at 4-5k objects), so it cannot fit 300 s at all;
2. `initial` is read once before the first condemning round (`:581-588`), so the backlog-scaled bound collapses to its floor while the backlog is still rising.
Also the run JSON says `fsck_status: "skipped"` when three fscks ran and only the final one was not reached.

Provenance: BACKLOG/testing-and-ci.md#soak-harness-bugs-2026-09-04 bullet 1; verified 2026-09-26 against 8b87aa15d21. Related: CAS-93 (CAS-93), CAS-63.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The bound is at least `stable` times the measured fsck duration plus a drain estimate from `pending_reclaim` and `cas_gc_log` round durations
- [ ] #2 The backlog used for the bound is read after the first condemning round
- [ ] #3 `fsck_status` distinguishes 'not reached' from 'skipped', and a unit test in `utils/ca-soak/tests` covers the new bound
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
First recorded: 2026-09-26 (b33dee93897, by 'soak-harness-bugs-2026-09-04')
<!-- SECTION:NOTES:END -->
