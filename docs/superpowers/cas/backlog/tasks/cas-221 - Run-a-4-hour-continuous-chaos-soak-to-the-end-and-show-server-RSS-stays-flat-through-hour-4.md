---
id: CAS-221
title: >-
  Run a 4-hour continuous chaos soak to the end and show server RSS stays flat
  through hour 4
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:38'
labels:
  - 'area:soak'
  - 'complexity:large'
  - 'risk:low'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:soak'
milestone: m-0
dependencies:
  - CAS-140
  - CAS-29.9
  - CAS-93
  - CAS-215
  - CAS-214
references:
  - utils/ca-soak/soak/run.py
  - utils/ca-soak/scenarios/RUN_HISTORY.md
documentation:
  - docs/superpowers/cas/2026-09-16-msan-cas-s3-shard-budget-rca.md
priority: high
type: task
ordinal: 279000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
No soak has run 4 hours with chaos. The only attempt (2026-06-28) stopped at ~106 min on the TTL-band oracle, since fixed
(`utils/ca-soak/scenarios/RUN_HISTORY.md:70`); a `workers=6` attempt was stopped because the pool grew 2.4 GB/min against a 60 GiB watchdog floor (`:68`).
An earlier hour-4 run ended in a server out-of-memory at ~49 GiB RSS (B165). The streaming `publishBlob` path (`fe80d150eec7`) should have removed the
full-body copy, but nothing has re-measured it. A long soak is the only evidence of long-running stability before a customer cluster.
Remaining blockers: fsck reaching a verdict at scale, the janitor page budget and batched deletes (2026-09-16 MSan RCA K1-K3), and the harness's false failures.

Provenance: BACKLOG/testing-and-ci.md [4h-continuous-chaos-soak]; absorbs [B165] (operability-and-introspection.md#b165-server-oom-hour4-soak, merged by u09-oper-c). Verified 2026-09-26 against 8b87aa15d21. Priority High, M1: the only long-running stability evidence before the first deployment.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A phase-3 soak with chaos runs 4 hours to completion with `dangling=0` and a complete fsck verdict at the end
- [ ] #2 Peak server RSS stays flat through hour 4: no growth trend after warm-up, and nowhere near the ~49 GiB of B165
- [ ] #3 The run and its metrics are recorded in `RUN_HISTORY.md`
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
First recorded: 2026-09-26 (5245fbc7a76, by '4h-continuous-chaos-soak')

Merged from u19b-soak (merge into CAS-221): Third non-CAS blocker from the 2026-07-07 4h chaos run, still open: the pool budget cannot be held. `_THROTTLE_MAX = 1.0` s per insert (`utils/ca-soak/soak/run.py:1166`) is the heaviest pacing and never stalls inserts, RustFS does no background compaction, and merge and mutation write amplification outpaced insert pacing (pool reached 187 GB in 4 h). Options recorded: a compacting store on the stand, a lower object-count scale, or a throttle that fully stalls inserts over budget. The run's other two blockers are handled: the TTL-band abort by `4501cc8415c`, fsck at scale by CAS-140. CAS correctness held through that run: replica agreement and `dangling=0` while fsck completed.
<!-- SECTION:NOTES:END -->
