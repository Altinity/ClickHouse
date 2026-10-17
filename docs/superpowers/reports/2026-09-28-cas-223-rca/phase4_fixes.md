# CAS-223 phase 4: fixes and measurements

All paths relative to `tmp/cas223/`. Patches are against PR #2440 head `58f8458c707` and were built and measured in
the worktree `~/workspace/ClickHouse/cas223-perf` (branch `cas223-perf`, uncommitted, release build `build/`, TSan build
`build_tsan/`). Nothing was committed or pushed.

Measurement rig: `phase3_repro.md`. Object store: RustFS `1.0.0-rc.3` in Docker with the CI environment
(`RUSTFS_DURABILITY_MODE=relaxed`, `RUSTFS_RUNTIME_MAX_BLOCKING_THREADS=64`, ..., `ci/jobs/scripts/clickhouse_proc.py:224-232`),
port 19121. Workload: 8 workers x 1000 CREATE / 3 INSERT / SELECT / DROP SYNC cycles, GC on with `cas_gc_interval_sec` 20 (CI value).
"Before" is the unpatched PR head built in the same tree with the same flags (`bin/clickhouse_rel_base`).

## Fix A: the catalog drain erases every proven row in one write, through the hot-key lane

Patch: `patches/0001-cas-gc-drain-batch-erase-through-hot-key-lane.patch` (product) and the test changes listed below.

**Root cause it removes.** `CatalogLifecycleReconciler::reconcile` erased completed-removal rows one per full catalog rewrite,
each with its own read-then-`op.replace` (`CasRefCatalog::deleteCompletedRemovingAtSnapshot`). Every other catalog writer of the
server goes through the per-pool hot-key lane (`casUpdateImpl` -> `CasHotKeys::submit`), whose FIFO exists because in-server
writers of one key racing each other each pay a read, a refused write, a resolve read and a growing sleep
(`Backend/CasHotKeys.h:21-38`). The drain was the one catalog writer left outside it. Under a stream of CREATE/DROP it lost almost
every race, and because the drain gates the whole round (`CasGc.cpp:473-481`), GC stopped: no fold, no deletes, no janitor, and
`removing` rows piled up in the one catalog object every foreground operation decodes.

**Change.**
- `CasRefCatalog::deleteCompletedRemovingBatch` (new): one `casUpdateImpl` hold whose `decide` removes every row the adopted
  parent proves completed (same per-row predicate as the single-row erase, factored into `isProvenCompletedRemoving`), then the
  mandatory resolution read; any erased row the read still shows throws retry-later, as the single-row erase does.
  `refresh_authority` and `op.admitted()` are checked before the lane and in every `decide`.
- `CatalogLifecycleReconciler::reconcile` selects all eligible rows and calls the batch; a second pass happens only if another
  actor changed the catalog in between; bounded at 16 passes, then retry-later.
- The single-row `deleteCompletedRemoving*` API is left in place (used by tests and tools).

**Why this layer.** It is not a knob: the lane is the server's existing serialization point for this key, and the batch keeps the
per-row proof unchanged while turning an O(backlog x catalog) drain into O(catalog). The on-S3 format is untouched.

**Tests** (`src/Disks/tests/gtest_cas_gc_frontier_gate.cpp`, in the worktree):
- new `CASCatalogLifecycleReconciler.DrainDoesNotLoseRacesToThisServersCatalogWriters`: after each catalog read the drain makes,
  one more CREATE of the same server writes the catalog, committed at once when the lane is free and queued behind the holder
  when the drain holds it (deterministic via `CasHotKeys::queueDepthForTest`); asserts zero refused catalog writes, no lost
  foreground row, no `removing` row left.
- new `CASCatalogLifecycleReconciler.ErasesEveryProvenRowInOneCatalogWriteAndLeavesTheRest`: 8 proven rows + 1 live bystander ->
  8 erased in exactly one catalog overwrite, bystander untouched.
- updated for the new request shape: `DeletesEligibleRowsFromReturnedResolutionCuts` (catalog GETs 3 independent of the row
  count, one overwrite), `ReturnsRetiredLifeWhenAuthorityMovesAfterResolution` (the resolution cut is now the third read),
  `RetriesFromTheMandatoryConflictResolutionCut` (5 GETs: selection, lane base, refused write's resolve read, retry's lane base,
  resolution cut).
- `DrainRaceBackend::write` now journals `cas_conflict <key>` for a refused conditional write.

## Fix B: reuse the decoded catalog when the bytes did not change

Patch: `patches/0002-cas-catalog-reuse-decoded-snapshot-for-identical-bytes.patch` (on top of 0001).

`CasRefCatalog::readShared` still GETs the object on every call (freshness unchanged) and returns a shared immutable snapshot;
when the bytes are byte-identical (and the etag equal) to the last bytes this process decoded for that key, it returns the cached
decode instead of decoding, validating, indexing and copying every entry again. Identical bytes decode to an identical snapshot,
so the verdict is exactly `read`'s. Used by the two admission re-reads in `CasRefLedger::commitRefChunk`, which were 67 % of
catalog-read CPU in the TSan profile (`phase3_repro.md`, experiment 3).

## Measurements, release, RustFS, GC on (`runs/s3_*`)

