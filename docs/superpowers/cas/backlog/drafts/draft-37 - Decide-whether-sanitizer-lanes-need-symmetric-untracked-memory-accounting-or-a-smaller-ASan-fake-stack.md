---
id: DRAFT-37
title: >-
  Decide whether sanitizer lanes need symmetric untracked-memory accounting or a
  smaller ASan fake stack
status: Draft
assignee: []
created_date: '2026-09-16'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:ci'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:issue'
dependencies:
  - CAS-222
  - CAS-223
references:
  - src/Common/memory.h
  - src/Common/MemoryWorker.cpp
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Two untried levers from the ASan memory-ceiling RCA, both orthogonal to the `MemoryWorker` fix and the thread-pool profile:
(b) without jemalloc, `untrackMemory` in `src/Common/memory.h` subtracts `malloc_usable_size` on an unsized delete, which the file itself calls
inaccurate under sanitizers, so the tracker drifts below zero; (d) `max_uar_stack_size_log=18` (default 20, ~11 MB per thread) shrinks the ASan fake stack
without disabling use-after-return detection. Revisit only if the ASan lane still hits its ceiling after the two tasks it depends on.

Provenance: BACKLOG/testing-and-ci.md#asan-memory-tracker-snap candidates (b) and (d), alias [asan-thread-count-fake-stack-ceiling]; verified 2026-09-26 (neither lever configured anywhere under tests/config or ci).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 After the fix port and the thread profile, one ASan CAS run shows whether RSS or tracker drift still reaches the limit
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
First recorded: 2026-09-16 (cbf3fe14e36, by 'asan-memory-tracker-snap')
<!-- SECTION:NOTES:END -->
