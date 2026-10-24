---
id: CAS-29.6
title: >-
  Make covered ref-object cleanup list its own life's range instead of the
  fold-time global listing (spec B2)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:gc'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-1
dependencies:
  - CAS-29.1
references:
  - CA/Gc/CasGc.cpp
  - CA/Pool/CasRefProtocol.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#b2-cleanup-range
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f14
parent_task_id: CAS-29
priority: high
type: feature
ordinal: 112000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`Gc::cleanupRefObjects` (`CA/Gc/CasGc.cpp:3548`) plans from the fold-time `listRefPrefix` result, so it cannot outlive the
global LIST that B1 removes. Covered `_log` keys it fails to delete stay listed by every round and become janitor debris
at DROP: 880k new keys/day against 600k deleted under a 200k cap on otel.demo (audit F14), 4.6M keys listed per round.
B2: for every `Live`/`Removing` life whose checkpoint names a valid recovery triple, LIST `<life>/_log/` from the start and
stop at the first key above the bound, LIST `<life>/_snap/`, keep the existing rule of `planRefCleanup`
(`CA/Pool/CasRefProtocol.cpp:824`), and delete in chunks through `removeChunkWriteOnceOrOneByOne`. No cursor: deleted keys vanish.
Before reusing the LIST path confirm `max_keys` propagation into `S3ObjectStorage::iterate` (audit F6).

Provenance: spec B2, motivated by BACKLOG/gc.md#covered-log-cleanup-aborts-on-catalog-etag ('why it matters beyond cleanup'). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Keys below the bound are deleted in batches; a key equal to the checkpoint or above the durable cursor is never deleted
- [ ] #2 A second round with nothing below the bound issues one LIST per life and no delete
- [ ] #3 The phase has no input from the fold-time global listing
- [ ] #4 The `ref_object_cleanup` phase row in `system.cas_gc_log` carries `objects_deleted` and `objects_pending` (issue #2429 observability gap)
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `Removing` is 2026-06-07 (996d156fdfb); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->
