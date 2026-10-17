---
id: CAS-63
title: Give the ca-soak fsck wait helpers honest timeouts and current finding labels
status: To Do
assignee: []
created_date: '2026-07-26'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - utils/ca-soak/soak/run.py
  - utils/ca-soak/soak/checker.py
priority: low
type: chore
ordinal: 77000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The main checkpoint gate already degrades honestly (600 s budget, `_fabricated` marker, `GATE-SKIPPED` line). Two helpers were not updated:
`wait_for_pool_consistent` and `settle_fsck_for_dump` still default `timeout_s=180.0` (`utils/ca-soak/soak/run.py:997`, `:1079`), and their
callers (`:1590`, `:1744`, `:1956`, `:2050`) pass none. On a large pool they time out where the main gate would not.
Docstrings in `utils/ca-soak/soak/checker.py:108,292,545` still call the residual "M-F debris, B140", which the product now classifies as `AwaitingGc`.

Provenance: BACKLOG/operability-and-introspection.md#fsck-large-pool-fixed (a), (b), (c) (2031-triage); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Every `timeout_s` default in the harness is either sized for the current backlog or degrades to an explicit skipped-gate line, never to fabricated values
- [ ] #2 No harness docstring uses the B140 label for `awaiting-gc`
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
First recorded: 2026-07-26 (a285b399be6, by 'fsck-large-pool-fixed')
<!-- SECTION:NOTES:END -->
