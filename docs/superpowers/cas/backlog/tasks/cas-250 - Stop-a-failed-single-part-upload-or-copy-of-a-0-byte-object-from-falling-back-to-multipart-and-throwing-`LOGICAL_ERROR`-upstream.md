---
id: CAS-250
title: >-
  Stop a failed single-part upload or copy of a 0-byte object from falling back
  to multipart and throwing `LOGICAL_ERROR` (upstream)
status: To Do
assignee: []
created_date: '2026-09-03'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:upstream'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-5
dependencies: []
references:
  - src/IO/S3/copyS3File.cpp
  - src/IO/AzureBlobStorage/copyAzureBlobStorageFile.cpp
  - 'https://github.com/Altinity/clickhouse-regression/actions/runs/33560831028'
priority: medium
type: upstream
ordinal: 315000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`src/IO/S3/copyS3File.cpp:526` and `:751` call `performMultipartUpload` / `performMultipartUploadCopy` after `EntityTooLarge`,
`InvalidRequest`, `InvalidArgument`, `AccessDenied` or a GCS-rewrite hint without re-checking the size. A 0-byte object always
takes the single-shot path, so its fallback reaches `calculatePartSize(0)` and throws "Chosen multipart upload for an empty file.
This must not happen" (`:328`, `LOGICAL_ERROR`: an exception in release, an abort in debug and sanitizer builds). Same class in
`src/IO/AzureBlobStorage/copyAzureBlobStorageFile.cpp:114`. Seen once in clickhouse-regression run 33560831028 on
`ATTACH PARTITION FROM`. Generic code: identical on both branches, and upstream/master (`f614faa054d`) still falls back
unconditionally. Decide the empty-object behavior (plain `PutObject` of an empty body, or an error naming the real cause) and
fix it upstream, not under a CAS commit. No matching upstream issue was found.

Provenance: BACKLOG/issue-2310.md#open-items [s3-empty-file-multipart-retry]; verified 2026-09-26 against 6eb16e1cc56, 8d62c314ec1 and upstream/master f614faa054d.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An upstream issue or pull request exists for the S3 and Azure paths
- [ ] #2 A failed single-part upload of a 0-byte object ends in the chosen behavior, never `LOGICAL_ERROR`, covered by a test
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
First recorded: 2026-09-03 (840b86dd228, by 's3-empty-file-multipart-retry')
<!-- SECTION:NOTES:END -->
