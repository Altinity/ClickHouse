---
id: CAS-29.10
title: >-
  Bound a GC round by `cas_gc_round_deadline_sec` and start the next round at
  once while carry remains (spec C1, C3)
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-29.2
  - CAS-29.4
  - CAS-29.6
  - CAS-29.9
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGcScheduler.h
  - CA/ContentAddressedSettings.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#stage-c
  - docs/en/antalya/cas/architecture/garbage-collection.md
  - docs/en/operations/system-tables/cas_gc_log.md
parent_task_id: CAS-29
priority: high
type: feature
ordinal: 116000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
No GC round has a time deadline; count caps are the surrogate and are not backpressure. Excess graduation and redelete work
is carried in `still_retired`, which the next round reads in full, so round cost grows with the debt while useful work stays
capped (class A). The server defaults are still 5000 (`CA/ContentAddressedSettings.cpp:65-69`); on otel.demo rounds grew from
98 s to 2,800 s over eight days with deletes pinned at 5,000 per round (#2429). A round that outruns its lease is fenced.
C1: `cas_gc_round_deadline_sec` (default 300) from round start; never cut the probes, the intake of every live life, the
reduce merge, the seal, the `gc/state` CAS, the prune cursor logic and the one-shot hand-off reclaim; cut graduation,
redelete, `ref_object_cleanup`, janitor and orphan sweep by `remaining()` before each window or chunk (their remainder is durable).
C3: a round that ends with `deadline_hit` and non-empty carry calls `requestRoundSoon`; `gc_interval_sec` applies only when carry is empty.
Rejected: backing off rounds when store delete latency is high; the deadline bounds round length instead.
Phase rows gain `deadline_hit` and `carried`; the round row gains `deadline_hit` and `carry_total` (spec C5).

Provenance: BACKLOG/gc.md#gc-round-budgets-not-backpressure (classes A, C) and #gc-budgets-need-a-deadline; janitor-page-hardcoded ask 4. `pending_reclaim` from the seal (spec C5) is CAS-1.3. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792: no deadline symbol on either.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A fake-clock gtest: a round with large carry stops each cuttable family at the deadline, commits, and the next round resumes from durable carry; intake and seal are never skipped
- [ ] #2 `deadline_hit` with carry schedules the next round without the interval; empty carry waits the interval
- [ ] #3 With a 500k-blob backlog on the stand, round duration stays at the deadline and the backlog falls linearly
- [ ] #4 After the drain, rounds take under a minute
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
First recorded: 2026-08-04 (9c1f6e8a741, by 'gc-budgets-need-a-deadline')
<!-- SECTION:NOTES:END -->
