---
id: CAS-29.8
title: >-
  Adjudicate and implement a per-life cleanup licence so one namespace's churn
  stops refusing every cleanup (spec B3)
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'origin:review'
  - 'origin:2031-triage'
milestone: m-1
dependencies:
  - CAS-29.7
  - CAS-29.6
references:
  - CA/Gc/CasGc.cpp
  - docs/superpowers/cas/2031-triage.md#cas-079
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#b3-cleanup-licence
  - >-
    docs/superpowers/specs/2026-09-15-cas-gc-dead-namespace-debris-cleanup-design.md
parent_task_id: CAS-29
priority: high
type: design
ordinal: 114000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The whole-catalog etag equality in `authorityHolds` licenses irreversible deletes; its comment calls the strictness deliberate
("the SAME complete catalog observation and GC lease that adopted the fold"). After B2 the cleanup no longer uses the fold-time
listing: its inputs are a catalog cut, the adopted `gc/state` and the life's own checkpoint.
Hypothesis to adjudicate, not to assume: the cleanup takes its own fresh catalog cut at phase start; each chunk is licensed by
row value, life resolution (`throwIfAmbiguous`) and `gc/state` owner + `seq`; a refusal is scoped to its namespace and the pass
continues. It is also the only licence shape that survives multi-node execution.
Rejected: relaxing the etag check directly. Constraints: the round stays one-pass, LIST trust is not reopened (decision-2),
no new object kinds (decision-4). Process: ca-arch with the code, then codex review capped at three rounds, then code.
Proceed only if the measurement subtask shows etag-only refusals dominate.

Provenance: BACKLOG/gc.md#covered-log-cleanup-aborts-on-catalog-etag (plan step 2 and 'not to do'); formerly {#ref-cleanup-whole-catalog-token-stillness}/2031-triage CAS-079. u01's pointer bullet merges here. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A written adjudication answers whether the fold seal or this life's coverage depends on the rest of the cut, reviewed to no MAJOR
- [ ] #2 A CREATE of an unrelated table between fold and cleanup no longer refuses the pass
- [ ] #3 A changed row or life of the cleaned namespace refuses that namespace only; a moved `gc/state` owner or seq refuses everything
- [ ] #4 On the ca-s3 lane, refusals whose row and life were unchanged are zero
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-079')

2031-triage CAS-079: label added; the audit ranks CAS-079 #3 in 'Where to start'.
<!-- SECTION:NOTES:END -->
