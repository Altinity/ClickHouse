---
id: CAS-53
title: >-
  Classify per-key `DeleteObjects` errors with the core AWS error mapper in
  `deleteFilesFromS3`
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:upstream'
  - 'complexity:trivial'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies: []
references:
  - src/IO/S3/deleteFileFromS3.cpp
  - src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
priority: low
type: upstream
ordinal: 63000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`src/IO/S3/deleteFileFromS3.cpp:201` maps each per-key error of a `DeleteObjects` reply with `S3ErrorMapper::GetErrorForName` alone.
That mapper knows only S3-specific names, so a service-wide name such as `AccessDenied` comes back `UNKNOWN`.
Message and code text are right; only the `S3Errors` enum callers may match on is wrong.
The CAS path already does the two-step lookup (S3 mapper, then `Aws::Client::CoreErrorsMapper`, the order `S3ErrorMarshaller::Marshall` uses)
in `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:152-162`. The generic path is shared upstream code: a separate small PR.

Provenance: BACKLOG/operability-and-introspection.md#delete-files-from-s3-generic-error-classification (found 2026-09-08); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A `DeleteObjects` per-key `AccessDenied` error is classified as `ACCESS_DENIED`, not `UNKNOWN`, on the generic path
- [ ] #2 The fix ships as its own upstream pull request with a test
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
First recorded: 2026-09-26 (2b987d9c55e, by 'delete-files-from-s3-generic-error-classification')
<!-- SECTION:NOTES:END -->
