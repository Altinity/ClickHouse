---
id: CAS-143
title: >-
  Make `cas-gc-dryrun` distinguish a healthy empty preview from a missing
  adopted seal, per shard
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:gc'
  - 'area:tooling'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - programs/disks/CommandCaGcDryRun.cpp
documentation:
  - docs/en/antalya/cas/operations/debugging.md
priority: low
type: bug
ordinal: 185000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Gc::previewDeletes` returns an empty list both for an absent `gc/state`
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:4613-4615`) and for an absent adopted seal
(`:4625-4627`, "Absent seal => no candidates"). A regular round treats a missing adopted seal at generation > 0 as `CORRUPTED_DATA`.
`clickhouse-disks cas-gc-dryrun` prints `preview_deletes=0` for all three cases (`programs/disks/CommandCaGcDryRun.cpp`).
The dry run is read-only and never authorizes a delete, but it reports a disaster as "nothing to do".

Provenance: BACKLOG/gc.md#gc-dryrun-silent-on-damaged-state (2031-triage CAS-095). Same function as CAS-12; do both in one change if convenient. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The preview reports one of: healthy, no baseline yet (generation 0 or absent state), adopted seal missing; plus per-shard candidate counts
- [ ] #2 A gtest per case checks the reported status
- [ ] #3 The CLI description states what the command previews and what the status means
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
First recorded: 2026-08-21 (e811b3d68f4, by 'CAS-095')
<!-- SECTION:NOTES:END -->
