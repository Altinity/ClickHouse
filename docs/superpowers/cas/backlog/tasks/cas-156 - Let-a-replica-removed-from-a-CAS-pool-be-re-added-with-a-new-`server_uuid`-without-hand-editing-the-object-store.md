---
id: CAS-156
title: >-
  Let a replica removed from a CAS pool be re-added with a new `server_uuid`
  without hand-editing the object store
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:mounts'
  - 'complexity:large'
  - 'risk:high'
  - 'touches:protocol'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:canary'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
  - programs/server/Server.cpp
documentation:
  - docs/en/antalya/cas/architecture/mounts-and-leases.md
priority: high
type: feature
ordinal: 199000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Field report (2026-09-04): on a clickhouse-operator cluster, replica `cas-demo-0-1` was removed and re-added the next day with
a regenerated local `uuid` file (`ServerUUID::load`). Re-adding fails and nothing supported fixes it:
- `claimOwnerOrThrow` refuses a `server_uuid` mismatch (`CA/Pool/CasServerRoot.cpp:536-551`).
- A matching uuid is refused once retired (`throwIfOwnerRetired`, `:441`), because decommission tombstones the owner in place
  without changing `server_uuid` (`CA/Tools/CasDecommission.cpp:468`).
- Deleting the owner by hand over a non-empty subtree hits "identity lost over existing data" with no advice (`:556-561`).
So `SYSTEM DROP REPLICA` plus `SYSTEM CAS DROP POOL MEMBER` and a re-add does not work; only an accidental hand-edit leaves a
claimable slot. Owner's ask: "the operator already runs `SYSTEM DROP REPLICA`, there must be something similar for CAS".
Roadmap §4 "Operator recovery".

Provenance: BACKLOG/mounts-and-lifecycle.md#operator-replica-readd-uuid-trap; field report from a real k8s deployment, not the otel.demo stand. Verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 After the supported removal verb, a server with the same `server_root_id` and a new `server_uuid` mounts the pool
- [ ] #2 An integration test drops a member, recreates it with a fresh uuid file, and inserts and reads through it
- [ ] #3 No path lets a live server be claimed over by a new uuid
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
- [ ] #5 The mount TLA+ model covers the new claim path
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
First recorded: 2026-09-04 (8d6022b3070, by 'operator-replica-readd-uuid-trap')
<!-- SECTION:NOTES:END -->
