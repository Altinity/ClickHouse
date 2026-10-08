---
id: CAS-71.3
title: Space writes to a hot key on the GCS `Generation` dialect inside the lane
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:43'
labels:
  - 'area:gcs'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:protocol'
  - 'confidence:plausible'
  - 'needs:measurement'
  - 'needs:decision'
milestone: m-6
dependencies:
  - CAS-167
  - CAS-70.3
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.cpp
  - docs/superpowers/cas/BACKLOG/gcs.md#gcs-hot-control-keys-429
documentation:
  - docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md
parent_task_id: CAS-71
priority: medium
type: feature
ordinal: 94000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
GCS answers `429 SlowDown` above about one mutation per second per object; `ref_catalog` and `_ckpt` are the hot keys.
Gate: the 429/`SlowDown` count on the catalog key over the parallel stateless suite on the `gcs` lane after phase A (owner: "GCS will say if we are too fast").
Design (spec rev.26, INV-HK9, tests 17-18): `write_spacing_ms` constructor argument (`Pool` passes 1000 on `Generation`, 0 elsewhere); `Lane::last_write_end_ms` set by a no-throw guard; before the write, `op.pause` the smaller of the remainder and the leader's window; erase a lane only when its queue is empty and its last write is older than the interval.
`BACKLOG/gcs.md#gcs-hot-control-keys-429` proposes a narrower `_ckpt` coalescing (A1); reconcile the two before starting either.

Provenance: BACKLOG/performance.md#hot-key-lane-phase-b item 2. Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The gate measurement is recorded; if it is a go, spec tests 17 and 18 pass
- [ ] #2 On the `gcs` lane the catalog key shows no `429` over the parallel stateless suite
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
First recorded: 2026-09-26 (b1c34d03479, by 'hot-key-lane-phase-b item 2')
<!-- SECTION:NOTES:END -->
