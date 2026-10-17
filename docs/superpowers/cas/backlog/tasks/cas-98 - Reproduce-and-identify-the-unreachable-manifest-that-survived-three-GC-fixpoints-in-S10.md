---
id: CAS-98
title: >-
  Reproduce and identify the unreachable manifest that survived three GC
  fixpoints in S10
status: To Do
assignee: []
created_date: '2026-08-31'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:gc'
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
dependencies:
  - CAS-7
references:
  - utils/ca-soak/scenarios/cards/s09_s11_mutations.py
  - utils/ca-soak/scenarios/RUN_HISTORY.md
priority: medium
type: bug
ordinal: 136000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S10 at `--scale full` (2026-08-31, `7278a1e183f`) ended with fsck `unreachable: 1, dangling: 0` of 163 reachable, classified `leak` as `unreachable:_manifests: 1`; `reclaimable_drain_check` called it reclaimable and `gc_fixpoint_history = [1, 1, 1]`.
The residual came from the lightweight-delete workload. `graduation_drain_history` in the same run had one 128 in a run of ones.
One full S10 run after `0aa331b8c7a` passed, which does not rule out a rare residual.
Before calling it a defect: name the object and what points at it; a manifest published after the fold's coverage seal is bookkeeping, not a leak. Derive the verdict from `cas_gc_log` and fsck, not `cas_log`.

Provenance: BACKLOG/performance.md#s10-manifest-residual. Verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S10 at `--scale full` is run at least three times on HEAD and every run's fsck residual is recorded
- [ ] #2 Any surviving unreachable manifest is identified by key, with its publisher and why GC did not reclaim it
- [ ] #3 The outcome is either a filed GC bug with a failing test, or a documented bookkeeping classification the card accepts
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
First recorded: 2026-08-31 (c75b227db67, by 's10-manifest-residual')
<!-- SECTION:NOTES:END -->
