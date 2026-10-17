---
id: CAS-70.4
title: >-
  Fail a same-thread reentrant `CasHotKeys::submit` immediately instead of
  self-waiting
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:ref-ledger'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp
parent_task_id: CAS-70
priority: low
type: enhancement
ordinal: 88000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A `decide` or other callback that calls `submit` on the key its own thread holds waits on itself until the deadline. The spec states the callback rule; `R/Backend/CasHotKeys.cpp` does not enforce it.
A thread-local in-hold flag checked at `submit` entry makes the violation immediate.
A `LOGICAL_ERROR` aborts debug and sanitizer builds, so its test must be a `*DeathTest` suite, never `EXPECT_THROW`.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-a-followups (deferred item 2). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A reentrant `submit` from inside a hold fails at once with a `LOGICAL_ERROR` naming the key
- [ ] #2 A `CASHotKeysDeathTest` case covers it and passes on an ASan build
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

Identifier trace: the earliest docs mention of `decide` is 2026-06-03 (87be5558142); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
