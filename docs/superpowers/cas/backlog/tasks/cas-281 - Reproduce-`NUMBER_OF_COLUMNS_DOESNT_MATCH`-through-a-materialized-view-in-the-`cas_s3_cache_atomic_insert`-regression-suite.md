---
id: CAS-281
title: >-
  Reproduce `NUMBER_OF_COLUMNS_DOESNT_MATCH` through a materialized view in the
  `cas_s3_cache_atomic_insert` regression suite
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:41'
labels:
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2300'
priority: low
type: bug
ordinal: 348000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The Altinity regression suite `cas_s3_cache_atomic_insert` failed with `NUMBER_OF_COLUMNS_DOESNT_MATCH` through a materialized
view during the PR #2300 CI series (item R3 of the r2-series plan, recorded in `4344c878234`). The notes directory
`tmp/pr2300-cicd-watch/` is gone, so only the error and the suite name survive. It is not known whether CAS is involved.

Provenance: BACKLOG/formats-and-storage.md#cas-r2-r3-numcolumns-repro; the suite lives in the Altinity regression repository, not under tests/. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The statement pair is run on plain, CAS and cache-over-CAS policies, and the report says which policies fail
- [ ] #2 A failure on CAS only becomes a bug task with a stateless repro; a failure everywhere is closed as not CAS
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
First recorded: 2026-09-26 (f7e7a540506, by 'cas-r2-r3-numcolumns-repro')
<!-- SECTION:NOTES:END -->
