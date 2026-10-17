---
id: DRAFT-2
title: >-
  Concurrent per-partition `commitPart` in `ReplicatedMergeTreeSink` (stage 2,
  postponed)
status: Draft
assignee: []
created_date: '2026-07-24'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'needs:measurement'
  - 'origin:review'
dependencies:
  - CAS-17
references:
  - src/Storages/MergeTree/ReplicatedMergeTreeSink.cpp
documentation:
  - >-
    docs/superpowers/cas/history/2026-09-26-stage2-concurrent-commitpart-hazards.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Postponed by user decision 2026-07-24: "too strong/unpredictable an effect on upstream/generic code". Revisit only with an explicit go-ahead.
Design: bounded concurrent dispatch of `ReplicatedMergeTreeSink`'s per-partition commit (`max_concurrent_part_commits_per_insert`, default 1 = dormant), ~100-150 lines; a bounded worker pool sized by `cas_commit_concurrency` avoids deadlock. Absent on both branches.
Scoping: (a) `ReplicatedMergeTreeSink` only, (b) non-replicated leg, (c) `MergeTreeSink`. MUST: the per-iteration reset of `deduplication_async_inserts_cache_version` is a shared member (`ReplicatedMergeTreeSink.cpp:475`) and must become per-task first.
Precondition: remeasure. Even a perfect stage 2 was not expected to reach 1.0x because blob presence checks took ~12% of wall; redo that estimate under the mandatory-HEAD protocol.

Provenance: BACKLOG/performance.md#stage2-concurrent-commitpart-postponed + [cas-commit-pool-anti-deadlock]. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 User go-ahead recorded
- [ ] #2 The hazard inventory (shared Keeper session, shared dedup caches, quorum ordering) is closed with a test per hazard
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
First recorded: 2026-07-24 (d7085bb9f2a, by 'stage2-concurrent-commitpart-postponed')
<!-- SECTION:NOTES:END -->
