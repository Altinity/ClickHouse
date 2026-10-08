---
id: CAS-76
title: Skip stack-trace capture when a conditional S3 write gets an expected 412
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:44'
labels:
  - 'area:backend'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'needs:measurement'
dependencies: []
references:
  - src/IO/WriteBufferFromS3.cpp
priority: low
type: upstream
ordinal: 101000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`WriteBufferFromS3` logs an expected `PreconditionFailed` at Debug but still throws a full `S3Exception` (`src/IO/WriteBufferFromS3.cpp:819-831`); every `DB::Exception` captures and later symbolizes a `StackTrace`.
On the 2026-09-04 CA-s3 stateless lane, 21% of the server's ~280 CPU samples were stack unwinding and symbolization for expected 412/404/timeout exceptions.
Low priority: audit F18/F19 show the production-like stand latency-bound, not CPU-bound. Measure the share on a CPU-busier workload before shaping an upstream patch.

Provenance: BACKLOG/performance.md#stateless-lane-wall-time-is-drop-table (target 3). Verified 2026-09-26 against 59494ebf366.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A measurement shows the stack-capture share of CPU for expected 412s on a named workload
- [ ] #2 If worth it: expected conditional-write failures carry no captured stack trace, and error logs for real failures still do
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `DB::Exception` is 2026-06-05 (2c762c70d33); the finding itself has no record before this migration, so the creation date stays 2026-09-26.

Merged from u19c-soak (Skip stack-trace capture when a conditional S3 write gets an expected 412): Second cost of the same throw: every expected 412 increments `ErrorCodes::S3_ERROR` in `system.errors` / `system.error_log`
(the exception is constructed in `WriteBufferFromS3`, `src/IO/WriteBufferFromS3.cpp:815-831`, before CAS reclassifies it as
`PutOutcome::PreconditionFailed` in `CA/Backend/CasObjectStorageBackend.cpp:242-263`). On the 2026-07-19 5 h soak the
`S3_ERROR` last message was a blob-key `PreconditionFailed`, so the counter reads as real S3 errors on a CAS disk.
Returning the 412 as an outcome without constructing an exception fixes both the stack capture and the counter.
Provenance: utils/ca-soak/scenarios/BACKLOG.md#OPTIMIZATION OPPORTUNITY (conditional-PUT collisions inflate S3_ERROR).
<!-- SECTION:NOTES:END -->
