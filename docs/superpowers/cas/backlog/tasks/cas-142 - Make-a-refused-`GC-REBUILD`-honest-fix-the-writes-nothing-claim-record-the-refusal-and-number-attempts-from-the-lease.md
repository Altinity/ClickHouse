---
id: CAS-142
title: >-
  Make a refused `GC REBUILD` honest: fix the 'writes nothing' claim, record the
  refusal, and number attempts from the lease
status: To Do
assignee: []
created_date: '2026-07-13'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:gc'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:2031-triage'
milestone: m-7
dependencies: []
references:
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h
priority: low
type: bug
ordinal: 184000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`runGcRebuildNow`'s contract says "a refused rebuild writes nothing"
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:206-208`). The last refusal,
a lost `gc/state` CAS (`Gc/CasGc.cpp:4576-4582`), comes after the flushed runs and the fold seal are already written
(`:4565`), so it leaves a complete durable residue. The residue is never adopted and later pruning removes it.
Attempt numbering comes from the per-shard flush count (`:4561-4563`), not from `lease.seq`.
A performed rebuild emits a `GcRebuild` `cas_log` event (`:4588`) but no `cas_gc_log` row, and a refusal records nothing.

Provenance: BACKLOG/gc.md#rebuild-refusal-leaves-run-and-seal-residue (2031-triage CAS-094) and [gc-rebuild follow-ups] part (a), no gc-round-log row for `rebuildBaseline`. Verified 2026-09-26 against d4be7f7045a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The header comment matches the code: which refusals write nothing and which leave pruned residue
- [ ] #2 Every rebuild, performed or refused, leaves one audit record with its refusal reason, and a gtest checks the lost-CAS refusal writes it
- [ ] #3 Rebuild attempt numbers cannot collide across two refused rebuilds of one generation, proven by a gtest
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
First recorded: 2026-07-13 (45a6c8ee2b6, by 'gc-rebuild follow-ups')
<!-- SECTION:NOTES:END -->
