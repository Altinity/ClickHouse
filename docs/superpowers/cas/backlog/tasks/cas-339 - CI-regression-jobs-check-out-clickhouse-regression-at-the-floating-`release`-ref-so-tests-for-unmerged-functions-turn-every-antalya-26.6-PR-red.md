---
id: CAS-339
title: >-
  CI: regression jobs check out clickhouse-regression at the floating `release`
  ref, so tests for unmerged functions turn every antalya-26.6 PR red
status: To Do
assignee: []
created_date: '2026-10-01 17:03'
updated_date: '2026-10-01 19:29'
labels:
  - ci
dependencies: []
priority: low
ordinal: 432000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Observed 2026-10-01 on PR #2463 (also any PR on antalya-26.6 after 05:10Z): cas_aggregate_functions_1 and cas_s3_cache_aggregate_functions_1 fail on release and aarch64 because clickhouse-regression commit 57cff3f90a added uniqApacheHLL coverage, and the function exists only in the open PR #2398. 3194 UNKNOWN_FUNCTION failures per variant, everything else passes or is XFail; #2455 ran on regression 1029b41ba4 and passed. Evidence: tmp/ci-2463/rca.md. Fix: pin the regression checkout to a commit (bump deliberately) or make the regression suite gate new-function tests on the server version; until then every antalya-26.6 PR shows these two reds.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Regression workflow checks out clickhouse-regression at a pinned commit or the suite skips tests for functions the server does not have
- [ ] #2 cas_aggregate_functions_1 green on a PR without #2398
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
2026-10-01 owner: not ours — the clickhouse-regression maintainers own the release ref; the reds disappear once PR #2398 (uniqApacheHLL) merges. Kept only as the explanation for cas_aggregate_functions_* reds on antalya-26.6 PRs in the meantime.
<!-- SECTION:NOTES:END -->
