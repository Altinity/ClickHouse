---
id: CAS-230
title: >-
  Unwrap the lazy table proxy before `SYSTEM SYNC REPLICA` checks for a
  materialized view
status: To Do
assignee: []
created_date: '2026-08-22'
updated_date: '2026-09-26 12:39'
labels:
  - 'area:upstream'
  - 'area:replication'
  - 'complexity:trivial'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:review'
milestone: m-5
dependencies:
  - DRAFT-18
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
priority: low
type: upstream
ordinal: 288000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`trySyncReplica` casts to `StorageMaterializedView` without `unwrapTableProxy` (`src/Interpreters/InterpreterSystemQuery.cpp:2160`) and unwraps
only for the replicated cast (`:2166`), on both branches. With `lazy_load_tables` an unmaterialized MV stays a `StorageTableProxy`, so the MV branch
is skipped and the verb misreports the MV as not replicated instead of syncing its target. Not CAS code; the `lazy_load_tables` class.

Provenance: BACKLOG/testing-and-ci.md#sync-replica-lazy-mv-proxy (opus-review triage T11); verified 2026-09-26 against 8b87aa15d21 and 8d62c314ec1. Siblings: u09-oper-c lazy-proxy-* tasks.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SYSTEM SYNC REPLICA` on an unmaterialized lazy MV syncs its replicated target table
- [ ] #2 A stateless test covers a lazy database with an MV over a replicated table
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
First recorded: 2026-08-22 (512e55b5bc7, by 'sync-replica-lazy-mv-proxy')
<!-- SECTION:NOTES:END -->
