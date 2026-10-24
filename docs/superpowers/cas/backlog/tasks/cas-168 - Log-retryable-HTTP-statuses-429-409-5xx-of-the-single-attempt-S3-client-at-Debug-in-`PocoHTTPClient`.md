---
id: CAS-168
title: >-
  Log retryable HTTP statuses (429, 409, 5xx) of the single-attempt S3 client at
  Debug in `PocoHTTPClient`
status: To Do
assignee: []
created_date: '2026-09-05'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:observability'
  - 'area:backend'
  - 'area:upstream'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-7
dependencies: []
references:
  - src/IO/S3/PocoHTTPClient.cpp
  - src/IO/S3/Client.h
  - docs/superpowers/cas/2026-09-04-single-attempt-client-log-level-proposal.md
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f28
  - docs/superpowers/cas/umbrella-roadmap.md
priority: high
type: upstream
ordinal: 214000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every 429 / 409 that the CAS engine resolves and reissues still prints `<Error> AWSClient: Response status: 429, Too Many Requests` from `PocoHTTPClient::makeRequestInternalImpl`'s status branch (`src/IO/S3/PocoHTTPClient.cpp:740`, the `HTTP_NOT_FOUND != status_code || !is404Expected()` arm), same on both branches.
GCS smoke 2026-09-05: 174 / 161 such Error lines per node in 8 minutes. otel.demo: ~110 `409 Conflict` Error lines per day (audit F28), part of the ~3k lines/day of conditional-write noise in roadmap §3.
The two thrown-exception sites were demoted already (`08c2a2ec25e`, `7ec4d8b0c39`); this is the third site of the 2026-09-04 log-level proposal.
Fix, about ten lines: a `bool single_attempt` in `PocoHTTPClient`, set in the `PocoHTTPClientConfiguration` constructor (`:222`, next to `s3_use_adaptive_timeouts`) by `dynamic_cast<const SingleAttemptRetryStrategy *>(retryStrategy.get())` (`src/IO/S3/Client.h:374`); the bare-config constructor (`:240`) leaves it false.
In the log branch: single-attempt and status 429 / 409 / 5xx go to Debug; other 4xx stay Error. Retries and outcomes are untouched.

Provenance: BACKLOG/gcs.md#single-attempt-client-status-error-log-site (incl. its 409 log bullet). Roadmap §3 'Log noise on conditional writes' also names the `WriteBufferFromS3: Nothing to abort` / `was canceled` pair, which no backlog item carries; not added here. Verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A `PocoHTTPClient` gtest on `TestPocoHTTPServer` shows a 429 at Debug for a single-attempt client and at Error for an ordinary one; the same for 409
- [ ] #2 A GCS smoke shows zero `Response status: 429` at Error and the same 429 count at Debug
- [ ] #3 Non-retryable 4xx on a single-attempt client still log at Error
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
First recorded: 2026-09-05 (6b752525a36, by 'single-attempt-client-status-error-log-site')
<!-- SECTION:NOTES:END -->
