---
id: CAS-253
title: >-
  Find why a replica killed mid-`MOVE PARTITION` keeps a duplicated partition
  after restart
status: To Do
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:40'
labels:
  - 'area:replication'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:soak'
milestone: m-8
dependencies: []
references:
  - src/Storages/MergeTree/MergeTreePartsMover.cpp
  - utils/ca-soak/scenarios/cards/s36_s37_disk_move.py
priority: high
type: bug
ordinal: 318000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The S37 chaos leg once hard-killed a replica during `ALTER TABLE ... MOVE PARTITION ID 'all' TO VOLUME 'cas'` and after the
restart the partition was duplicated: `count() = 200`, `uniqExact(id) = 100`. A later merge consolidates the parts but keeps
both copies of every row, so it does not heal. Single-part `MOVE PART` (S36) stays green; only multi-part `MOVE PARTITION`
duplicates. The same duplication was seen before MOVE-to-CA worked, through another failure mode, so it is likely a generic
crash-atomicity or replication-replay bug.
S37's kill is best-effort timing (`utils/ca-soak/scenarios/cards/s36_s37_disk_move.py:743-771`), and later S37 rows pass
(`RUN_HISTORY.md:648`, `:699`): the defect is intermittent, not fixed.

Provenance: BACKLOG/replication.md#killed-mid-move-partition; verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A deterministic repro exists, for example a failpoint between the per-part moves of one `MOVE PARTITION`
- [ ] #2 The same repro is run on a non-CA two-disk policy, and the report says whether the bug is generic or CA-specific
- [ ] #3 A fix or an upstream issue exists, and the repro shows `count() = uniqExact(id)` after the kill
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
First recorded: 2026-08-04 (0f266066bef, by 'killed-mid-move-partition')
<!-- SECTION:NOTES:END -->
