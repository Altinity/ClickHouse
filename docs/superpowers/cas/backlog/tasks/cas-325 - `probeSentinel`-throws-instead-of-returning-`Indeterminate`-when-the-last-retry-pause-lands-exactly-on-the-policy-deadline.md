---
id: CAS-325
title: >-
  `probeSentinel` throws instead of returning `Indeterminate` when the last
  retry pause lands exactly on the policy deadline
status: To Do
assignee: []
created_date: '2026-09-30 10:30'
updated_date: '2026-09-30 10:30'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - src/Disks/tests/gtest_cas_sentinel_probe.cpp
priority: low
type: bug
ordinal: 411000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Boundary mismatch in the reissue loop of `probeSentinel` (`Backend/CasRequests.cpp`; line numbers at antalya-26.6 @e2dd0f5d343).
After an `Indeterminate` probe the loop draws a full-jitter pause (`:701-709`, uniform 0..5000 ms from `CasRetry.cpp:11-18`) and returns `Indeterminate` when `fits(pause + reservation)` is false. `fits` (`:445-452`) accepts `needed <= deadline - now` but the loop's top check (`:672`) requires `now < deadline`. With a zero reservation and a pause equal to the remaining time, the sleep ends at `now == deadline`; the next iteration throws through `giveUpReadDeadline` (`:673`): 'gave up at the policy deadline after N attempt(s)'.
Seen as a flaky gtest: `CASSentinelProbe.NativeClassifiesUnmodeledErrorAsIndeterminate` failed once in an ASan gate run on 2026-09-30; 6 failures in 10,000 repeats of the test alone (about 6e-4), matching the 1/5001 chance per step near the deadline. Production needs a backend with a zero request envelope to hit it.
The same check shape is in `removeCurrent` and the read loop (`CasRequests.h:512-513`); they throw on give-up by contract, so there only the reported attempt count is affected. Not verified.
Not touched by the GC request-cuts branch or by the base commits merged on 2026-09-30.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The reissue loop returns the last `Indeterminate` when a pause ends exactly at the deadline (strict check before the sleep, or the top check returns the probe's outcome once a probe has run)
- [ ] #2 Deterministic gtest: the last pause equals the remaining window; the outcome is `Indeterminate`
- [ ] #3 `CASSentinelProbe.NativeClassifiesUnmodeledErrorAsIndeterminate` passes 100,000 repeats
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
