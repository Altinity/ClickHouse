---
id: CAS-216
title: >-
  Make two soak diagnostics name their real cause: the B152/B185 flap warning
  and a `None` pool-size probe
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/soak/run.py
  - utils/ca-soak/soak/pool.py
priority: low
type: chore
ordinal: 274000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
1. `wait_for_pool_consistent` warns that the pool "did not HOLD dangling==0 ... after a fault window" (`utils/ca-soak/soak/run.py:1049-1054`),
   but on 2026-09-04 (phase 3, seed 20260904, binary `6ddaefbcc9e`) the flap happened in the routine `gc_checkpoint` before any fault.
   A real finding could be dismissed as the known post-restart flap.
2. `pool_size` returns `(None, None)` on empty stdout and on any exception, including a subprocess timeout, without logging (`soak/pool.py:70-76`),
   although its docstring says it logs. One `pool_bytes=None` sample under host I/O contention could not be attributed.

Provenance: BACKLOG/testing-and-ci.md#soak-harness-bugs-2026-09-04 bullets 2-3; verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The flap warning names the stage it happened in and does not claim a fault window when there was none
- [ ] #2 `pool_size` logs subprocess timeout and empty stdout as distinct causes
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
First recorded: 2026-09-26 (b33dee93897, by 'soak-harness-bugs-2026-09-04')
<!-- SECTION:NOTES:END -->
