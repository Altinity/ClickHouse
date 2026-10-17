---
id: CAS-298
title: >-
  Stop the mount-lease renewal from fencing on `external_lease_deadline` while
  most of the confirmed budget remains (#2431)
status: In Progress
assignee: []
created_date: '2026-09-26 12:37'
updated_date: '2026-10-02 16:26'
labels:
  - 'area:mounts'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'needs:repro'
  - 'origin:issue'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/issues/2431'
  - 'https://github.com/Altinity/ClickHouse/issues/2423'
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
priority: high
type: bug
ordinal: 377000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An 8-second burst of S3 503s on PUT/POST (renew period 10 s, TTL 30 s) fences the mount lease in 2-2.6 s with
`classification=external_lease_deadline` while `watermark_renew.remaining_confirmed_budget_ms` is ~17,000. The next INSERT fails with
Code 210 `mount lease not held`. The last observation at give-up is `present` with an etag, so the mount object was intact.
Reproduced twice on wiped ca-soak pools with `26.6.4.20001.altinityantalya` release: run 1 both replicas fenced (2255 ms / 5 attempts,
1803 ms), run 2 one fenced (2634 ms / 6 attempts) while the other logged `recovered after 8 physical attempts in 3576 ms`. The 40 s fault
also fences after ~2.5 s with ~17 s left instead of at the TTL.
The label comes from `GaveUp{Why::Deadline, Source::Lease}` → `ExternalLeaseDeadline` (`Pool/CasServerRoot.cpp:1692-1695`, both
branches), so the lease-sourced deadline handed to the request engine for the renewal ends far earlier than the confirmed budget says.
CAS-152 (S03, "stopped with 1,969 ms left") may be the same defect; this is the first dev-scale reproduction.

Provenance: GitHub issue #2431 (open, no assignee), reconciled by u17-github-cas; verified 2026-09-26 against cae9288ee65 (cas-gc-rebuild) and altinity/antalya-26.6. Related CAS-152, CAS-150. Soak rig, not a real deployment, so no origin:canary.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A test injects an 8 s 503 window on mount-object writes; the renewal commits after the window and no `fenced after` line is logged
- [ ] #2 With the window held past the TTL, the fence happens no earlier than the confirmed deadline minus the configured margin
- [ ] #3 `remaining_confirmed_budget_ms` at give-up and the lease deadline the engine was bound to are logged together, and agree
- [ ] #4 S39's short-fault check queries every replica, not only `clickhouse1`
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
2026-10-02: implemented on local branch `fix/antalya-26.6/cas-mount-renewal-no-deadline` (worktree `cas-95-1`, base `41824f955e7`, head `7443ccd3a86`, not pushed, no PR yet). The background renewal retries one tuple until a definitive answer, a stop or a remount request (`MountRenewPolicy::UntilDefinitive`, spacing 1000 ms, own request plane); the lease TTL alone bounds write authority; an expired lease shows as `lifecycle_reason = 'lease_expired'` in `system.cas_mounts`, `CASMountLeaseExpired` counts restores, `expired_ms` in `watermark_renew`. Commits `c0e112dbc14`..`98776bf591d` (tasks 3 to 5 of the plan) plus tests, docs and review fixes up to the head. With defaults a conditional write is refused once less than 16 s of the lease remain, about 4 s after a due renewal starts failing: the outage now costs refused writes with a transient error, not a fence and a remount. Open for the owner: a log line at the moment of expiry; whether to widen that 4 s window (renew period, TTL or the two-envelope reservation). Simplification (one lease thread, one renewal path) is a second plan. Gates on the head: `CAS*` unit gate 2604 passed; integration module `test_cas_mount_renewal_retry` 6 passed. No ASan run yet. Two whole-branch reviews found no defect in production code. Spec: `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md`; plan: `docs/superpowers/plans/2026-10-02-cas-mount-renewal-without-lease-deadline-part1.md`.

2026-10-02 (later): the fix branch gained one WARNING per lease expiry (head `e1c38ca7478`; all gates green, ASan on its exact tree included). The simplification of the spec's steps 4 to 8 is done on the local branch `refactor/antalya-26.6/cas-mount-one-lease-thread` (head `56897c5ccbc`, contains the fix branch; not pushed): one lease thread, one renewal path, a result-returning renewal, cleanups; production net -624 lines against the fix branch; unit gate 2613, integration 6 passed, ASan 2617 with no error. It is a separate pull request, to be opened after the fix branch merges. Plan and contract: `docs/superpowers/plans/2026-10-02-cas-mount-one-lease-thread-part2.md` and `…-part2-contract.md`. Follow-ups: CAS-343 (bound one control-plane attempt by a wall-clock deadline), CAS-344 (soak card S39), the settings notes `docs/superpowers/specs/2026-10-02-cas-mount-lease-settings-ux-notes.md` with the external study.

2026-10-02 (evening): both branches pushed to `altinity`. Draft pull requests: https://github.com/Altinity/ClickHouse/pull/2474 (the fix, into `antalya-26.6`) and https://github.com/Altinity/ClickHouse/pull/2475 (the simplification, stacked on the fix branch to keep its diff readable; retarget to `antalya-26.6` after 2474 merges).
<!-- SECTION:NOTES:END -->
