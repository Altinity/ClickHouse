---
id: CAS-133
title: >-
  Record the GC leader's host identity in `gc/state` and name it on a follower's
  `GC RUN` row and in `cas_mounts.is_leader`
status: To Do
assignee:
  - '@k-morozov'
created_date: '2026-08-21'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:observability'
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:on-s3-format'
  - 'confidence:solid'
  - 'needs:spec'
  - 'origin:issue'
milestone: m-7
dependencies:
  - CAS-132
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h
  - src/Storages/System/StorageSystemContentAddressedMounts.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2211'
documentation:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/README.md
  - docs/en/antalya/cas/operations/debugging.md
priority: medium
type: feature
ordinal: 172000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A follower cannot name the GC leader: `GcLease` is `{owner, seq}` with `owner` a random per-process UInt128
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.h:15-19`), and
`system.cas_mounts.is_leader` is filled only for the local mount, NULL on other servers' rows
(`src/Storages/System/StorageSystemContentAddressedMounts.cpp:52,203-210`).
Decided 2026-08-21 (user): add advisory `hostname` (plus `server_uuid`, `pid`) next to the protocol fields, as
`MountLease` already does (`Formats/CasServerRootFormats.h:47-52`), written at acquire and steal. `owner` stays the
random per-instance id: a restarted server is a new GC actor and must not resume the old lease.
Rejected (user): documenting a `clusterAllReplicas(system.cas_mounts) WHERE is_leader=1` discovery recipe as the contract.
`gc/state` is format v1 since 26.6.4 (decision-4): the new fields need a format version and a reader that accepts
both; the source's "pre-release, no compat scaffolding" is superseded.

Provenance: BACKLOG/gc.md#issue-2211-gc-run-follower-noop (fix bullets 2-4). Verified 2026-09-26 against d4be7f7045a and 0dbbd797792: `GcLease` has no identity field on either branch.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A follower's `GC RUN` row carries `leader_host` naming the lease holder
- [ ] #2 `system.cas_mounts.is_leader` is non-NULL for every row whose server matches the `gc/state` identity
- [ ] #3 A gtest decodes a v1 `gc/state` without the new fields and a new one with them; the lease decision ignores the advisory fields
- [ ] #4 The format change is recorded in the formats README with its upgrade order
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
First recorded: 2026-08-21 (50fa0d52a9f, by 'issue-2211-gc-run-follower-noop')

Issue #2211 is assigned to k-morozov (2026-09-26).
<!-- SECTION:NOTES:END -->
