---
id: CAS-174
title: >-
  Give the soak harness one owner for container names and resolve them at call
  time
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:soak'
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
dependencies: []
references:
  - utils/ca-soak/soak/chaos.py
  - utils/ca-soak/soak/pool.py
  - utils/ca-soak/scenarios/framework/observe.py
  - utils/ca-soak/scenarios/framework/lifecycle.py
  - tests/integration/test_gcs_live/test.py
priority: low
type: chore
ordinal: 227000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Default container names are duplicated in `soak/chaos.py` (`_DEFAULT_CONTAINER`, :35), `soak/pool.py` (:37) and `scenarios/framework/observe.py` (:38, :56).
`observe.RUSTFS_CONTAINER` is an import-time constant still used directly by `s15_s18_shards_lifecycle.py:156`, `s28_s33_corner.py:472`, `s34_s35_d1_churn.py:218` and `scripts/t8_s44_stuck_removing_discrimination.py:32`, although `observe.rustfs_container()` exists for exactly this.
`lifecycle.DEFAULT_FSCK_CONTAINER` (`scenarios/framework/lifecycle.py:18`) has no user.
The `object_kind != 'none'` filter in `test_gcs_live` (`test.py:554`) has no stub-driven local coverage.

Provenance: BACKLOG/gcs.md#environment 'Harness debt'. Verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 One module owns the default container names; every caller resolves them at call time
- [ ] #2 `RUSTFS_CONTAINER` and `DEFAULT_FSCK_CONTAINER` are removed; `utils/ca-soak/tests` pass
- [ ] #3 A local stub test covers the `object_kind != 'none'` filter
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

Identifier trace: the earliest docs mention of `observe.RUSTFS_CONTAINER` is 2026-09-02 (9f87032fde6); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
