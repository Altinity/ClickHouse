---
id: CAS-223
title: >-
  Add a sanitizer-only thread-pool profile for the CAS stateless stand so MSan
  and TSan shards fit the 6-hour budget
status: To Do
assignee:
  - '@filimonov'
created_date: '2026-09-16'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:ci'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'origin:issue'
  - 'confidence:contested'
  - 'theme:parity'
milestone: m-8
dependencies: []
references:
  - tests/config/install.sh
  - 'https://github.com/Altinity/ClickHouse/issues/2298'
documentation:
  - docs/superpowers/cas/2026-09-16-msan-cas-s3-shard-budget-rca.md
priority: high
type: task
ordinal: 281000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
MSan CAS runs 0.5k tests/h against 2.0k/h on the plain S3 lane (TSan <0.9k/h vs 2.0k/h), so those shards never fit the job budget (#2298, OPEN).
The thread census points at pools: ~2400 threads at 30 inline disks (BgSchPool 508, IOWriter 165, CAS per-disk GC pools 523).
`tests/config/install.sh` has an `is_sanitizer_build` branch (`:181-183`) and a `--cas-s3-storage` flag (`:33`) but no profile for both.
Proposed keys: `threadpool_writer_pool_size` 500 to 64-100, `background_schedule_pool_size` 512 to 128-256 (not below 128: renewal and cleanup run there),
`cas_gc_read_concurrency`/`cas_gc_meta_pool_size` 16 to 4 on the default CAS disk. Inline disks do not inherit disk settings; `SYSTEM CAS FORGET` stays their lever.
`max_thread_pool_free_size` is not a lever (tried, no change). Idle-thread retirement in the shared `ThreadPool` was rejected: wide blast radius; per-disk pools go away with round-scoped GC pools instead.
The same profile is candidate (c) for the ASan RSS ceiling (threads times fake stacks).

Provenance: BACKLOG/testing-and-ci.md#sanitizer-cas-thread-pool-profile, plus candidate (c) of #asan-memory-tracker-snap; verified 2026-09-26 against 8b87aa15d21. Related: CAS-29.2, CAS-85.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A `config.d` profile is linked only when the build is a sanitizer build and CAS-s3 storage is enabled, each key with a one-line reason
- [ ] #2 A `system.stack_trace` census per thread group before and after shows the reduction
- [ ] #3 MSan and TSan CAS shards finish inside the job budget, or the remaining gap is measured and resharding is decided
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
First recorded: 2026-09-16 (cbf3fe14e36, by 'asan-memory-tracker-snap')

Issue #2298 (assigned filimonov): the ASan half is resolved on antalya-26.6 (test-policy `cas_gc_interval_sec` 5 → 20 in `38661e72127` plus the GC round-cost work; ASan CAS-S3 shards finish in 3.7 h and 2.9 h on the v26.6.4 tag). MSan and TSan shards are what remains. PR #2393 (merged, #2362 workaround) also speeds the sanitizer lanes.

2026-09-28 (owner review): the thread-pool-profile proposal above is rejected as knob-turning; the standard is CAS as fast as the plain S3 disk on the same workload, with the loss located by metrics, traces and a synthetic repro. Systematic-debugging investigation started on PR #2440 run https://github.com/Altinity/ClickHouse/actions/runs/36347426173 (tsan 's3 storage' 1/2 finished in 2h05 vs 'cas s3 storage' 1/2 cancelled at the 6h timeout with 5,618 of 5,636 tests done; msan cas s3 1/3-3/3 cancelled at 6h). Phase 1 (tmp/cas223/phase1_ci_evidence.md): the loss is broad (top-1000 tests hold 67% of the extra time, median per-test 1.83x, p90 6.9x), confined to MergeTree tests (3.19x, ramping 2.0 -> 5.7 by elapsed hour; tests without tables 1.19x flat), and present only under TSan/MSan: the ASan CAS lane has the same threads and pools and runs 2.7-3.4k tests/h, as fast as TSan S3, which refutes the thread-count explanation. A constant 2.5x exists in release between CAS over RustFS and CAS over local objects (part-removal repoint ~140 ms release, ~570 ms TSan). CI confounds: CAS lane on RustFS without cache vs S3 lane on MinIO with a 5 GiB filesystem cache; TSan S3 lane runs with random query settings off. Phase 2 (tmp/cas223/phase2_hypotheses.md): H1 = O(pool catalog) CPU per CAS op: CasRefCatalog::read (GET + full decode + CatalogLifeIndex) on every part-commit append (CasRefLedger.cpp:3394/3425), twice per readable-runtime acquire and in listings (CasPool.cpp:1884/1912); Removing rows are erased only after a GC fold adopts cleanup evidence, so catalog = live tables + GC backlog, and a sanitizer-slowed GC grows it (feedback loop, predicts MSan > TSan > ASan). Overlaps CAS-176.1 (drop the per-flush catalog GET) and CAS-72. Next: phase 3 repro on the exact CI binaries and mining the completed-but-slow sanitizer runs' system-table dumps (query_log, metric_log, trace_log, cas_log).

2026-09-28 phase 3 (tmp/cas223/phase3_repro.md), reproduced locally with the exact CI TSan and release binaries, CAS over local objects (no RustFS): per-op cost is linear in ref-catalog entries at identical request counts (release +12-16 ms per 1000 entries per create/insert/drop cycle; TSan +260-330 ms per 1000, 20x the release slope); perf under TSan at 3000 entries: CasRefCatalog::read = 56% of all server CPU (decode 33%, CatalogLifeIndex 10%, snapshot copy/free 15%), 67% of those reads from commitRefChunk's admission re-read before every ref-log append. Why the catalog grows: Removing rows are erased only by the GC pre-fold drain, one row per full catalog rewrite via a raw optimistic op.replace, while every foreground catalog write (~11/s) goes through the per-pool hot-key lane; the drain's O(N) read-encode-write window loses nearly every race, backs off and retries, and the round cannot proceed without it. TSan GC-on run: one round in 286 s, drain 232.7 s, catalog 1987 rows, per-op 382 -> 1670 ms. Same signature in CI: the release CAS lane had 10 rounds failing with 'CAS ref catalog erase ... gave up at the policy deadline after zero attempt(s)' and rounds of 9-27 min; ASan rounds 22 s -> 10 min. Refuted: thread count (ramp reproduces at constant ~750 threads), request count, port exhaustion, GC CPU competition. Phase 4 in progress: patches tmp/cas223/patches/0001 (batch the drain through the hot-key lane) and 0002 (reuse the decoded snapshot for byte-identical GETs), before/after on a release build in ~/workspace/ClickHouse/cas223-perf.

2026-09-28 phase 4 (tmp/cas223/phase4_fixes.md, patches in tmp/cas223/patches/, built and measured in worktree cas223-perf, nothing committed): Fix A = CatalogLifecycleReconciler drains every proven completed-removal row in ONE catalog write through the per-pool hot-key lane (new CasRefCatalog::deleteCompletedRemovingBatch, same per-row proof, bounded at 16 passes then retry-later); it was the one catalog writer outside the lane and lost nearly every race, and because the drain gates the round, GC stopped. Release + RustFS + GC on, 8 workers x 1000 create/insert/drop cycles: before 578.6 s wall, 1 ok / 5 aborted rounds ('CAS ref catalog erase ... could not be committed', the CI message), 7980 removing rows at the end, 319 -> 805 ms per cycle; fix A 447.6 s, 7/0 rounds, 579 rows, 306 -> 495 ms (last bucket -38%). Fix B = CasRefCatalog::readShared returns the cached decoded snapshot when etag and bytes are identical (GET still issued, freshness unchanged), used by the two admission re-reads in commitRefChunk: removes 20-40% of the catalog-proportional cost (3800 entries: 154 -> 125 ms per cycle). Rejected: janitor full sweep (41 s, deletes nothing that round), synchronous per-key delete of drained lives inside the round (39-70 s per round, delays the next drain). Still growing after A: defer_decision (global LIST of cas/ns/stream/) 4.4 -> 45.4 s over 7 rounds because dropped tables' ref objects stay listed until the one-page-per-round janitor reaches them (dead-life debris, CAS-29.1 / K1-K3); that is the next fix. New gtests: CASCatalogLifecycleReconciler.DrainDoesNotLoseRacesToThisServersCatalogWriters, ErasesEveryProvenRowInOneCatalogWriteAndLeavesTheRest, plus updated request-count pins.

2026-09-28 FINAL (docs/superpowers/reports/2026-09-28-cas-223-rca/RCA.md): RC1 proven = GC pre-fold catalog drain outside the hot-key lane starves and aborts rounds; RC2 proven = every ref-log append fully decodes cas/ref_catalog (~29x per table lifecycle, ~15 ms per 1000 entries release, ~300 ms TSan); RC1+RC2 feedback loop steepened by the sanitizer CPU multiplier explains the MergeTree-only ramp (MSan > TSan > ASan). RC3 measured, open = request amplification: CAS issues 3.4x plain S3's requests per create/insert/drop cycle, one 3-part DROP = 19 ref-log txns each with a _log and a _ckpt PUT (overlaps CAS-167, CAS-176, DRAFT-26, CAS-83). RC4 measured, open = defer_decision LIST grows with dead-life debris 4.4 -> 45 s over 7 rounds (CAS-29.1, CAS-29.9). Fixes A+B on the exact CI TSan binary, RustFS rc.3, CI env, GC on, 8 workers: 219.3 -> 163.4 s, last-bucket cycle 1223 -> 737 ms, rounds 1 ok/1 aborted -> 3/0, catalog 1985 -> 980; release 569.4 -> 449.7 s, rounds 1/5 -> 6/0, catalog 7842 -> 1107. Plain S3 on the same RustFS: 59.0 s (TSan), 143.9 s (release): CAS still 2.8x / 3.1x slower, from RC3 and RC4. CAS* gate green (2538) with the patches; the two new tests are red on the PR head. Not claimed: that A+B alone bring the sanitizer shards under 6 h. Repro rig tmp/cas223/rig/. Next: CAS review of patch 0001 (GC-safety code), then a branch on antalya-26.6; RC3 and RC4 as their own items; export cas_gc_log and catalog size from the CI CAS lanes (CAS-85).

2026-09-28 CI confirmation of RC2 (docs/superpowers/reports/2026-09-28-cas-223-rca/phase3_ci_completed_runs.md): no TSan/MSan CAS stateless job completed in the last 200 runs (skipped everywhere except run 36347426173, all cancelled); stateless jobs upload system-table dumps only on failure, so the specimen is the failed ASan CAS shard 1/2 of run 34105324590 (PR #2320, 9091be4a672, 2026-09-07, 303 min; metric_log, query_log, trace_log, no cas_gc_log). trace_log symbolized against the matching ASan binary (47,754 distinct addresses, 98.7% resolved), early 14:00-14:45 vs late 17:00-18:15: CPU share CasRefCatalog::read 7.3% -> 20.3%, decodeRefCatalog 3.1% -> 13.3%, commitRefChunk 9.0% -> 21.3%, part removal 6.5% -> 14.3%, GC flat 7.8% -> 7.4%; top three late stacks are all catalog decode under commitRefChunk. metric_log: 3-5 of 16 cores used, CASRefQueueWaitMicroseconds 2.5k -> 16.2k s per 15 min, queries per 15 min halve. query_log INSERTs: request counts constant, S3 latency ~1.5x, ref-queue wait per INSERT 159 ms -> 1.0-1.4 s (the dominant growing term): the queue leader decodes the whole catalog while holding the per-table ref-log queue, so catalog growth becomes queue wait. GC rounds on that build grew to 55 min then 2 h 04 min with deletes capped ~4950 per round. Catalog size itself is not in the dumps; GC lag -> catalog growth remains shown locally only.
<!-- SECTION:NOTES:END -->
