---
id: DRAFT-30
title: >-
  Back off on local socket-resource errors (`EADDRNOTAVAIL`, `EMFILE`, `ENFILE`)
  instead of retrying at rate
status: Draft
assignee: []
created_date: '2026-08-20'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:backend'
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:plausible'
  - 'origin:issue'
milestone: m-5
dependencies:
  - CAS-74
references:
  - src/IO/S3/PocoHTTPClient.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2243'
priority: low
type: upstream
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Issue #2243 (closed): under ephemeral-port exhaustion every layer classifies `EADDRNOTAVAIL` as a remote transient, and reads
retry it up to `s3_retry_attempts = 500`, feeding the exhaustion. CAS writes now treat "Cannot assign requested address" as a
connect failure and reissue at once (`isConnectFailureHint`, `CA/Backend/CasRequests.cpp:246-261`, `9a6bcb68aca`), which
settles ambiguity but does not slow the rate. The upstream S3 retry path has no local-resource class (no match for these errno
under `src/IO/S3`). Template: the DNS sub-classification branch in `PocoHTTPClient.cpp`.
Decide after the non-loopback keep-alive redo whether the amplifier still matters.

Provenance: BACKLOG/mounts-and-lifecycle.md#issue-2243-port-exhaustion-lease direction (1); the source's 'addressed' status is only half true. Verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Either local socket-resource errors get a hard backoff in the S3 retry strategy, or the redo shows the amplifier is gone and this closes
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
First recorded: 2026-08-20 (d61e8ac938c, by 'issue-2243-port-exhaustion-lease')
<!-- SECTION:NOTES:END -->
