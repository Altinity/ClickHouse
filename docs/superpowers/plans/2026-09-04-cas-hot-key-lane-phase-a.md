---
description: 'Status and remaining work for phase A of the CAS hot-key write lane. Tasks 0-6 (the FIFO ticket, the cache, Half 2 conflict pacing, the catalog caller, the pool wiring, the gate/review/merge) shipped and are DONE. Task 7, the acceptance measurement, and the ASan run of the death-test twin are still open and tracked in BACKLOG.'
sidebar_label: 'CAS hot-key lane phase A plan'
sidebar_position: 9
slug: /superpowers/plans/cas-hot-key-lane-phase-a
title: 'CAS Hot-Key Lane Phase A Implementation Plan'
doc_type: 'plan'
---

# CAS hot-key write lane, phase A — Implementation Plan {#cas-hot-key-lane-phase-a-implementation-plan}

> Groomed 2026-09-25. Tasks 0-6 of the original 8-task plan (Task 0 through Task 6) are DONE:
> implemented, tested, reviewed and merged. Only Task 7 (the acceptance measurement) remains
> open. This file is trimmed to that remaining task plus a status summary; the full original
> plan (Tasks 0-6 with their test code, step-by-step instructions and self-review) is recoverable
> from git history at `docs/superpowers/plans/2026-09-04-cas-hot-key-lane-phase-a.md` before this
> trim.

**Goal (shipped):** Every catalog mutation of one pool waits its turn in a per-key FIFO above the
request engine, starts from the pool's last known object when the cache holds one, and is repaid
after a flat jitter when it loses a race to another server, so the in-process 412 storms and the
growing conflict backoff that made `DROP TABLE` p90 11.9 s disappear.

**Spec:** `docs/superpowers/specs/2026-09-04-cas-hot-key-write-lane-design.md` (revision 34,
phase A).

## Status of Tasks 0-6 {#status}

