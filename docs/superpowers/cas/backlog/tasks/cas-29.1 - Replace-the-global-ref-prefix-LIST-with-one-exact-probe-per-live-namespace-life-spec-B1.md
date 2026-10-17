---
id: CAS-29.1
title: >-
  Replace the global ref-prefix LIST with one exact probe per live namespace
  life (spec B1)
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:2031-triage'
  - 'theme:parity'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Backend/CasObjectStorageBackend.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-29
priority: high
type: feature
ordinal: 36000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`listRefPrefix` is one LIST of `<prefix>/cas/ns/stream/`, O(all `_log` keys). On otel.demo that is 4.6M keys, ~1575 s and
~1.2 GiB resident per round (audit F19); earlier it was 79% of GC wall time (2026-08), the audit re-measures 30-42%.
The S3 async iterator also schedules one global-pool job per page, ~1,500 short-lived threads per round (audit F24).
B1: take `Live`/`Removing` lives from the catalog cut, issue one exact GET of `refLogKey(life, last_folded + 1)` per life
on the round's io pool (404 = quiet), and LIST only a hot life's stream from `start-after` = its covered floor.
`shouldDeferRound` is decided from the probes. LIST is not consulted for the verdict, so decision-2 is untouched.
Before reusing the LIST path, confirm `max_keys` propagation into `S3ObjectStorage::iterate` (audit F6: ~3 requests per
1000-key page).

Provenance: BACKLOG/gc.md#gc-defer-decision-list-cost and [gc-frontier-one-list]; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6: global LIST still in place on both.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A pool with K live lives and M `_log` keys below every floor: `defer_decision` issues exactly K exact GETs and no LIST when nothing changed
- [ ] #2 A hot life costs one LIST page starting at its floor; the DEFER verdict and `ref_tables` equal the global-LIST oracle's for the same state
- [ ] #3 A `_log` key above the floor omitted by an injected LIST omission still folds; a key below the floor is never touched
- [ ] #4 On otel.demo `defer_decision` wall time no longer grows with `_log` keys below the floor of quiet lives
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
Rejected alternative (from gc.md, deferred rounds pay the full LIST): a cross-round index of dead lives does NOT help by itself; S3 still walks their keys, only the parsing is saved.

First recorded: 2026-07-29 (539584ac324, by 'GC-DEFER-DECISION-LIST-COST')

2031-triage CAS-035: the audit's 'a GC round fully lists `cas/ns/stream/` and retains every key in memory, also on rounds that then defer' is this LIST; the edge-run memory half is CAS-88.
<!-- SECTION:NOTES:END -->
