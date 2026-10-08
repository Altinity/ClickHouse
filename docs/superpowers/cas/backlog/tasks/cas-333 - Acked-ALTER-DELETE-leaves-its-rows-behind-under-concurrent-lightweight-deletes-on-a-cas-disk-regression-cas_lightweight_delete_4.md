---
id: CAS-333
title: >-
  Acked ALTER DELETE leaves its rows behind under concurrent lightweight deletes
  on a cas disk (regression cas_lightweight_delete_4)
status: Done
assignee: []
created_date: '2026-09-30 21:17'
updated_date: '2026-09-30 21:19'
labels:
  - cas
  - ci
  - mutations
dependencies: []
priority: high
ordinal: 422000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Regression job `cas_lightweight_delete_4` (TestFlows, `/lightweight delete/concurrent delete/MergeTree/concurrent delete without overlap with alter delete`, `concurrent_delete.py:155`) fails intermittently on `antalya-26.6`: every lightweight `DELETE` and every `ALTER ... DELETE` ran with `mutations_sync = 2` and returned 0, the even-row checks read 0, yet the final `count(*)` is 50000 instead of 0 - the rows an acknowledged `ALTER DELETE` mutation targeted survive. Server error log for the window: only Code 236/341 "Cancelled mutating parts"; no port exhaustion, no lease loss.

Seen on: PR #2452 (run 36750016225; RCA in session scratch `tmp/ci-2452-lwd4/rca.md`), PR #2437 (run 36164109736, two "without overlap" scenarios), PR #2440 (`cas_s3_cache_lightweight_delete_4`, surplus rows 1 and 1). The changes of those PRs do not touch mutations. Evidence gap: the uploaded server logs start after the failing window, so the mutation/part lines for the table are missing.

This is a correctness symptom (an acked mutation whose effect is not visible), not a flake to re-run away. Next: reproduce locally with the TestFlows scenario against a `cas` disk (loop), keep the full server log, and find whether the mutation's result part is published and made active, whether "Cancelled mutating parts" is the same mutation, and whether the acked wait (`mutations_sync = 2`) returns before the part swap on a `cas` disk.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Local reproduction with the full server log of the failing window
- [ ] #2 Root cause named: which step lets an acked mutation's rows survive (publish, activate, or the sync wait)
- [ ] #3 Fix or a named upstream defect; the regression scenario passes 20 times in a row
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
2026-09-30 OWNER: duplicate of the known mutation-registration race, CAS-252 — upstream fix https://github.com/ClickHouse/ClickHouse/pull/122934 (open), Altinity issue https://github.com/Altinity/ClickHouse/issues/2331, fork branch feature/antalya-26.6/fix-mutation-registration-race. The cas_lightweight_delete survivors are the documented CI signature of that race (CAS-252 notes: 'observed in CI as the cas_lightweight_delete survivors of PR #2300'). Closed as duplicate; the #2452 occurrence is recorded in CAS-252.
<!-- SECTION:NOTES:END -->
