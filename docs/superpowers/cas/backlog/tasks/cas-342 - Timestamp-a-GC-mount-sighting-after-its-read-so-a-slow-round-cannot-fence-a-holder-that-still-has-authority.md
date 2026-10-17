---
id: CAS-342
title: >-
  Timestamp a GC mount sighting after its read so a slow round cannot fence a
  holder that still has authority
status: In Progress
assignee: []
created_date: '2026-10-01 19:31'
updated_date: '2026-10-02 02:10'
labels:
  - 'area:gc'
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:medium'
  - 'confidence:solid'
  - 'needs:repro'
  - 'origin:review'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
  - src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.cpp
  - >-
    docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md
  - 'https://github.com/Altinity/ClickHouse/issues/2431'
priority: high
type: bug
ordinal: 435000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The GC leader credits a mount-token sighting with a clock sample taken before the read that produced it, so a slow round followed by a close second round can write `gc_fenced` over a holder that still has write authority.

`Gc::runRegularRound` and the rebuild pass in `Gc/CasGc.cpp` call `computeHeartbeatFloor(op, layout, now_ms_fn(), mono_ms_fn(), ...)`. The monotonic sample is an argument, taken before the `LIST` of `gc/server-roots/` and the per-slot `readModifyWrite`. `computeHeartbeatFloor` (`Pool/CasServerRoot.cpp`) stores it as `first_seen_mono_ms`, on the first decision and on the re-decision after a refused fence-out. A sighting made `d` after the sample is credited `d` too early. `claimMountAwaitingExpiry` samples after its read and is not affected.

Example with defaults (TTL 30 s, period 10 s, threshold 41.5 s): round samples at 0 s; holder commits a renewal at 18 s (authority until 48 s); GC reads the slot at 20 s and records `first_seen = 0`; a round at 42 s reads the same token and fences after 22 s of real observation; the holder admits writes until 48 s.

Conditions, both needed:
- The walk spends more than `threshold - TTL` (11.5 s; 10 s with a 5 percent faster observer clock) between the sample and the slot read when the holder's later renewals fail, or more than 31.5 s when the holder is healthy (its token must stay unchanged across both reads).
- A second round starts soon after. `CasGcScheduler::loop` waits a full `gc_interval_sec` (60 s) after a round ends, which is longer than the TTL, so the timer alone does not trigger it. `runOneRoundNow` (manual round), an early requested round and the rebuild pass do.

Today a holder whose renewals fail fences itself about 14 s after its last commit, which hides the first case. The design in `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md` (CAS-298) keeps authority until the deadline and lists this fix as its prerequisite (rule 11).

Not traced: what GC removes or retires for the fenced epoch while the holder still admits writes. A fence is the certificate that lets the same server reclaim the slot at once and lets GC treat the epoch as ended.

Fix: pass the clock as a function and take the `first_seen` sample inside the decision, after the read. Keep the round-start sample for the stability test; it is earlier than the real instant, so it is conservative. No format change.

Provenance: found in the review of the CAS-298 spec, 2026-10-01; the timestamp misuse was verified against the code at fe7fd325bda (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 computeHeartbeatFloor records the first sighting of a token from a clock sample taken after the read that returned it, on the first decision and on the re-decision after a refused fence-out
- [ ] #2 The stability test uses a sample taken before the confirming read
- [ ] #3 A gtest advances the injected clock by more than threshold - TTL between the call and the slot read: a second call one threshold after the first call's start does not fence, a call one threshold after the read does
- [ ] #4 Both call sites in Gc/CasGc.cpp pass the clock function
- [ ] #5 The task records what GC can remove or retire for a fenced epoch whose holder still admits writes
- [ ] #6 A gtest covers the re-decision: a sighting recorded after a refused fence-out is credited after that read, not at the round's start
- [ ] #7 A gtest covers the confirming call: a call whose own clock sample precedes the threshold does not fence, however long its walk takes
- [ ] #8 The model that owns the timing of GCFence gets a sabotage in which an observer credits a sighting with a sample taken before its read and fences a holder inside its authority
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
The fix and its tests are specified as step one of docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md (section 'Step one: GC counts a sighting from after its read').

Model result (CaMountObserveCore, commit 11a0675f8d3): the defect is reachable in today's code without the new renewal rule when the renewal thread is stalled, because the holder then keeps authority to start + TTL; the early self-fence hides it only when renewals fail. Sabotages sab_sighting_before_read and sab_redecision_stale_sample reproduce it; the walk bound is threshold - TTL (11.5 s, 10 s with 5 percent drift).

2026-10-02: implemented on local branch `fix/antalya-26.6/cas-mount-renewal-no-deadline` (worktree `cas-95-1`, base `41824f955e7`, head `7443ccd3a86`, not pushed, no PR yet). Commits `703b9ab01b0` (`TokenWatch` replaces both hand-written copies) and `38c5ac5fb65` (`computeHeartbeatFloor` takes the clock function and dates every sighting after its read, including the re-decision after a refused fence-out). Tests `CASTokenWatch.CountsFromAfterTheRead`, `CASHeartbeat.GcCountsASightingFromAfterItsRead` (red on the base). The `rebuildBaseline` caller has no test of its own. Gates on the head: `CAS*` unit gate 2604 passed; integration module `test_cas_mount_renewal_retry` 6 passed. No ASan run yet. Two whole-branch reviews found no defect in production code. Spec: `docs/superpowers/specs/2026-10-01-cas-mount-renewal-without-lease-deadline-design.md`; plan: `docs/superpowers/plans/2026-10-02-cas-mount-renewal-without-lease-deadline-part1.md`.
<!-- SECTION:NOTES:END -->
