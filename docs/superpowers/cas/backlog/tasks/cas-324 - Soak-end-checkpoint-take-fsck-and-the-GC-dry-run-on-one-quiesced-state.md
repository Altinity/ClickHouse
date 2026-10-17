---
id: CAS-324
title: 'Soak end checkpoint: take fsck and the GC dry-run on one quiesced state'
status: To Do
assignee: []
created_date: '2026-09-30 09:25'
updated_date: '2026-09-30 12:52'
labels:
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
dependencies: []
references:
  - docs/superpowers/reports/2026-09-30-soak-s11-dryrun-subset-red-rca.md
  - utils/ca-soak/scenarios/framework/checkpoint.py
priority: medium
type: bug
ordinal: 410000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The verdict `dryrun ⊆ deletable` fails by timing. The end checkpoint runs `fsck_detail` and then `cas-gc-dryrun` as two processes with no shared snapshot (`utils/ca-soak/scenarios/framework/checkpoint.py:76-84`), and the quiescence check does not cover the ReplicatedMergeTree cleanup thread.
Observed 2026-09-30, S11 `--scale ci`, seed 20260930, binary antalya-26.6 @61d24118251, 1 of 6 runs: the cleanup thread removed 3834 outdated parts one second after fsck started; a GC round then folded the removals and condemned 2965 blobs (deleted 0, graduated 0); the dry-run listed them while fsck had seen them reachable. Not a GC decision defect: the round deleted nothing. A sibling run missed the window by 10 s.
RCA with the timeline: `docs/superpowers/reports/2026-09-30-soak-s11-dryrun-subset-red-rca.md`.
Second harness race seen the same day: S46 failed at entry with 'a GC leader was identified: none' 16 s after the stack came up; the rerun passed. The scenario reads the leader once instead of waiting for one.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Before the final fsck and dry-run the checkpoint waits until outdated-part removal has finished and GC has settled (a round with no changed shards)
- [ ] #2 The verdict is INCONCLUSIVE, not FAIL, when the GC round number moved between fsck and the dry-run
- [ ] #3 Deterministic reproduction: stop the cleanup thread, run fsck, restart cleanup, run one GC round, run the dry-run; the checkpoint reports INCONCLUSIVE or waits
- [ ] #4 S46 waits for a GC leader with a deadline instead of reading it once
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
Third occurrence of the same race, 2026-09-30, S11 `--scale ci` with `cas_gc_interval_sec` = 300 (one round folds 44-60k edges): the leak verdict failed in 2 of 3 runs of one binary and 0 of 2 of another, with 9 and 63 unreachable `_manifests` left. In both failures a `RemovePart` batch of the writers' outdated-part cleanup landed 6-7 s after the last GC round had listed the ref stream and before the final fsck (21 parts -> 63 manifests, 3 parts -> 9); the 150-s drain deadline expired before another round. A live rerun passed the harness and then held 15,677 unreachable `_manifests` with 4,130 outdated parts still queued; forced rounds took it to 0 within 6 rounds and it stayed 0 for 33 more. So it is not a leak. Fix as in the first acceptance criterion: wait for zero outdated parts before the final rounds.
<!-- SECTION:NOTES:END -->
