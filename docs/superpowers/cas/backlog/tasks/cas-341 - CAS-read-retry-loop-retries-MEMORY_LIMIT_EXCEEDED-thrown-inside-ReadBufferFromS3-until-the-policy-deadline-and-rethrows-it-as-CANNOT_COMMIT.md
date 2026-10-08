---
id: CAS-341
title: >-
  CAS read retry: give up with the original exception, and do not reissue
  verdicts on the query (query-level memory limit, overcommit kill, cancel,
  max_execution_time)
status: To Do
assignee: []
created_date: '2026-10-01 17:57'
updated_date: '2026-10-01 19:28'
labels:
  - gc
  - bug
  - ci
dependencies: []
priority: high
ordinal: 434000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Observed 2026-10-01 on PR #2463, job "Stateless tests (arm_binary, cas s3 storage, parallel)", test 04023_rollup_totals_memory_limit under randomized MergeTree settings (prewarm_*, use_primary_key_cache): the manifest GET allocates ~1.1 MB under max_memory_usage = 1 MB and throws 241 "Query memory limit exceeded" inside ReadBufferFromS3 (which itself does not retry 241: "Attempt: 1/1"). CasOperation::readLoop (Backend/CasRequests.h) classifies it as transient: refreshAndClassifyReadFault passes only LOGICAL_ERROR / NOT_IMPLEMENTED / BAD_ARGUMENTS / CORRUPTED_DATA through, every DB::Exception is a Poco::Exception, so 241 is reissued 38 times to the policy deadline, then giveUpReadDeadline throws 210 "CAS write could not be committed (read of ...: gave up at the policy deadline after 38 attempt(s)); retrying later (NETWORK_ERROR)". Evidence: tmp/ci-2463/rca.md section 7, tmp/ci-2463/cas_s3.log query b42da83c. Not caused by #2463.

Three parts, decided with the owner 2026-10-01:
(a) Unconditional: when a read or write loop gives up at its deadline, rethrow the LAST underlying exception with its own code and text instead of the 210/NETWORK_ERROR wrapper. "write could not be committed", "network", "retrying later" were all false here; in production they send the operator to the network.
(b) Do not reissue a verdict on the query itself: 241 at the query level ("Query memory limit exceeded", max_memory_usage) and the overcommit decision ("OvercommitTracker decision: Query was selected to stop"), QUERY_WAS_CANCELLED, TIMEOUT_EXCEEDED raised by max_execution_time (the query budget, not a socket timeout). Nothing a reissue does can change these. Between attempts check the query status so KILL QUERY and max_execution_time interrupt the loop (overlaps CAS-181 / CAS-55).
(c) Keep retrying genuine environment faults: socket/transport timeouts (Poco::TimeoutException, SOCKET_TIMEOUT) as today; 241 at the total or user level ("Memory limit (total) exceeded", "(for user)") may clear when neighbours finish, so a short bounded reissue (a few attempts, seconds) is acceptable there — not the full policy deadline.
The memory-tracker level is only in the exception text today; if no structured field exists, add one to MemoryTracker rather than matching strings, and keep the fork patch minimal.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The CAS read/write retry loops rethrow MEMORY_LIMIT_EXCEEDED, QUERY_WAS_CANCELLED and the other caller-side errors immediately with the original code
- [ ] #2 A gtest injects MEMORY_LIMIT_EXCEEDED from the backend read and asserts one attempt and code 241
- [ ] #3 04023_rollup_totals_memory_limit passes on the cas s3 storage lane under the randomized settings that failed
- [ ] #4 A read/write loop that gives up rethrows the last underlying exception (code and text preserved); the 210 wrapper is used only when no underlying exception exists
- [ ] #5 gtest: backend read throws query-level 241 -> exactly one attempt, code 241; cancelled query / max_execution_time -> loop exits at the next attempt boundary with the query error
- [ ] #6 gtest: total-level 241 -> bounded reissue (N attempts) then the original 241
- [ ] #7 Socket timeouts keep today behaviour (existing tests unchanged)
- [ ] #8 04023_rollup_totals_memory_limit passes on the cas s3 storage lane under the randomized settings that failed
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
