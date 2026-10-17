---
id: CAS-231
title: >-
  Classify a RustFS 412 body without the `Unable to parse ExceptionName` log
  line or a message-substring match
status: To Do
assignee: []
created_date: '2026-07-15'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:backend'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies: []
references:
  - src/IO/S3Common.h
  - src/IO/S3/Client.cpp
  - src/IO/S3/tests/gtest_aws_s3_client.cpp
priority: low
type: enhancement
ordinal: 294000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`S3::isPreconditionFailedError` (`src/IO/S3Common.h:94-99`) decides a 412 by HTTP status first, then `ExceptionName`, then a raw
substring of the message. RustFS returns a non-AWS error body, so the SDK logs `Unable to parse ExceptionName: ...` on every
expected `PreconditionFailed` and leaves the name empty. The status arm already classifies the 412 correctly; the substring
arm can match unrelated text and the log line is noise on every dedup race in the RustFS CI lanes.
The predicate is one of the Group G carve-outs, so fix its shape before it goes upstream.

Provenance: BACKLOG/docs-and-cleanup.md#minor [RUSTFS-ERROR-XML]; narrowed: the HTTP-status arm already decides; verified 2026-09-26 against 6eb16e1cc56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A RustFS-shaped 412 body is classified as `PreconditionFailed` by a shape-tolerant XML read, not by a message substring
- [ ] #2 A gtest in `src/IO/S3/tests/gtest_aws_s3_client.cpp` covers an AWS body, a RustFS body and a non-412 body whose message contains `PreconditionFailed`
- [ ] #3 A RustFS-lane 412 no longer logs `Unable to parse ExceptionName`
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
First recorded: 2026-07-15 (db28579459a, by 'RUSTFS-ERROR-XML')
<!-- SECTION:NOTES:END -->
