---
id: CAS-292
title: >-
  Keep the mount-lease renewal alive when the server is over its memory limit
  (#2403)
status: In Progress
assignee: []
created_date: '2026-09-25'
updated_date: '2026-10-02 02:10'
labels:
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:issue'
  - 'origin:canary'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2403'
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
documentation:
  - docs/en/antalya/cas/architecture/mounts-and-leases.md
priority: high
type: bug
ordinal: 366000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The renewal runs on a plain `ThreadFromGlobalPool` (`CA/Pool/CasMountRuntime.cpp:578`, `:606`, loop at `:643`) with no thread group,
so its allocations are charged to the global tracker and can throw `MEMORY_LIMIT_EXCEEDED` above `max_server_memory_usage`.
One terminal renewal trips the mount; an exception outside the inner `try` reaches the catch-all (`CasMountRuntime.cpp:717-742`),
which trips the fence and ends the renewal thread for good.
Each attempt allocates ~1 MiB for the PUT buffer and ~1 MiB for a settling read. `conditionalWriteSettings`
(`CA/Backend/CasObjectStorageBackend.cpp:805`) does not set `s3_allow_parallel_part_upload = false`, so the PUT runs on a remote-FS pool thread.
Unfixed on both branches: no `LockMemoryExceptionInThread` anywhere under `CA/`.
Fix per the issue: hold `LockMemoryExceptionInThread(VariableContext::Global)` per renewal iteration (precedent `KeeperServer.cpp:397`,
`TransactionLog.cpp:491`), keep the control-plane PUT on the renewal thread, size control-plane buffers to the payload.
Rejected: `MemoryTrackerBlockerInThread` (hides accounting), classifying the error as deterministic, a longer budget.
Item 4 of the issue (carry the transport error into `GaveUp`) is CAS-50.

Provenance: umbrella-roadmap.md section 4 bullet 'Mount lease under memory pressure' (#2403); filed 2026-09-26 (user decision). Related: CAS-50, CAS-152. Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 With the global tracker held above its limit, renewals keep committing and the mount is not lost; without the change the same test reports a terminal renewal
- [ ] #2 A fault-injected `MEMORY_LIMIT_EXCEEDED` on the renewal path neither ends the renewal thread nor trips the fence
- [ ] #3 The control-plane PUT runs on the `CAS_LEASE_RENEWER` thread, and the renewal's memory still shows in the server's accounting
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
First recorded: 2026-09-25 (74f60f0a3a5, by 'https://github.com/Altinity/ClickHouse/issues/2403')

Merged from the GitHub reconciliation (u17, would have been a duplicate task for #2403): The renewal thread has no thread group, so its allocations go to the global tracker and can throw `MEMORY_LIMIT_EXCEEDED` whenever the server is above `max_server_memory_usage`. One terminal renewal loses the mount; an exception outside the inner `try` reaches the loop's catch-all (`Pool/CasMountRuntime.cpp:717`, loop at `:643`) and ends the renewal thread for good. A transient memory condition therefore takes down something that must never stop. Unfixed on both branches (no `LockMemoryExceptionInThread` in the file). Each attempt allocates ~1 MiB for the PUT buffer plus ~1 MiB for a settling read; the PUT runs on the remote-FS write pool because `conditionalWriteSettings` (`Backend/CasObjectStorageBackend.cpp:805`) keeps `s3_allow_parallel_part_upload = true`, so a guard on the renewal thread would not cover it. Fix (issue proposal): hold `LockMemoryExceptionInThread(VariableContext::Global)` across a renewal iteration (precedent: `KeeperServer.cpp:397`, `TransactionLog.cpp:491`); disable parallel part upload for control-plane writes with a comment saying why; size control-plane buffers to the payload. The "name the transport error in `GaveUp`" part is CAS-50. Rejected: `MemoryTrackerBlockerInThread` (hides the memory), classifying the limit as deterministic (fails faster), a longer budget. AC to keep: With the global tracker held above its limit, the renewal keeps committing and the mount stays Live; without the change the same test reports a terminal renewal; A fault-injected memory-limit exception on the renewal path neither ends the renewal thread nor trips the fence; The control-plane PUT is issued on the renewal thread, not a remote-FS write-pool thread; The renewal's allocations still show up in server memory accounting

Covered by the design in docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md (section 'Renewal under memory pressure', rule 12). Under that design a memory-limit exception inside a request is a transient failure and is retried; item 4 of the issue (failure text in GaveUp) becomes the last-failure text in lifecycle_detail and in the WARNING that reports a restored lease.

2026-10-02: implemented on local branch `fix/antalya-26.6/cas-mount-renewal-no-deadline` (worktree `cas-95-1`, base `41824f955e7`, head `7443ccd3a86`, not pushed, no PR yet). Commits `0ec8214d15b` (`LockMemoryExceptionInThread` across the renewal loop) and `817426de55c` (conditional `PUT` on the calling thread, buffer of the body size). Tests `CASMountRuntime.MemoryLimitDoesNotEndTheLeaseThread`, `CASBackend.ConditionalPutRunsOnTheCallingThread`, `CASBackend.ConditionalPutBufferStartsAtTheBodySize`. An unwrapped `std::bad_alloc` stays terminal. Gates on the head: `CAS*` unit gate 2604 passed; integration module `test_cas_mount_renewal_retry` 6 passed. No ASan run yet. Two whole-branch reviews found no defect in production code. Spec: `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md`; plan: `docs/superpowers/plans/2026-10-02-cas-mount-renewal-without-lease-deadline-part1.md`.
<!-- SECTION:NOTES:END -->
