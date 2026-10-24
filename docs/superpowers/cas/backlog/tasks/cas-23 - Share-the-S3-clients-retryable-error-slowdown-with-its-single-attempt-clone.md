---
id: CAS-23
title: Share the S3 client's retryable-error slowdown with its single-attempt clone
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:2031-triage'
dependencies: []
references:
  - src/IO/S3/Client.h
  - src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
  - docs/superpowers/cas/2031-triage.md#cas-119
priority: low
type: upstream
ordinal: 29000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Each `S3::Client` owns its `next_time_to_retry_after_retryable_error` (`src/IO/S3/Client.h:364`). The single-attempt clone that CAS conditional writes use (`S3ObjectStorage::getSingleAttemptClient`, `S3ObjectStorage.cpp:1212`) gets its own copy, so a store-wide 503 seen by one does not pace the other.
Not a correctness issue: outcomes stay `Unresolved`, not `Committed`, and retries are bounded.
The other residual of this source item (no jitter) is fixed: `Retry::backoff` is full jitter since `6726575e665`. Adjacent: audit F26 (GC LIST bursts correlate with throttling).

Provenance: BACKLOG/performance.md#conditional-write-retry-pacing-and-jitter residual (2) (2031-triage CAS-119). Related: ref-protocol.md [timeout-retry RFC residuals]. Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A 503 on either client delays the next request of both, verified by a test that shares the slowdown state
- [ ] #2 The change is a minimal, portable patch to `src/IO/S3` suitable for upstreaming
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
First recorded: 2026-08-21 (b9a1774ba4e, by 'conditional-write-retry-pacing-and-jitter')
<!-- SECTION:NOTES:END -->
