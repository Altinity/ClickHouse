---
id: CAS-226
title: >-
  Find the test that writes `test`, `test1`, `test2` into the repo-root cwd and
  move its output to the per-test dir
status: To Do
assignee: []
created_date: '2026-07-15'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:ci'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - tests/clickhouse-test
priority: low
type: bug
ordinal: 284000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`clickhouse-local`'s default database overlays the cwd, so stray `test`/`test1`/`test2` files shadow `default.test` and fail ~19 `clickhouse-local`
tests in any full run started from a poisoned checkout. The producer was never found. The shared worktree still collects fresh untracked
test litter at the root (for example `03371_qbit_read_write_test_*.clickhouse`, `test_resize1`), not proven to be the same producer.

Provenance: BACKLOG/testing-and-ci.md [GATE-DEBRIS]; checked 2026-09-26 against 8b87aa15d21 (no debris present now).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The producing test is named and writes under its own test dir
- [ ] #2 The local praktika wrapper refuses or cleans known debris names at the repo root before a run
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
First recorded: 2026-07-15 (eddc07464bb, by 'GATE-DEBRIS')
<!-- SECTION:NOTES:END -->
