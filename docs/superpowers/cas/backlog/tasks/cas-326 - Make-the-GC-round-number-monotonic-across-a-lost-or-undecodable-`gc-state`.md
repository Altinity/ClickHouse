---
id: CAS-326
title: Make the GC round number monotonic across a lost or undecodable `gc/state`
status: To Do
assignee: []
created_date: '2026-09-30 14:20'
updated_date: '2026-09-30 14:22'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:review'
  - 'touches:protocol'
dependencies:
  - CAS-30
references:
  - >-
    docs/superpowers/specs/2026-09-30-cas-gc-meta-delete-names-its-condemnation-design.md
priority: low
type: enhancement
ordinal: 413000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Round numbers can go backwards after `gc/state` is lost. Line numbers at antalya-26.6 @e2dd0f5d343, `Gc/CasGc.cpp`: a normal round takes `state.round + 1` (`:643`) and commits by CAS on the `gc/state` etag (`:1124`); the lease acquire copies the round (`:4899-4977`); `rebuildBaseline` takes `max(max_fence_round, state.round, max_gen) + 1` (`:4681`); bootstrap on an absent `gc/state` starts at 0 (`:4909-4916`). So a rebuild from an undecodable state is bounded below only by surviving fence and generation numbers, and new rounds can be lower than rounds already stamped into blob `.meta` objects.
Why it matters: the fix of CAS-30 (spec `docs/superpowers/specs/2026-09-30-cas-gc-meta-delete-names-its-condemnation-design.md`, section 3, limit of P1/P2) compares `condemn_round` values. After such a rebuild a `.meta` delete job of a GC process that stayed frozen through the recovery carries a high round and could delete a marker written after the rebuild with a lower round. CAS-30 accepts this corner (owner decision 2026-09-30) together with an operator rule: restart every node of the pool before a rebuild that follows a lost `gc/state`.
Direction, not verified: start the round after a bootstrap or rebuild from a value that is above any round the pool could have stamped (for example derived from wall-clock time). To check first: everything that depends on the magnitude or density of round numbers (generation and fence key names and their ordering, `condemn_round < current_round` in the graduation rule, pruning of superseded generations), and the dependence on the node clock.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every use of the round number's magnitude is listed with file:line and judged for a jump in the numbering
- [ ] #2 After a bootstrap or a rebuild that followed a lost `gc/state`, the first committed round is greater than every `condemn_round` stored in the pool's blob `.meta` objects, shown by a test
- [ ] #3 No on-S3 format change
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
Correction after the external review of the CAS-30 spec, checked against `Gc/CasGc.cpp` at e2dd0f5d343 on 2026-09-30: `max_fence_round` is declared as 0 (`:4515`) and never updated, so it contributes nothing to the rebuild round (`:4681`). `acquireOrRenewLease` decodes an existing `gc/state` unconditionally (`:4919`), so a rebuild does not get past an undecodable state through this path. A regression therefore needs `gc/state` to be ABSENT (bootstrap from 0) and the surviving generation numbers (`max_gen`) to be lower than rounds already stamped into `.meta`; what `max_gen` preserves after a state loss is not established. First step of this task: establish whether a regression is reachable at all.
<!-- SECTION:NOTES:END -->
