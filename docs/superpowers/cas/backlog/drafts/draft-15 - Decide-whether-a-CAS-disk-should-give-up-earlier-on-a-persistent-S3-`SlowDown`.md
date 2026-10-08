---
id: DRAFT-15
title: Decide whether a CAS disk should give up earlier on a persistent S3 `SlowDown`
status: Draft
assignee: []
created_date: '2026-08-31'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:backend'
  - 'complexity:small'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'touches:settings'
  - 'confidence:speculative'
  - 'needs:decision'
  - 'origin:soak'
dependencies: []
references:
  - src/IO/S3Defines.h
  - utils/ca-soak/configs/rustfs.env
  - utils/ca-soak/configs/profiling.xml
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The S3 client retries up to `DEFAULT_RETRY_ATTEMPTS = 500` (`src/IO/S3Defines.h:44`, upstream default). Against a store refusing on a concurrency ceiling, 104 threads sat in `RetryRequestSleep` and a merge made no progress for minutes: neither success nor an error.
The rig was fixed (`RUSTFS_OBJECT_MAX_CONCURRENT_DISK_READS=256`, `s3_max_connections=192`). The product question remains: should a self-caused, persistent 503 surface as an error long before attempt 500 on a CAS disk?
Open rig question: 503s continued at ceiling 256 on an idle host (579 in three minutes, load 0.82, iowait 0%); cause unknown.

Provenance: BACKLOG/performance.md#soak-retry-budget-livelock. Rig half done in 3257a1407eb and 73d5e0bfa2b. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A decision is recorded: keep the upstream default, cap retries for CAS disks, or add a no-progress deadline
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
From gcs.md (u12 merge): the S3 client's own retry strategy, 500 attempts with a 5 s cap and zero jitter, can block a thread for about forty minutes on a persistent SlowDown, and a CasOperation deadline cannot preempt it; any give-up-earlier decision has to bound that inner retry loop, not only the CAS-level attempt.

First recorded: 2026-08-31 (271b7132f6f, by 'soak-retry-budget-livelock')
<!-- SECTION:NOTES:END -->
