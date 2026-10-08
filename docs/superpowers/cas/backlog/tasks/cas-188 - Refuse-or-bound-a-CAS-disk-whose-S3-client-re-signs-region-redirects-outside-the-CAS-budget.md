---
id: CAS-188
title: >-
  Refuse or bound a CAS disk whose S3 client re-signs region redirects outside
  the CAS budget
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'origin:review'
dependencies: []
references:
  - src/IO/S3/Client.cpp
  - src/IO/S3/PocoHTTPClient.cpp
  - R/Backend/CasRequests.cpp
priority: low
type: task
ordinal: 245000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Every CAS request runs through `CasRequests`/`CasOperation` with `Retry::standard()` (90 s budget); the old `CasRequestController` is gone.
The S3 client's redirect loop (`S3::Client::doRequest`, `src/IO/S3/Client.cpp:730-787`) re-issues a request up to `max_redirects` times outside that budget when `detect_region` is true.
`detect_region` holds for an AWS endpoint whose host names no region (`Client.cpp:334`, `PocoHTTPClient.cpp:198-200`), so a CAS disk can be `aws-global` by configuration today. No guard exists.

Provenance: BACKLOG/ref-protocol.md#ref-protocol-rev6 [timeout-retry RFC residuals]. The source's 'CAS disks are not aws-global today' is a configuration fact, not a code guarantee. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A CAS disk configured with a region-less AWS endpoint is either refused at mount with a message naming the fix, or its redirects count against the CAS request budget (test for the chosen behavior)
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'timeout-retry RFC residuals')
<!-- SECTION:NOTES:END -->
