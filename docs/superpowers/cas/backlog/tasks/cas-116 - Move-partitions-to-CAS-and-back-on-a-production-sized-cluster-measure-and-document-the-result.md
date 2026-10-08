---
id: CAS-116
title: >-
  Move partitions to CAS and back on a production-sized cluster, measure, and
  document the result
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:soak'
  - 'area:docs'
  - 'complexity:large'
  - 'risk:medium'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:review'
milestone: m-0
dependencies: []
references:
  - docs/en/antalya/cas/operations/migration.md
documentation:
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: task
ordinal: 154000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Roadmap §1 blocks the first deployment on "migration tested at scale in both directions"; §7 lists "Migration at scale" open.
`docs/en/antalya/cas/operations/migration.md` documents `ALTER TABLE ... MOVE PARTITION` onto CAS and rollback, but no run at
realistic size exists. `MOVE PARTITION` to a CAS disk re-packs every part, so its request count, duration and GC load on the
pool are unknown at scale. The mixed-version rollout half of the source item moved to `format-version-rollout-design`.

Provenance: BACKLOG/operability-and-introspection.md#b13-migration-path ([B13]); verified 2026-09-26 against dd0ed2f263a.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A recorded run moves at least one multi-TB table onto CAS and back, with duration, S3 request counts and GC round times
- [ ] #2 Row counts and checksums match before and after each direction, and fsck is clean
- [ ] #3 `migration.md` states the measured throughput and any limits found
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
First recorded: 2026-09-26 (4afa7f68ee1, by 'b13-migration-path')
<!-- SECTION:NOTES:END -->
