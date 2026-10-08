---
id: CAS-256
title: >-
  Re-run the sustained-S3-fault repro and stop merges from recomputing in a loop
  after the mount fence closes
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:replication'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - src/Storages/MergeTree/ReplicatedMergeMutateTaskBase.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.cpp
  - utils/ca-soak/docker-compose-s3faultproxy.yml
priority: medium
type: bug
ordinal: 321000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
A sustained S3 fault can correctly close the mount-lease fence. The original repro then saw one merge recomputed 239 times in a
tight loop, because CAS reported the fence refusal as `ABORTED` and `ReplicatedMergeMutateTaskBase::executeStep`
(`src/Storages/MergeTree/ReplicatedMergeMutateTaskBase.cpp:71-77`) treats `ABORTED` as "not an error" and saves nothing.
Since `58578af0c6d` (2026-07-29) the lease-fence refusal is minted as `NETWORK_ERROR` by `throwCasTransientUnavailable`
(`CA/Backend/CasRequests.cpp:105-117`, callers `CA/ContentAddressedMetadataStorage.cpp:1276`, `CA/Pool/CasMountRuntime.cpp:130`),
which is saved and paced by the queue. Other CA `ABORTED` throws remain (`CA/Pool/CasPool.cpp:741`, `:766`,
`CA/Pool/CasServerRoot.cpp:667`, `:1423`). The transient-blip half is fixed (S39: 50 pulses, no fence loss).
Also owed: fsck to fixpoint after fence-loss recovery.

Provenance: BACKLOG/replication.md [merge-progress-reset-mount-fence]; the ABORTED premise changed with 58578af0c6d. Verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The repro on `utils/ca-soak/docker-compose-s3faultproxy.yml` reports how many times one merge is recomputed during a sustained fault
- [ ] #2 If it still loops, the refusal reaching the merge task gets a retry-later class with backoff, and the repro shows it
- [ ] #3 fsck after recovery reaches a fixpoint with `dangling = 0`
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
First recorded: 2026-08-04 (b4420fe512a, by 'merge-progress-reset-mount-fence')
<!-- SECTION:NOTES:END -->
