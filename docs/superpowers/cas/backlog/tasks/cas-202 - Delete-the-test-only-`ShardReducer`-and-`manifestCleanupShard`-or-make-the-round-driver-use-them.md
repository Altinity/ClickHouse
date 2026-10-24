---
id: CAS-202
title: >-
  Delete the test-only `ShardReducer` and `manifestCleanupShard` or make the
  round driver use them
status: To Do
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:gc'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:review'
milestone: m-4
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
priority: low
type: chore
ordinal: 259000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`ShardReducer` and `manifestCleanupShard` (`CA/Gc/CasGcShardPlan.h:58`, `:62`) have no production caller on either branch.
The live sharded fold buckets by `blobShard` and calls `foldDeltasIntoGeneration` directly (`CA/Gc/CasGc.cpp:3328`, `:3372`, `:4319`);
`CasGc.cpp:3343` names `ShardReducer` only in a comment. Tests of the dead API prove nothing about the running fold.
`manifestCleanupShard` is the only routing function for the part-manifest cleanup axis, so deleting it must be a stated decision.

Provenance: BACKLOG/testing-and-ci.md#shard-reduce-api-production-dead; verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Either the API is gone and its tests target `foldDeltasIntoGeneration` and the live bucketing, or the round driver constructs one reducer per shard
- [ ] #2 The decision about the manifest-cleanup routing function is recorded in the commit message
- [ ] #3 `CAS*` gate green
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
First recorded: 2026-08-21 (2583e3427aa, by 'shard-reduce-api-production-dead')
<!-- SECTION:NOTES:END -->