Implemented on branch `cas-hot-key-lane` (13 commits over `e59fe7e8e4b`), merged into
`cas-gc-rebuild` at `9bf134686af` ("ca: merge the hot-key write lane, phase A (spec rev.34, plan
2026-09-04)"), and landed on the release branch at `altinity/antalya-26.6` commit `4ec755474fb`
("cas: hot-key write lane, phase A — serialize and combine a process's own writes to a hot
control object"). `CAS*` gate 2406/2406 on the final branch tree; whole-branch review (opus):
mergeable; end-to-end review (codex `gpt-5.6-sol`, high): three MAJORs found and folded
(`bc57cdcdb5e`, `9ac8a45c405`, `2f1c51aa450`). Full rulings and checklist verification:
`docs/superpowers/cas/2026-09-04-hot-key-lane-phase-a-rulings.md`.

| task | verdict | evidence |
|---|---|---|
| Task 0 (branch, baseline) | DONE | `cas-hot-key-lane` branched from `cas-gc-rebuild` at `e59fe7e8e4b` |
| Task 1 (`Conflict::any_ambiguous`, `conflictBackoff`, `pauseForConflict`) | DONE | `c22ae7b9c9b`; `Backend/CasWriteResult.h`, `Backend/CasRetry.h`, `Backend/CasRequests.{h,cpp}` |
| Task 2 (`CasHotKeys` lane, ticket, hold) | DONE | `836be9fb454` (+ fix rounds `4cf344d73d3`, `2e5f1a19ed0`); `Backend/CasHotKeys.{h,cpp}` |
| Task 3 (the cache under one rule) | DONE | `74eff7b8380`; `src/Disks/tests/gtest_cas_hot_keys.cpp` |
| Task 4 (catalog writes through the lane) | DONE | `c76d3952933` (+ `56080dae51f`, `1216f815837`); `Pool/CasRefCatalog.cpp` `casUpdateImpl` calls `op.hotKeys().submit` |
| Task 5 (pool owns the lane) | DONE | `2b46c3dbd91`; `Pool/CasPool.h` `PoolConfig::hot_key_cache_bytes`, `Pool::hot_keys` |
| Task 6 (gate, review, merge) | DONE except the ASan sub-step | gate green, both reviews done, merged at `9bf134686af`; **Step 2 (sanitizer run of the lane tests) was never executed** — no ASan build directory exists in `lane-g` or `master`; tracked below |

The death-test twin `CASRefCatalogDeathTest.AStaleHintCasAdmitEntryAdmitsWhenTheStoreHasRoomAborts`
(`src/Disks/tests/gtest_cas_ref_catalog.cpp:2516`, present on both `cas-gc-rebuild` and
`altinity/antalya-26.6`) has still never run under a debug or sanitizer build. This is tracked in
`docs/superpowers/cas/BACKLOG.md` under `{#hot-key-lane-phase-a-followups}`, not re-added here.

---

### Task 7: The acceptance measurement {#task-7}

**Status: OPEN.** Not started as of this grooming pass (2026-09-25); no run log, commit, or
BACKLOG update records it having happened.

**Files:** none. This task produces numbers, recorded in `docs/superpowers/cas/BACKLOG.md` under
`{#ref-catalog-cas-starvation}` (a paragraph "Measured after phase A") and, for the phase B gate,
under `{#hot-key-lane-phase-b}`.

- [ ] **Step 1: Run the same workload the RCA measured**

Ten minutes of the parallel stateless suite on the CA-s3 lane, exactly as
`docs/superpowers/cas/2026-09-04-ref-catalog-starvation-rca.md` describes its run (the same lane,
the same `~10` parallel jobs, the local S3), on the merged `cas-gc-rebuild`. Record the run's
start and end timestamps.

- [ ] **Step 2: The before-and-after numbers**

On the server, over the run's window (`$from`, `$to`):

```sql
SELECT quantile(0.5)(query_duration_ms) AS p50, quantile(0.9)(query_duration_ms) AS p90, max(query_duration_ms) AS max, count() AS n
FROM system.query_log
WHERE type = 'QueryFinish' AND query_kind = 'Drop' AND event_time BETWEEN $from AND $to;

SELECT quantile(0.5)(query_duration_ms) AS p50, quantile(0.9)(query_duration_ms) AS p90, max(query_duration_ms) AS max, count() AS n
FROM system.query_log
WHERE type = 'QueryFinish' AND query_kind = 'Create' AND event_time BETWEEN $from AND $to;

SELECT count()
FROM system.text_log
WHERE event_time BETWEEN $from AND $to AND message LIKE '%PreconditionFailed%' AND message LIKE '%ref_catalog%';

SELECT event, value FROM system.events
WHERE event IN ('CASHotKeyQueueWaitMicroseconds', 'CASHotKeyCacheStarts', 'CASHotKeyReadStarts',
                'CASHotKeyCacheVerdictsReread', 'CASRequestConflictPause', 'CASRequestReissue', 'CASRequestResolveRead');
```

Take the `system.events` values at the start and the end of the window and record the deltas. The
`DROP TABLE` p90 was 11.9 s and the `PreconditionFailed` count was 113 in 80 s before; the phase A
target is a p90 near one second and a count near zero on the catalog key.

- [ ] **Step 3: The phase B gate**

Divide the `CASHotKeyQueueWaitMicroseconds` delta by the number of holds (`CASHotKeyCacheStarts +
CASHotKeyReadStarts`) for the mean queue wait per submission, and set it beside the mean `DROP
TABLE` duration. Combining (phase B) pays when the queue wait, not the write, dominates a
submission. Record both numbers and the verdict under `{#hot-key-lane-phase-b}` in the BACKLOG,
commit the BACKLOG in the master worktree with `git commit -- docs/superpowers/cas/BACKLOG.md`.

Also run, as part of the same pass, Task 6 Step 2 (never executed): build `unit_tests_dbms` under
an ASan build directory and run `--gtest_filter='CASHotKeys.*:CASRefCatalog.TheGCErase*'` (or the
whole `CAS*` gate) to exercise
`CASRefCatalogDeathTest.AStaleHintCasAdmitEntryAdmitsWhenTheStoreHasRoomAborts` and the lane's
concurrency-sensitive tests under the sanitizer at least once.
