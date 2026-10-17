---
id: CAS-29
title: >-
  Make GC rounds finish in minutes and catch up at S3 speed (spec stages A, B,
  C)
status: To Do
assignee: []
created_date: '2026-09-25'
updated_date: '2026-09-30 18:48'
labels:
  - 'area:gc'
  - 'complexity:epic'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
  - 'https://github.com/Altinity/ClickHouse/pull/2351'
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
priority: high
type: feature
ordinal: 35000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Epic for the plan of record in the 2026-09-25 spec. On otel.demo a round takes hours: the global ref-prefix LIST alone is
4.6M keys, ~25 min and ~1.2 GiB per round (audit F19), graduation pays a serial GET per carried row after restart (F3),
and `pending_deletes` is serial at 44 ms per blob (conclusion 4).
Stage A: parallelism with no change of decisions (A0 persist `marker_confirmed` on carry, A1 graduation gate through the
read-ahead, A2 pinned HEAD window, A3 `pending_deletes` fan-out from PR #2351, A4 manifest-body memo per round).
Stage B: discovery and cleanup O(new) (B1 per-life probe, B2 cleanup lists its own range, B3 per-row licence, B4 janitor).
Stage C: `cas_gc_round_deadline_sec` replaces the count budgets, carry resumes next round, back-to-back rounds.
Section 7 records the multi-node direction the stages must not block.

Provenance: BACKLOG/gc.md#gc-rounds-in-minutes; verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6 (not built on either). Importer: re-parent the stage tasks from other units onto this epic: u02 #gc-pending-deletes-fan-out (A3), #gc-condemn-head-read-ahead-pinned-window (A2), #gc-reduce-confirm-marker-read-ahead (A1), #covered-log-cleanup-aborts-on-catalog-etag (B3), #janitor-page-hardcoded (B4), #gc-round-budgets-not-backpressure (C), #gc-deferred-round-pays-full-list (likely MERGE into B1 below); u03 audit A0/A4 items and #gc-outcome-budget-skews-round-report-counters (C2).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every stage A, B and C item of the spec is done or explicitly deferred by the owner
- [ ] #2 On otel.demo with a 500k-blob backlog, round duration stays at the deadline and the backlog decreases linearly
- [ ] #3 After the drain, rounds take under a minute
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
2026-09-30 OWNER DECISION on the request-cuts branch: nothing shipped from it. A0 (29.3) blocked by CAS-30; A2 (29.5) rejected by measurement; memo (29.12) parked, re-measure after CAS-83 (D); janitor batches (29.9) return only with the page-cap change (C). Draft backed up at github.com/filimonov/ClickHouse cas/gc-request-cuts @1edf28f2a9a. The measured GC cost that moved in practice came from CAS-83 (PR #2452): the part-removal flow published two extra manifests per removed part.
<!-- SECTION:NOTES:END -->
