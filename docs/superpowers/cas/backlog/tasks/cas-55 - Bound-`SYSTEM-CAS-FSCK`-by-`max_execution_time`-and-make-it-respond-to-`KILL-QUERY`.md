---
id: CAS-55
title: >-
  Bound `SYSTEM CAS FSCK` by `max_execution_time` and make it respond to `KILL
  QUERY`
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:42'
labels:
  - 'area:fsck'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies:
  - CAS-9
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp
  - src/Interpreters/InterpreterSystemQuery.cpp
priority: medium
type: enhancement
ordinal: 65000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`runFsckNow` calls `Cas::runFsck(*store(), detail)` with no progress, deadline, partial flag or namespace prefix, and holds `lifecycle_mutex`
for the whole scan (`CA/ContentAddressedMetadataStorage.cpp:1170-1183`). The CLI passes all four (`programs/disks/CommandFsck.cpp:67`;
signature `CA/Tools/CasFsck.h:269-271`). The scan polls no query cancellation, so the statement
(`src/Interpreters/InterpreterSystemQuery.cpp:2610-2630`) ignores `KILL QUERY` and `max_execution_time`,
and FORGET, GC STOP and GC START block behind it. Serializing them against FSCK is deliberate; being unable to bound the scan is not.
FSCK does not finish on a ~30 GiB pool (`gc.md#fsck-scale-timeout`), so this is reachable in practice.
Once the SQL scan can end partial, `FsckReport::clean` ignoring `partial` (u07 `fsck-report-coverage-honesty`, case 0) must be fixed first,
or a timed-out SQL FSCK reports a clean pool.

Provenance: BACKLOG/operability-and-introspection.md#lifecycle-verbs-wait-out-uncancellable-scans, SQL FSCK half (2031-triage CAS-049); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SYSTEM CAS FSCK` with `max_execution_time` set returns a partial result, visibly marked partial, when the deadline passes
- [ ] #2 `KILL QUERY` stops a running `SYSTEM CAS FSCK` within one list page or ref walk step
- [ ] #3 A partial SQL result is never reported as clean
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
First recorded: 2026-08-21 (b2a38f0b611, by 'lifecycle-verbs-wait-out-uncancellable-scans')
<!-- SECTION:NOTES:END -->
