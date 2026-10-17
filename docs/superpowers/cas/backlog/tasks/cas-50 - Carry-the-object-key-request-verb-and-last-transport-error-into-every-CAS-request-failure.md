---
id: CAS-50
title: >-
  Carry the object key, request verb and last transport error into every CAS
  request failure
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:observability'
  - 'area:backend'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasWriteResult.h
priority: medium
type: enhancement
ordinal: 60000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
When a CAS request fails, the operator sees no cause. Two sites lose it:
- The retry loops classify each transport failure and log nothing: `CasOperation::observe` and `observePresence`
  (`CA/Backend/CasRequests.cpp:753-793`) and the write-attempt catch (`:945-1005`). `GaveUp` (`CA/Backend/CasWriteResult.h:60-70`)
  carries no error text, so the give-up message (`:499`, via `throwCasWriteRetryLater`) names only the deadline and the attempt count.
  A lane that wedges after `max_attempts` never names the socket error, timeout or S3 code that caused it.
- Under the single-attempt control plane (every writable Native mount, `CA/ContentAddressedMetadataStorage.cpp:819-823`) the read loop
  rethrows the raw transport exception (`CA/Backend/CasRequests.h:490-491`). A transient failure on the catalog, `_ckpt` or `gc/state`
  reaches the query as a bare `S3_ERROR` with no key and no operation.
Deterministic local failures are already rethrown (`isDeterministicLocalFailure`), so this is diagnosis only; every exit stays fail-closed.
Shape: record the last attempt's exception text in the loop state, put it into the give-up message, annotate an escaping exception
with verb and key, and add a rate-limited log at classification (`logCasWriteRetryLater`, `:85-94`, is the existing limiter).

Provenance: BACKLOG/operability-and-introspection.md#putifabsent-swallowed-attempt-cause (2031-triage CAS-068; `putIfAbsentControlled` deleted by c3f7b20f8ab) and #control-object-generic-s3-error; verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A CAS write or read that gives up after retries produces an error message naming the key, the verb and the last attempt's transport error
- [ ] #2 A single-attempt control-object failure that reaches a query names the object key and the operation
- [ ] #3 Per-attempt transport failures are logged through a rate limiter, and a test shows the rate limit holds under a retry storm
- [ ] #4 No error code or retry decision changes: existing request-engine tests pass unchanged
- [ ] #5 A `_ckpt` publish that gives up for a transport or deadline reason is not reported as `persistent CAS contention`: the message states the `GaveUp` reason, source and attempt count (`Pool/CasRefCkpt.cpp:270-280`, issue #2343)
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
First recorded: 2026-08-21 (2a29781641d, by 'putifabsent-swallowed-attempt-cause')
<!-- SECTION:NOTES:END -->
