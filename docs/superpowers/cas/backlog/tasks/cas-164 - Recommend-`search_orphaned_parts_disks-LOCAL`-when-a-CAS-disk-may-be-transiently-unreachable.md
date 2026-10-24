---
id: CAS-164
title: >-
  Recommend `search_orphaned_parts_disks = LOCAL` when a CAS disk may be
  transiently unreachable
status: To Do
assignee: []
created_date: '2026-07-23'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:docs'
  - 'area:mounts'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
milestone: m-7
dependencies: []
documentation:
  - docs/en/antalya/cas/configuration.md
  - docs/en/antalya/cas/operations/troubleshooting.md
priority: low
type: docs
ordinal: 210000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
With `search_orphaned_parts_disks = ANY`, a transiently unreachable CAS disk strands the load of an unrelated table. Accepted
residual of the rev.8 lifecycle round (spec §4 blast radius); the cure is `ATTACH` or a restart. The guidance to keep `LOCAL`
appears nowhere under `docs/en`.

Provenance: BACKLOG/mounts-and-lifecycle.md#disk-lifecycle-rev8-closure (accepted residual); verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The CAS configuration or troubleshooting docs recommend `LOCAL` and name the `ATTACH`/restart cure
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
First recorded: 2026-07-23 (87aeefac9bd, by 'disk-lifecycle-rev8-closure')
<!-- SECTION:NOTES:END -->
