---
id: CAS-160
title: >-
  Reject `next_writer_epoch = 0` when decoding the server epoch and drop the two
  unread `MountFence` identity fields
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'area:formats'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h
priority: low
type: chore
ordinal: 206000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Neither is a live defect; both fail loud or are inert.
- `decodeServerEpoch` (`CA/Formats/CasServerRootFormats.cpp:111-136`) accepts `next_writer_epoch = 0` on the present path;
  the absent path is clamped in `allocateWriterEpoch`. No writer produces zero; the first ref or manifest encode would throw
  `CORRUPTED_DATA`. Reject at decode, matching `CasDecommission`'s precondition. A decode-time check on a value no writer
  emits is not a format change (decision-4).
- `MountFence::server_uuid`/`writer_epoch` (`CA/Pool/CasMountRuntime.h:127-135`) are assigned in `armMountFence`
  (`CasMountRuntime.cpp:172-173`) and read nowhere; durable identity is enforced by `liveWriterEpoch`. Drop them or mark them
  diagnostic.

Provenance: BACKLOG/mounts-and-lifecycle.md#server-epoch-zero-and-dead-fence-identity; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Decoding a server-epoch body with zero throws `CORRUPTED_DATA`, covered by a unit test
- [ ] #2 `MountFence` carries no unread fields
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
First recorded: 2026-08-21 (5e6e8a16e82, by 'server-epoch-zero-and-dead-fence-identity')
<!-- SECTION:NOTES:END -->
