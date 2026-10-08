---
id: DRAFT-22
title: >-
  Make whole-database `DROP REPLICA` and `RESTART REPLICAS` see unmaterialized
  lazy tables
status: Draft
assignee: []
created_date: '2026-08-04'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:upstream'
  - 'area:replication'
  - 'complexity:small'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies:
  - DRAFT-18
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
priority: medium
type: bug
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
If the feature stays. The `DROP REPLICA` safety scan casts `iterator->table()` to `StorageReplicatedMergeTree` without
`unwrapTableProxy` (`src/Interpreters/InterpreterSystemQuery.cpp:1747-1760`), so it misses a local lazy table with the same
ZooKeeper path; `dropStorageReplicasFromDatabase` (`:1802-1805`) and `restartReplicas` (`:1631`) skip lazy tables the same way,
so a stale remote replica may stay uncleaned. The single-table verbs already unwrap (`:283`, fix `2ba28ac4b6f`).

Provenance: BACKLOG/operability-and-introspection.md#lazy-load-tables-decision-2026-07-21 (T15 follow-ups; formerly [drop-replica-stop-proxy-forwarding-tails]); verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The safety scan refuses to drop the ZooKeeper path of a never-accessed local lazy replicated table
- [ ] #2 `SYSTEM DROP REPLICA ... FROM DATABASE` and `SYSTEM RESTART REPLICAS` act on never-accessed lazy tables in a stateless test
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
First recorded: 2026-08-04 (f08734d17df, by 'drop-replica-stop-proxy-forwarding-tails')
<!-- SECTION:NOTES:END -->
