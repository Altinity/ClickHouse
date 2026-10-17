---
id: CAS-214
title: >-
  Stop the soak scoring a designed fence-and-recover after a near-TTL pause as a
  late-PUT violation
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:soak'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/soak/chaos.py
  - utils/ca-soak/soak/signals.py
  - utils/ca-soak/soak/run.py
priority: medium
type: bug
ordinal: 272000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
GCS soak `ca_live_20260904_r2`: a `both pause` of 29 s against a 30 s lease TTL fenced `ch2`; its ref lane went `CASRefNeedsRecovery`, then
remounted normally. The data model matched byte for byte and fsck was clean, yet the checkpoint aborted the driver as a "LATE-PUT FENCING VIOLATION".
`PAUSE` draws 5-60 s (`utils/ca-soak/soak/chaos.py:16`) against a 30 s TTL, and `CASRefNeedsRecovery` is a cumulative must-stay-zero counter
(`soak/signals.py:94`, gate at `soak/run.py:871`), so one correct recovery keeps it nonzero forever.
Options: classify a pause within 1 s of the TTL, or longer, as `freeze_long`; or exempt the counter when the lane has since remounted and the model matches.

Provenance: BACKLOG/testing-and-ci.md#soak-harness-needsrecovery-after-near-ttl-pause; verified 2026-09-26 against 8b87aa15d21 (harness is not on antalya-26.6). Not a duplicate of CAS-162.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A pause at or beyond the lease TTL minus 1 s is scheduled or scored as `freeze_long`
- [ ] #2 A recovery that ends in a remount with a matching model does not fail the checkpoint; a lane still in `NeedsRecovery` does
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
First recorded: 2026-09-04 (69defeaac9e, by 'soak-harness-needsrecovery-after-near-ttl-pause')
<!-- SECTION:NOTES:END -->
