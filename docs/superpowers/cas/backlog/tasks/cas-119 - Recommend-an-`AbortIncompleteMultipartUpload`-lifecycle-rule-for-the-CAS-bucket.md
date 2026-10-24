---
id: CAS-119
title: >-
  Recommend an `AbortIncompleteMultipartUpload` lifecycle rule for the CAS
  bucket
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:36'
labels:
  - 'area:docs'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - docs/en/antalya/cas/bucket-requirements.md
  - src/IO/WriteBufferFromS3.cpp
priority: low
type: docs
ordinal: 157000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
CAS writes through `WriteBufferFromS3`, which aborts multipart uploads on cancel and in its destructor, so only a process
kill or a failed `AbortMultipartUpload` leaves parts behind, billed until a bucket lifecycle rule expires them. Same as any
ClickHouse S3 disk, but CAS docs promise pool byte accounting, and fsck `physical_bytes` counts only listed blob bodies
(`CA/Tools/CasFsck.cpp:746`, `:760`, `:1075`). No mention of the rule in `docs/en/antalya/cas/` on either branch. Cost only.

Provenance: BACKLOG/operability-and-introspection.md#mpu-and-probe-debris-unaccounted (multipart half; 2031-triage CAS-082); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792. Per-class byte accounting is CAS-11.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `bucket-requirements.md` or the operations docs recommend an `AbortIncompleteMultipartUpload` lifecycle rule with an example
- [ ] #2 The fsck docs say `physical_bytes` counts committed objects, not in-flight multipart parts
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
First recorded: 2026-08-21 (b183da7ea36, by 'mpu-and-probe-debris-unaccounted')
<!-- SECTION:NOTES:END -->
