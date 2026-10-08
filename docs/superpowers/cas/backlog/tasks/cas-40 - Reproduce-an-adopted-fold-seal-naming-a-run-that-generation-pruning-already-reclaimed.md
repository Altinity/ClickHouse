---
id: CAS-40
title: >-
  Reproduce an adopted fold seal naming a run that generation pruning already
  reclaimed
status: To Do
assignee: []
created_date: '2026-07-25'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - CA/Gc/CasGc.cpp
priority: medium
type: research
ordinal: 50000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Observed 2026-07-25: an adopted seal named a run that `pruneSupersededGenerations` had reclaimed. It is silent by policy:
the round reclaims nothing for that shard. The guard now protects every generation named by the new fold seal and by the
parent seal (`referenced_generations`, `CA/Gc/CasGc.cpp:966-980`), but nothing establishes whether prune raced adopt
(for example a losing leader's pre-CAS prune against a winner's newer seal). Needs a targeted test.

Provenance: BACKLOG/gc.md#adopted-seal-pruned-run-2026-07-25; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test drives two leaders across a lease handover and asserts no adopted seal ever names a pruned run
- [ ] #2 If the race exists, a fix is proposed with that test failing first
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
First recorded: 2026-07-25 (30134ea3224, by 'adopted-seal-pruned-run-2026-07-25')
<!-- SECTION:NOTES:END -->
