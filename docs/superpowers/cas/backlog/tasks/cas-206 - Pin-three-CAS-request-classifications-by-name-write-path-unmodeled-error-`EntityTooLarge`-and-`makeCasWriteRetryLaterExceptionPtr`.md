---
id: CAS-206
title: >-
  Pin three CAS request classifications by name: write-path unmodeled error,
  `EntityTooLarge`, and `makeCasWriteRetryLaterExceptionPtr`
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:testing'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - src/Disks/tests/gtest_cas_requests.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
priority: low
type: task
ordinal: 263000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Left by the deletion of `gtest_cas_request_control.cpp`. Each shares its path with a tested sibling, so none is a live risk, but none is pinned:
1. no write-path twin of `CASRequests.AnUnmodeledStoreErrorOnAReadIsReissuedNotSurfaced` (`T/gtest_cas_requests.cpp:2018`) shows an unmodeled or `SlowDown` `S3Exception` falling to ambiguity, not `Refused`;
2. `EntityTooLarge` is not tested by name (`isEntityTooLargeError` in the refusal chain at `CA/Backend/CasRequests.cpp:240`; siblings `MalformedXML`/`AccessDenied` are);
3. `makeCasWriteRetryLaterExceptionPtr` (`CA/Backend/CasRequests.h:89`) has no test against `throwCasWriteRetryLater` (`:83`).

Provenance: BACKLOG/testing-and-ci.md#request-control-classification-gaps; verified 2026-09-26 against 8b87aa15d21.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Three new tests next to their siblings in `gtest_cas_requests.cpp`, the third next to `CASWriteResult.OrThrowMapsEveryAlternative`
- [ ] #2 Each test fails when its classification is changed
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
First recorded: 2026-09-04 (229f9e617e8, by 'request-control-classification-gaps')
<!-- SECTION:NOTES:END -->
