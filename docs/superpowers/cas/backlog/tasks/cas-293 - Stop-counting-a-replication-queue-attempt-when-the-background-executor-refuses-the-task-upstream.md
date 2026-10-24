---
id: CAS-293
title: >-
  Stop counting a replication-queue attempt when the background executor refuses
  the task (upstream)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:replication'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'needs:repro'
milestone: m-8
dependencies: []
references:
  - src/Storages/StorageReplicatedMergeTree.cpp
  - src/Storages/MergeTree/ReplicatedMergeTreeQueue.cpp
  - src/Storages/MergeTree/BackgroundJobsAssignee.cpp
  - src/Storages/MergeTree/MergeTreeBackgroundExecutor.cpp
priority: medium
type: upstream
ordinal: 367000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`scheduleDataProcessingJob` selects a queue entry first. Selection builds `CurrentlyExecuting`, which does `++entry->num_tries`
and sets `last_attempt_time` (`src/Storages/MergeTree/ReplicatedMergeTreeQueue.cpp:2019`). It then ignores the result of all four
`schedule*Task` calls (`src/Storages/StorageReplicatedMergeTree.cpp:4390-4432`). `trySchedule` returns false when the executor is at
`max_tasks_count` (`src/Storages/MergeTree/MergeTreeBackgroundExecutor.cpp:169`).
So while the common, fetch or merge pool is full, an entry's `num_tries` grows without the entry running. `num_tries` sets the
exponential postpone after a failure (`ReplicatedMergeTreeQueue.cpp:1637`) and is what operators read in `system.replication_queue`.
Same code on upstream/master. The observation behind the roadmap bullet is not recorded.

Provenance: umbrella-roadmap.md section 4 bullet '`num_tries` when the common pool is full'; filed 2026-09-26 (user decision). The bullet was introduced in 74f60f0a3a5 with no source observation. Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild), 8d62c314ec1 (altinity/antalya-26.6) and upstream/master.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test fills the common executor and shows `num_tries` of a waiting entry does not grow while `trySchedule` refuses it
- [ ] #2 A refused schedule leaves the entry's `num_tries` and `last_attempt_time` as they were before selection
- [ ] #3 The change is prepared as an upstream pull request
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

Identifier trace: the earliest docs mention of `ReplicatedMergeTreeQueue` is 2026-06-02 (019f825a59a); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
