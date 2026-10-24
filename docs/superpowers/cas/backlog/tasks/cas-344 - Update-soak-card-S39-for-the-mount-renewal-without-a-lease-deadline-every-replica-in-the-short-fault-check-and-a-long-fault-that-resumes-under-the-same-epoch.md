---
id: CAS-344
title: >-
  Update soak card S39 for the mount renewal without a lease deadline: every
  replica in the short-fault check, and a long fault that resumes under the same
  epoch
status: To Do
assignee: []
created_date: '2026-10-02 09:23'
labels:
  - 'area:mounts'
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:spec'
milestone: m-8
dependencies:
  - CAS-298
priority: medium
type: task
ordinal: 437000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py` encodes the behaviour before the mount-renewal change, and it reads only the first node.

What the card asserts today and what the code does after CAS-298 (branch `fix/antalya-26.6/cas-mount-renewal-no-deadline`):
- short fault (a renewal retried inside the lease): unchanged, but the profile events, the lifecycle and the renewal outcomes are read from `cl.nodes()[0]` only. Spec test 20 of `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md` asks for every replica.
- long fault (a `PUT` outage longer than the lease): the card expects `CASMountLeaseLost == 1`, a `not_live` period and a remount. After the change the background renewal keeps retrying: the mount shows `lifecycle_reason = 'lease_expired'`, `CASMountLeaseLost` stays 0, no remount happens, the `writer_epoch` is unchanged, `CASMountLeaseExpired` advances by one when a renewal restores the lease, and the `watermark_renew` event carries `expired_ms`. A fence and a remount still follow a GC fence by another member or a definitive answer of the store; if the card wants that path it has to cause it (an outage that only this node sees, long enough for the GC leader on the other node to fence).

The card cannot be changed before the binary the soak stand runs has the new behaviour: the stand mounts `build/programs/clickhouse` of the worktree that holds `utils/ca-soak`, and that lineage does not have CAS-298 yet. Do both together.

Integration coverage that exists meanwhile: `tests/integration/test_cas_mount_renewal_retry` (`test_short_put_outage_does_not_fence`, `test_long_put_outage_resumes_under_the_same_epoch`) on one node with GC disabled.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The short-fault check reads the events, the lifecycle and the renewal outcomes on every replica
- [ ] #2 The long-fault phase asserts the same writer_epoch, lifecycle_reason lease_expired during the outage, CASMountLeaseLost unchanged, CASMountLeaseExpired +1 and expired_ms in the restoring event
- [ ] #3 A separate phase covers a fence by the GC leader of another member during a one-node outage, with the remount under a new epoch
- [ ] #4 The card passes on a stand whose binary contains CAS-298
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