| build | wall s | GC rounds ok / aborted | catalog rows at end | ms per cycle, iterations 0-199 | 800-999 |
|---|---|---|---|---|---|
| before (`s3_relbase`, RustFS reused) | 578.6 | 1 / 5 | 7980 (all `removing`) | 319.5 | 804.7 |
| before (`s3_relbase_fresh`, fresh RustFS) | 569.4 | 1 / 5 | 7842 | 299.9 | 817.8 |
| fix A, first variant (`s3_relfixA`) | 447.6 | 7 / 0 | 579 | 306.1 | 495.4 |
| fix A+B, final (`s3_relfixAB_final`, fresh RustFS) | 449.7 | 6 / 0 | 1107 | 337.4 | 483.9 |
| plain S3 disk, same RustFS (`s3_relplain_fresh`) | 143.9 | -- | -- | 123.4 | 143.1 |

"First variant" is fix A before two refinements that touch only the fenced paths (the pre-lane admission check and reporting
lives the fenced `decide`'s base no longer catalogs); the final A+B row is the exact tree of `patches/0001` + `patches/0002`.

## Measurements, TSan, RustFS, GC on, 8 workers x 250 cycles

| binary | wall s | GC rounds ok / aborted | catalog at end | ms per cycle, 0-49 | 200-249 |
|---|---|---|---|---|---|
| before: CI artifact `build_amd_tsan/clickhouse` (`s3_tsan_base`) | 219.3 | 1 / 1 | 1985 | 477.5 | 1222.8 |
| A+B, built in `build_tsan` with identical `system.build_options` (`s3_tsan_fixAB`) | 163.4 | 3 / 0 | 980 | 440.2 | 736.6 |
| plain S3 disk, CI TSan binary (`s3_tsan_plain`) | 59.0 | -- | -- | 195.2 | 274.0 |

TSan build of the worktree: 34 min for the `clickhouse` target (`build_tsan/build_clickhouse.log`), after a 29 min release build.

## Unit tests

Full CAS gate `unit_tests_dbms --gtest_filter='CAS*'` on the final tree: 2538 passed, 0 failed, 2 disabled (not ours)
(`cas223-perf/build/test_cas_gate3.log`). Failing-first check: the two new tests run against the PR head's product code fail
(`cas223-perf/build/test_failing_first.log`: 8 catalog overwrites for 8 rows; "the drain lost 3 catalog races to its own server's
writers", 906 ms of backoff), and pass with the fix.
Also adapted, because they pinned the per-row protocol: `CASGCFrontierGate.CompletedRemovalDrainUsesNPlusOneCatalogReads` (now
`...UsesThreeCatalogReadsForAnyNumberOfRows`), `CASGCFrontierGate.ALeaderDeposedBetweenTwoErasesStopsAfterTheFirst` (now
`ALeaderDeposedAfterItsBatchEraseStopsBeforeTheFold`: the round throws, no further catalog write, no hot LIST), and
`StaleIssuedCatalogCasLosesAfterNewLeaderHelpsBeforeListing` (leader B now opens its own pool, a second server with its own lane;
sharing leader A's pool would queue B behind A's deliberately blocked write, a race two servers cannot have).
One real regression was caught and fixed on the way: `CASGCCompletedRemovalFenceRace` requires a stale leader that learns from a
durable read that a life is gone to retire it even though fenced; the first batch version did not, the final one reports those lives.

Before: the five aborted rounds each end after 76-81 s with `CAS ref catalog erase for namespace '...' ... could not be committed`
(`runs/s3_relbase/gc_rounds.tsv`), the CI release lane's message. With fix A every drain takes 12-54 ms
(`runs/s3_relfixA/gc_log_all.tsv`), the catalog oscillates between ~600 and ~1700 rows, and the last-bucket cycle time is 38 % lower.

Fix B, release, GC off, catalog grown with idle tables (`runs/exp_rel_base_cas_idle.out` vs `runs/exp_rel_fixAB_cas_idle.out`,
local object store, 8 workers x 100 cycles):

| catalog entries | before: ms per cycle | A+B: ms per cycle |
|---|---|---|
| 0 | 75.6 | 73.0 |
| 3800 | 154.2 | 125.1 |
| 7600 | 207.1 | 186.9 |

B removes 20-40 % of the catalog-proportional cost; the rest is in readers not switched to `readShared` and in cache misses
(every CREATE/DROP changes the bytes).

## Tried and rejected

- `patches/rejected-0003-janitor-full-sweep.patch`: janitor pages until its cursor wraps. Round 1 swept 114 pages / 113,269 keys in
  41.5 s and deleted nothing (lives become dead only after the next drain); it doubles the per-round O(all keys) walk.
- `patches/rejected-0003b-retired-lives-sync-delete.patch`: delete the drained lives' objects by prefix at the end of the round.
  It deletes the right keys (20,962-39,173 per round, `runs/s3_relfixABC/gc_log_all.tsv`) but with one exact DELETE per key
  inside the round it takes 39-70 s and delays the next drain: wall 553.2 s vs 447.6 s for A alone, catalog 3401 vs 579 at the end.
  The deletes must move off the round's critical path (a background worker, or bulk delete where exactness allows).

## What still grows with fix A

`defer_decision` (the global LIST of `cas/ns/stream/`) grows from 4.4 s to 45.4 s over 7 rounds in `s3_relfixA`, because the
ref objects of every dropped table stay listed until the one-page-per-round janitor reaches them (`namespace_cleanup` lists 1000
keys per round). This is the known dead-life-debris item (prior RCA §2.6, K1-K3). It lengthens rounds, which lets the catalog
oscillate higher; it is the next thing to fix.
