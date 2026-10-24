---
id: DRAFT-36
title: >-
  Add a deterministic backoff seam to `CasRequests` if a jitter-dependent test
  red appears
status: Draft
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:testing'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:speculative'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
priority: low
type: enhancement
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Retry::backoff` draws full jitter with no test seam, so a test that drives an ambiguity under `untilLeaseSafe` with a short remaining lease
is a coin flip. A `setBackoffFnForTest` on `CasRequests` (used by `pauseAndReissue`) would make it deterministic.
No jitter-dependent red has been recorded since the idea was raised (2026-09-03); build it the first time one appears.

Provenance: BACKLOG/testing-and-ci.md 'Engine test seam' bullet (CP3/Task 7 review 2026-09-03); verified 2026-09-26 against 8b87aa15d21 (no seam exists).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A recorded jitter-dependent red exists before this is promoted to a task
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
First recorded: 2026-09-26 (5245fbc7a76, by 'unit-test-gate')
<!-- SECTION:NOTES:END -->
