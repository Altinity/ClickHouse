---
id: CAS-29.2
title: >-
  Land PR #2351 in its rework shape: round-scoped GC pools and a parallel
  `pending_deletes` (spec A3)
status: Done
assignee:
  - '@k-morozov'
created_date: '2026-08-04'
updated_date: '2026-09-29 21:19'
labels:
  - 'area:gc'
  - 'complexity:large'
  - 'risk:medium'
  - 'touches:settings'
  - 'confidence:solid'
  - 'origin:soak'
  - 'origin:canary'
milestone: m-1
dependencies: []
references:
  - CA/Gc/CasGc.cpp
  - CA/Gc/CasGc.h
  - CA/Gc/CasGcMetaWriter.h
  - 'https://github.com/Altinity/ClickHouse/pull/2351'
  - 'https://github.com/Altinity/ClickHouse/issues/2429'
documentation:
  - docs/superpowers/specs/2026-09-25-cas-gc-rounds-in-minutes-design.md
  - >-
    docs/superpowers/specs/2026-09-14-cas-gc-round-pool-and-parallel-redelete-design.md
    (rev. 11
  - 'on the PR #2351 branch only)'
  - docs/superpowers/reports/2026-08-04-gc-destructive-baseline-perf.md
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-29
priority: high
type: feature
ordinal: 108000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The blob redelete loop is serial: per entry one `op.head`, a token compare, one conditional `op.remove`
(`CA/Gc/CasGc.cpp:718-731`, `redelete_now` loop in the fold's `pending_deletes` phase). Measured: 44 ms per blob and 52%
of a healthy day on otel.demo (audit conclusion 4); 351 s for 1,261 blobs on real AWS; 551 s for 2,731 blobs on GCS;
in the T9 baseline wall time was 87% of the summed request time, so the requests barely overlap.
Each pair is independent and exact-token per key (I5), so concurrency changes nothing about safety.
The same change makes the pools per round: today `Gc` owns `read_pool` (`CA/Gc/CasGc.h:982`, 16 threads) and
`GcMetaWriter` its own pool (`CA/Gc/CasGcMetaWriter.h:86`, `gc_meta_pool_size` 16) for the disk's lifetime, which was
523 idle threads at 30 inline disks. PR #2351 (`altinity/cas/gc-parallel-delete-blobs`, not merged on either branch) still
carries a per-`Gc` `io_pool`; its approved rework spec (2026-09-14, rev. 11) asks for a round-scoped `GcRound` with
`io_pool` and `meta_writer` and one setting `cas_gc_concurrency` (default 16).
Shape: chunks from `redelete_now` run head, compare, remove on workers under the round's admitted generation; outcomes,
events and meta scheduling are applied on the round thread in index order within a shard; `authority_held` is checked
before each chunk. The read-ahead still never runs a destructive decision.

Provenance: BACKLOG/gc.md#gc-pending-deletes-fan-out (formerly [gc-delete-concurrency-serial]), #gc-blob-pending-deletes-now-dominant, #gc-per-disk-thread-pools; janitor-page-hardcoded ask 4 (bound the work per round). Verified 2026-09-26 against 59494ebf366 and 0dbbd797792: serial loop and per-disk pools identical on both; PR branch last commit a2f53e543a6 (2026-09-25) is an ancestor of neither.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 An instrumented-backend gtest shows N deletes in flight at concurrency N and peak 1 at concurrency 1
- [ ] #2 A fence lost mid-chunk issues no delete after it; a token mismatch inside a chunk is `Replaced` and the object stays
- [ ] #3 `objects_deleted`, `objects_spared`, `objects_replaced` and outcome rows equal a `cas_gc_concurrency = 1` run on the same input
- [ ] #4 A mass-removal round on the AWS stand has `pending_deletes` wall time at most 2 x (serial wall / N), with no `SlowDown`/503 at the chosen N
- [ ] #5 No GC thread pool outlives a round: an idle CAS disk holds no GC worker threads
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
- [ ] #5 ASan and TSan `CAS*` gate on the exact pushed tree
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
Landed with PR #2351 (merge 61d24118251 into antalya-26.6, 2026-09-29): GcRound with a per-round io_pool exists; the setting is cas_gc_io_concurrency (not the rework spec's cas_gc_concurrency) and gc_meta_pool_size / the per-disk meta pool remain, so rework AC #1 (one setting) and #5 (no pool outlives a round) are only partly met. Not in cas-gc-rebuild (branches diverged; carry-over is its own task).
<!-- SECTION:NOTES:END -->
