---
id: CAS-312
title: >-
  Create S40's table on both replicas so the ch2 kill hits a replica and the end
  checkpoint stops failing
status: To Do
assignee: []
created_date: '2026-09-26 14:44'
updated_date: '2026-09-26 14:47'
labels:
  - 'area:soak'
  - 'area:testing'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - utils/ca-soak/scenarios/cards/s40_insert_dedup_outage.py
  - utils/ca-soak/scenarios/framework/checkpoint.py
  - utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py
priority: medium
type: bug
ordinal: 391000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S40 is the regression gate for the 2026-07-17 acked-then-lost INSERT (phantom dedup znode under an S3 outage plus a replica kill).
It creates its `ReplicatedMergeTree` only on ch1 (`utils/ca-soak/scenarios/cards/s40_insert_dedup_outage.py:57`), then kills and
restarts ch2, which holds no replica of the table. The shared end checkpoint quiesces every node, so every S40 run records
`quiescence failed` (connection refused in 2026-07; `Table default.s40_dedup_outage does not exist` on ch2 on 2026-08-31 and 2026-09-01)
and skips the quiesced part of the end checkpoint, while the row still reads `pass`.
S39 had the same defect and was fixed by creating the table on every node (`8e39ea2fba4`).

Root cause of the silent pass: `utils/ca-soak/scenarios/checkpoint.py:37` sets `result.add_inconclusive = True`, but `report.py` `finalize()` looks only at `verdicts`, so the flag is never read.

Provenance: utils/ca-soak/scenarios/BACKLOG.md#S40-20260717T090957-1, #S40-20260718T003637-1, #S40-20260719T000359-1; verified 2026-09-26 against 66087be0ffb (cas-gc-rebuild). The quiescence exception path is `framework/checkpoint.py:35-38`.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S40 creates its table on every node and waits for both replicas before the fault window, or its docstring states why one replica is intended and the end checkpoint is scoped to ch1
- [ ] #2 An S40 run on HEAD records no `quiescence failed` anomaly and its row in `RUN_HISTORY.md` has an empty note
- [ ] #3 A failed quiescence step downgrades the scenario verdict: `checkpoint.py` sets `result.add_inconclusive = True` but `report.py` `finalize()` never reads it, so the run must end as inconclusive (or fail) instead of `pass` when quiescence fails
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
Merged from u19d-soak (S40 end-checkpoint quiescence fails on ch2 (resolve to the CAS id u19c's `s40-card-replica-shape` gets at import)): Three more S40 records with the same cause: 2026-08-31 01:41 (dev), 2026-08-31 23:10 (ci), 2026-09-01 06:18 (dev). Each is status pass with `quiescence failed: ... Table default.s40_dedup_outage does not exist (UNKNOWN_TABLE)` on localhost:8124 (`RUN_HISTORY.md:577,651,702`).
The framework-level fix in u19d `quiesce-only-hosted-replicated-tables` would cure S40, S43 and S46 together. If it lands first, close this one as done by it.
Source: utils/ca-soak/scenarios/BACKLOG.md:3311, :3437, :3481.
<!-- SECTION:NOTES:END -->
