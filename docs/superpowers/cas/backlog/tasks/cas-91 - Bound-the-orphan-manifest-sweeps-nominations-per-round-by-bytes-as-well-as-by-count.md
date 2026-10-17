---
id: CAS-91
title: >-
  Bound the orphan-manifest sweep's nominations per round by bytes as well as by
  count
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'origin:review'
dependencies: []
references:
  - CA/Gc/CasOrphanManifestSweep.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f29
priority: low
type: enhancement
ordinal: 126000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`planManifestCursorPage` caps nominations by count only (`nomination_budget`, `CA/Gc/CasOrphanManifestSweep.cpp:671,738`;
`manifest_sweep_delete_budget_keys` default 100). Manifests may be up to the stored-object limit, so a page of large bodies
is unbounded in bytes per round (the source estimates ~25 GiB at 256 MiB manifests). Observed manifests are small
(otel.demo p99 68 KiB, max 912 KiB, audit F29), so first confirm which sweep step reads bodies and how much per round.

Provenance: BACKLOG/gc.md [orphan-sweep-byte-budget]. Verified 2026-09-26 against 59494ebf366 (count-only budget).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A note states which sweep step reads nominated bodies and the worst-case bytes per round at default settings
- [ ] #2 If the worst case exceeds the round's memory budget, a byte cap exists and a gtest with oversized manifests shows it
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
First recorded: 2026-08-04 (f08734d17df, by 'orphan-sweep-byte-budget')
<!-- SECTION:NOTES:END -->
