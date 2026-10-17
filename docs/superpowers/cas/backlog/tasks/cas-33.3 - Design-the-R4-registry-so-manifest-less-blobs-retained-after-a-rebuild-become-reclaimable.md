---
id: CAS-33.3
title: >-
  Design the R4 registry so manifest-less blobs retained after a rebuild become
  reclaimable
status: To Do
assignee: []
created_date: '2026-07-29'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:solid'
  - 'needs:spec'
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - docs/superpowers/cas/2031-triage.md
documentation:
  - docs/superpowers/cas/2026-09-03-test-vacuity-audit/AUDIT-B.md
parent_task_id: CAS-33
priority: low
type: design
ordinal: 43000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Since Task 11 a rebuild condemns nothing, so a blob no manifest names is retained and shows as non-draining fsck
`unaccounted`. By design and pinned by `OrphanBlobIsRetainedNotCondemned`. No substitute reclamation is allowed: guessing
from a listing reopens the r5-finding-4 vector (LIST never authorizes destruction, decision-2). Closes when an R4
registry gives a positive, point-readable record of which bodies are owned. New object kind, so decision-4 applies.

Provenance: BACKLOG/gc.md [REBUILD R4 residual — manifest-less blobs unreclaimable] (also CAS-025, CAS-075 in 2031-triage); verified 2026-09-26 against aefe80eba98 (cas-gc-rebuild) and altinity/antalya-26.6.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A spec describes the registry, its format version and compatibility path, and why it cannot delete a still-named blob
- [ ] #2 The spec is reviewed and accepted by the owner before any code
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
First recorded: 2026-07-29 (b8f5c9a8453, by 'REBUILD R4 residual — manifest-less blobs unreclaimable')
<!-- SECTION:NOTES:END -->
