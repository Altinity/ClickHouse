---
id: CAS-212
title: >-
  Test that a reload flipping `http_client` from an endpoint-level `<s3>` block
  is refused and the old dialect survives
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:gcs'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - tests/integration/test_cas_gcs/test.py
  - tests/integration/test_cas_gcs/gcs_mocks/server.py
  - src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
priority: medium
type: task
ordinal: 269000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The token-dialect pin is checked in `S3ObjectStorage::applyNewSettings` against the fully merged settings
(`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:1113`, check at `:1159-1161`). The only test,
`test_a_reload_that_would_flip_the_token_dialect_is_refused` (`tests/integration/test_cas_gcs/test.py:1362`), flips the disk-level key,
which a disk-section-only guard would also refuse; its docstring says so. A dialect flip leaves persisted tokens uncomparable.
Prerequisite: the fake GCS mints numeric ETags and rejects a numeric `If-Match` as "a generation, not an ETag" (`gcs_mocks/server.py:365`),
so it needs a second, non-numeric ETag shape kept apart from the generation domain.
An absent key is not a flip: settings merge through `updateIfChanged`, so only an explicitly different value flips the dialect.

Provenance: BACKLOG/testing-and-ci.md#gcs-endpoint-level-reload-regression; verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The fake GCS can host an ETag-dialect mount with non-numeric ETags
- [ ] #2 A CAS disk with no disk-level `http_client` is refused a reload that sets a different one in an endpoint-level block
- [ ] #3 A write after the refused reload shows `If-None-Match` on the wire and no `x-goog-if-generation-match` anywhere
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
First recorded: 2026-08-21 (43b918b4ed6, by 'gcs-endpoint-level-reload-regression')
<!-- SECTION:NOTES:END -->
