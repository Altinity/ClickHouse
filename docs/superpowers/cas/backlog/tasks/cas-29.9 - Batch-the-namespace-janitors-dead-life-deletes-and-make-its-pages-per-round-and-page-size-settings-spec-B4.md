---
id: CAS-29.9
title: >-
  Batch the namespace janitor's dead-life deletes and make its pages per round
  and page size settings (spec B4)
status: In Progress
assignee: []
created_date: '2026-08-21'
updated_date: '2026-10-05 15:50'
labels:
  - 'area:gc'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:2031-triage'
  - 'theme:parity'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasNamespaceJanitor.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2298'
  - 'https://github.com/Altinity/ClickHouse/pull/2483'
  - 'https://github.com/Altinity/ClickHouse/pull/2468'
documentation:
  - >-
    docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md#b4-janitor
  - >-
    docs/superpowers/specs/2026-09-15-cas-gc-dead-namespace-debris-cleanup-design.md
parent_task_id: CAS-29
priority: medium
type: enhancement
ordinal: 115000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The janitor runs one page of a hard-coded 1,000 keys per round (`CA/Gc/CasGc.cpp:363`, `Gc::runNamespaceJanitorPage`) and
deletes each dead-life key by HEAD + exact remove (`CA/Gc/CasNamespaceJanitor.cpp:111,131`); the batch path is unused.
A full page costs 1.3-7.4 s (~7 ms per key); the limiter is request count. Under churn it cannot keep up: msan rig round
24 s to 597 s in 3.4 h, `CASGCPendingReclaim` 81 to 35,351, 93-95% of listed keys dead-life debris; parallel stateless lane
`defer_decision` 673 ms to 19.7 s in 40 min with 1,404 namespaces listed for 40 live tables.
Changes: (1) dead-life `_log`/`_snap` keys are write-once, so delete them through `removeChunkWriteOnceOrOneByOne`
(`CasGc.cpp:382`), a page becoming 1-2 batch requests; (2) `gc_janitor_pages_per_round` (default 1) and `gc_janitor_page_keys`
(default 1000) become settings. The phase keeps its position: moving it before `defer_decision` breaks the invariant
(`suppress_destructive` comes from the fold verdict). Taking more pages while debris dominates is bounded by the stage C deadline.

Provenance: BACKLOG/gc.md#janitor-page-hardcoded (formerly [gc-backlog-runaway]/2031-triage CAS-034; asks 1-2 and the revised order) and #gc-namespace-janitor-one-page-per-round (item 1). u01's pointer bullet merges here. Verified 2026-09-26 against 59494ebf366 and 0dbbd797792 (1000 hard-coded on both).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A dead life with P write-once keys drains in ceil(P/1000) delete requests
- [ ] #2 The pages-per-round and page-size settings are honoured and documented
- [ ] #3 An A/B on the msan rig with pages=10 shows `CASGCPendingReclaim` and round duration no longer growing with the run
- [x] #4 Exact-token deletes remain for every non-write-once key the janitor removes
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [x] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [x] #2 ASan lane green for touched suites
- [x] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-10-01 21:45: after owner-driven simplification rounds (pool pattern from #2351 instead of a window; telemetry/events/seams/duplicate tests cut; YAGNI cuts 1–5) PR #2468 head is 22afe53066b: +3296/−729 (src +719/−252, tests +2505/−431, docs +72/−46), down from +4123/−723 at the first push. Spec synced 1b3cf19130b.

2026-10-05: draft PR #2483 (branch `fix/antalya-26.6/cas-gc-janitor-time-budget`, head a39bccb67de, 19 files, +858/−65) replaces PR #2468. `Gc::runNamespaceJanitor` repeats `NamespaceJanitor::runOnePage` while a page deleted something, under a 20 s soft budget; dead `_log`/`_snap` keys go as write-once batches split evenly across the GC I/O pool; `_ckpt`/`_files` keep the exact-token delete. Local gates on the pushed tree: debug `CAS*` 2569, ASan `CAS*` 2573, `tests/integration/test_cas_janitor_drain` 2/2.
AC #1 is not met as written: a 1000-key page goes as up to `cas_gc_io_concurrency` requests (16 x 63 keys), not one. AC #2 was replaced by the time budget (no pages-per-round or page-size setting). AC #3 (A/B) is not done. Remaining work is in the subtask created today.
<!-- SECTION:NOTES:END -->
