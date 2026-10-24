---
id: CAS-29.7
title: >-
  Split `authorityHolds` refusals into ProfileEvents and measure how many
  cleanup aborts had an intact per-namespace licence (spec B3 steps 0-1)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:review'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - >-
    docs/superpowers/reports/2026-09-15-cas-gc-dead-namespace-debris-cleanup-codex-reviews/review_r2.md
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#b3-cleanup-licence
parent_task_id: CAS-29
priority: high
type: spike
ordinal: 113000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Before its first chunk `cleanupRefObjects` requires `current_catalog.etag == folded.catalog_cut->etag`, equality of the whole
pool catalog with the fold's cut (`CA/Gc/CasGc.cpp:3586-3597`, `authorityHolds`), and one refusal stops the whole pass (`:3690`).
Any CREATE or DROP of another table between fold and cleanup changes the etag. Run 8 of the msan rig: of 85
`ref_object_cleanup` rows, 37 read nothing (no checkpoint), 40 built a chunk and were refused at the catalog comparison,
8 deleted something (7 at the 5,000 cap). The loop: longer round, more aborts, more listed keys, longer round.
Step 0: from run 8's Phase rows, the window between the fold's cut and cleanup against the lane's CREATE/DROP rate.
Step 1: ProfileEvents per stop reason (etag only with row and life unchanged / row or life changed / `gc/state` absent or
moved) plus planned-but-undeleted objects per round; one 60-90 min local ca-s3 stateless run in a release build.
Relaxing the etag comparison is not allowed at this step.

Provenance: BACKLOG/gc.md#covered-log-cleanup-aborts-on-catalog-etag plan steps 0-1. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (etag comparison identical).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Three stop-reason ProfileEvents and a planned-not-deleted counter exist and appear on the `ref_object_cleanup` phase row
- [ ] #2 A written result gives the share of refusals whose row and life were unchanged, and the undeleted volume per run
- [ ] #3 The result states whether step 2 (licence adjudication) is justified
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

Identifier trace: the earliest docs mention of `cleanupRefObjects` is 2026-07-12 (d372bf410d4); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
