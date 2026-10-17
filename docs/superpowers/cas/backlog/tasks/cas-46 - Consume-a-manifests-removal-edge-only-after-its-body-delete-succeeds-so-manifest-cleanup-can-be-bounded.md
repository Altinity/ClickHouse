---
id: CAS-46
title: >-
  Consume a manifest's removal edge only after its body delete succeeds, so
  manifest cleanup can be bounded
status: To Do
assignee: []
created_date: '2026-08-03'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
priority: medium
type: enhancement
ordinal: 56000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The post-CAS `manifest_deletes` phase is one-shot: the intake cursor that discovered each `-1` commits in the same round's
CAS, before the deletes run, so a declined entry is never re-derived. The phase is therefore deliberately unbudgeted
(`CA/Gc/CasGc.cpp:1094-1100`). A 24 h soak with a 5000 cap left 112,518 entries skipped, 110,218 unreachable (FAIL);
uncapped, 223,714 drained in-round (PASS); the knob was removed by owner decision. The only other reclaimer, the orphan
sweep, drains ~100 objects per round. Spec C1's deadline does not list this phase, so a round cannot stop inside it.
Fix: durable retry, moving edge consumption after the delete so a deferred entry stays discoverable.

Provenance: BACKLOG/gc.md#gc-mf-cleanup-durable-retry (soak-t6b-report); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A round cut by the deadline inside manifest cleanup leaves the rest discoverable, and the next round deletes it
- [ ] #2 A soak burst of 200k owner-removed manifests drains with zero unreachable at checkpoint
- [ ] #3 A backend error in the post-CAS tail (GC cleanup after the round commit) is recorded and does not fail the committed round
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
First recorded: 2026-08-03 (d63e0d1f5b3, by 'gc-mf-cleanup-durable-retry')
<!-- SECTION:NOTES:END -->
