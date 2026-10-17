---
id: CAS-90
title: >-
  Design multi-node GC: one coordinator round with per-life and per-shard work
  claimed by executors
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:gc'
  - 'complexity:epic'
  - 'risk:high'
  - 'touches:protocol'
  - 'touches:on-s3-format'
  - 'confidence:plausible'
  - 'needs:spec'
  - 'origin:review'
dependencies:
  - CAS-29.10
  - CAS-29.8
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGcShardPlan.h
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#multi-node-direction
priority: low
type: design
ordinal: 125000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Attempt-scoped generations, the prerequisite, are done; the multi-worker claim and scheduler are not built.
Spec section 7 records the target: one leader as coordinator, one seal and one `gc/state` CAS; work units per life (probe,
intake into per-shard delta runs, cleanup), per blob-hash shard (reduce, graduation, redelete, outcomes, markers) and
coordinator-only (cut, lease, barriers, seal, prune, hand-off). Resume of an interrupted round is the same change: a
per-life completion marker in the attempt prefix is both the executor's done record and what lets a fresh attempt adopt
finished phases (two restarts during intake cost four hours each on otel.demo, 2026-09-25).
Claim and done markers are new object kinds: new format version and compatibility path (decision-4).

Provenance: BACKLOG/gc.md [distributed gc_shards>1 parallel GC] (u01's pointer bullet merges here). Roadmap §2 'Multi-node GC (later)'.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec defines work units, claim and done objects, barriers, failure and resume semantics, reviewed to no MAJOR
- [ ] #2 The spec shows every stage A/B/C constraint of section 7 still holds
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'distributed gc_shards>1 parallel GC')
<!-- SECTION:NOTES:END -->
