---
id: CAS-225
title: >-
  Make `scheduleRemountForTest` report the remount it scheduled even when the
  worker finishes it first
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:testing'
  - 'area:mounts'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
  - src/Disks/tests/gtest_cas_pool.cpp
  - 'https://github.com/Altinity/ClickHouse/pull/2300'
priority: medium
type: bug
ordinal: 283000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`CasMountRuntime::scheduleRemountForTest` (`CA/Pool/CasMountRuntime.cpp:1080-1085`) calls `scheduleRemount`, then re-takes `driver_mutex` to compute
`remount_requested_generation > remount_handled_generation`. The worker can finish the whole remount in between, so the seam returns `false`
for a remount that succeeded. Seen as a TSan unit-test flake (PR #2300 CI triage); seven asserts in `T/gtest_cas_pool.cpp` depend on it
(`:1739`, `:2825`, `:4090`, `:4145`, `:4222`, `:4268`, `:4609`). Test-only seam; production never uses this pattern.

Provenance: BACKLOG/testing-and-ci.md#remount-test-seam-stale-generation-race (from the deleted random/pr2300-ci-triage-20260902.md item 5); verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The return value is computed under the same lock that bumps the requested generation, or the seam returns the requested generation and tests wait for `handled >= requested`
- [ ] #2 `CASPoolRemount.*` passes 50 TSan repetitions
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
First recorded: 2026-09-26 (0b1e807ecba, by 'remount-test-seam-stale-generation-race')
<!-- SECTION:NOTES:END -->
