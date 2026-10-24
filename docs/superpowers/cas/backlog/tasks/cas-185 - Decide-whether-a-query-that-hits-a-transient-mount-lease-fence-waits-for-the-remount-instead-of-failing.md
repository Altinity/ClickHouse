---
id: CAS-185
title: >-
  Decide whether a query that hits a transient mount-lease fence waits for the
  remount instead of failing
status: To Do
assignee: []
created_date: '2026-09-04'
updated_date: '2026-09-26 12:38'
labels:
  - 'area:mounts'
  - 'area:write-path'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'needs:decision'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - docs/superpowers/cas/2026-09-04-stateless-lane-triage.md
  - R/Pool/CasServerRoot.cpp
priority: medium
type: design
ordinal: 242000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Under S3 endpoint saturation the renewer fenced after 3 attempts in 9068 ms; 371 "mount lease not held" lines followed, almost all self-healing background retries.
Two tests surfaced it to a synchronous client as `Code: 210 ... mount lease not held`: `01128_generate_random_nested` (INSERT's ref-log append) and `02581_share_big_sets_between_mutation_tasks` (`StorageMergeTree::waitForMutation`). Frequency 2/11137 on the local CA-s3 lane.
The fence itself is correct and fail-close. Open: should such a query wait for the remount, bounded by its own timeout, and where does that wait belong (ref-lane append, `waitForMutation`, or a disk-level "mount recovering" retry)?
Related: `txn-metadata-store-noexcept-class-fix` (the same retry-later reaching synchronous callers).

Provenance: BACKLOG/ref-protocol.md#cas-transient-lease-fence-surfaces-to-clients. Verified 2026-09-26 against b1c34d03479 and 0dbbd797792: no wait-for-remount path exists.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The owner records a decision on where, if anywhere, a query waits for a remount
- [ ] #2 If a wait is chosen, a test forces a fence during an `INSERT` and the query succeeds after the remount within its timeout
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
First recorded: 2026-09-04 (bbe9972e7a1, by 'cas-transient-lease-fence-surfaces-to-clients')
<!-- SECTION:NOTES:END -->
