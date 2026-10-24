---
id: CAS-316
title: >-
  Make the soak end checkpoint run `SYSTEM SYNC REPLICA` only on nodes that host
  the table as a replicated table
status: To Do
assignee: []
created_date: '2026-09-26 14:47'
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
  - utils/ca-soak/scenarios/framework/lifecycle.py
  - utils/ca-soak/scenarios/framework/checkpoint.py
  - utils/ca-soak/scenarios/cards/s43_same_uuid_recreation.py
  - utils/ca-soak/scenarios/cards/s46_restart_under_gc.py
priority: low
type: bug
ordinal: 395000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`quiesce_cluster` runs `SYSTEM SYNC REPLICA <t>` on every node for every table the card passes (`utils/ca-soak/scenarios/framework/lifecycle.py:120-133`). It tolerates only read-only and node-down errors and raises on anything else. The end checkpoint then records "quiescence failed" and skips the drain (`framework/checkpoint.py:33-38`).
Three cards pass tables that do not fit that shape. S43 creates a plain `MergeTree` on ch1 (`cards/s43_same_uuid_recreation.py:87,149`), so ch1 answers `BAD_ARGUMENTS ... is not replicated`. S46 creates its tables on ch1 only (`cards/s46_restart_under_gc.py:102-112`), and S40 does the same (`cards/s40_insert_dedup_outage.py:57`), so ch2 answers `UNKNOWN_TABLE`.
Every S43 run since 2026-07-29 carries the anomaly, and so do the S40 runs since 2026-08-31 and the only S46 run, while their status reads pass (`RUN_HISTORY.md:507,516,532,579,654,705,719`). The end-state checks therefore run without the replication-queue and merge drain they assume.
Fix: before the loop, read `system.replicas` per node and sync only the hosted replicated tables; still drain the queue and merges for the rest. A table that is missing from every node stays an error.
u19c's `s40-card-replica-shape` fixes the S40 card alone; this framework fix covers all three.

Provenance: utils/ca-soak/scenarios/BACKLOG.md S43 records at :3234, :3248, :3269, :3318, :3451, :3488 and S46 at :3502; related S40 records merged into u19c:s40-card-replica-shape. Verified 2026-09-26 against 56bf63c9fa7 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 S43, S46 and S40 dev runs record no `quiescence failed` anomaly and report `quiesce_s`
- [ ] #2 A card that passes a table absent from every node still gets a quiescence failure
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->
