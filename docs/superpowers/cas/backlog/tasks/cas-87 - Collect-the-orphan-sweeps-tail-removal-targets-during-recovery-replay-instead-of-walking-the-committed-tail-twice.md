---
id: CAS-87
title: >-
  Collect the orphan sweep's tail-removal targets during recovery replay instead
  of walking the committed tail twice
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-1
dependencies:
  - CAS-86
references:
  - CA/Gc/CasOrphanManifestSweep.cpp
  - CA/Pool/CasRefProtocol.cpp
documentation:
  - docs/superpowers/cas/2026-09-04-gcs-soak-15min.md
priority: low
type: enhancement
ordinal: 122000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`activeManifestKeys` (`CA/Gc/CasOrphanManifestSweep.cpp:192`) first recovers the table through
`recoverRefTableDetailedFromAuthority` (`:208-209`), which replays every log from the checkpoint base to the committed frontier,
then walks the same range again (from `:221`) to collect `-1` manifest edges above the fold cursor. Every ref log the sweep
touches costs two GETs, and an epoch crossing wastes two windows (`wasted=127` rows). On the 2026-09-04 GCS soak the sweep
issued 814 `CASRootGet` per reduce row. GC-internal, no protocol change.

Provenance: BACKLOG/gc.md#gc-sweep-reads-the-committed-tail-twice. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The sweep reads each ref log once per page, shown by `CASRootGet` on the reduce row halving on the GCS soak shape
- [ ] #2 Protection sets equal the two-walk implementation's on the existing sweep tests
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
First recorded: 2026-09-04 (2f285dd891e, by 'gc-sweep-reads-the-committed-tail-twice')
<!-- SECTION:NOTES:END -->
