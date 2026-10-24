# CAS-223 phase 3: reproduction

All paths are relative to `tmp/cas223/`. Binaries are the exact CI artifacts of PR #2440 head `58f8458c707`:
`bin/clickhouse_tsan` (`build_amd_tsan/clickhouse`) and `bin/clickhouse_release` (`build_amd_release/clickhouse`),
downloaded from `https://altinity-build-artifacts.s3.amazonaws.com/PRs/2440/58f8458c707.../`. No local sanitizer build was needed.
Host: 32 logical CPUs. TSan runs use `TSAN_OPTIONS=memory_limit_mb=20363`, as in CI.

## Rig

- `rig/mkconf.sh <dir> <gc 0|1>`: server config = stock `programs/server/config.xml` + one CAS disk over `local` object storage
  (`cas_gc_interval_sec` 20 as in CI, GC on or off) + policy `cas`. System logs stay on the local default disk.
  The object store is local on purpose: it removes RustFS, so what remains is CAS's own CPU and synchronization cost.
- `rig/start.sh <binary> <dir> <port>`: starts the server.
- `rig/workload.py <http_port> <policy> <workers> <iterations> <out.tsv>`: each worker loops
  `CREATE TABLE ... MergeTree` / 3 x `INSERT` (3 parts) / `SELECT` / `DROP TABLE ... SYNC`, the shape of a stateless test,
  and records the time of each step.
- `rig/mkidle.py`: creates K idle tables on a chosen policy (to grow the catalog without other activity).
- `rig/exp_catalog.sh <binary> <dir> <port> <idle_policy> <iters> <K...>`: GC off; for each K, add idle tables, then run
  the workload (8 workers) and record `system.events` deltas.
- `rig/sampler.sh`: every 10 s, catalog object size / entry count / `removing` count (read from the local object file
  `cas_pool/ca/cas/ref_catalog`), server threads and CPU, CAS event counters.
- `rig/an.sh <tsv> <bucket>`: per-step mean latency per bucket of iterations.

## Experiment 1: the ramp reproduces with no object store and no sanitizer (release, GC on)

`runs/rel_cas_gc1/`, 8 workers x 1000 iterations, 172.8 s (`long_cas.out`).

| iterations | create ms | insert ms | drop ms | total ms |
|---|---|---|---|---|
| 0-99 | 19.2 | 19.7 | 54.1 | 95.4 |
| 400-499 | 49.7 | 28.0 | 104.9 | 185.4 |
| 900-999 | 64.3 | 36.3 | 144.3 | 247.3 |

The catalog grew to 7670 entries (1.3 MB), 7670 of them `removing` (`samples.tsv`). GC ran 5 rounds in 3.5 min:
0.5 s, 0.15 s, 4.4 s, 30 s, then an 81 s round `Aborted` in the `pre_fold_ref_drain` phase with
`CAS ref catalog erase for namespace '...': gave up at the policy deadline after zero attempt(s)`; 0 objects were deleted by any round
(`gc_rounds.tsv`). After the load stopped, the next round took 138 s. 210,438 files were left in the pool.

## Experiment 2: per-operation cost is linear in catalog entries (GC off, catalog grown with idle tables)

Same request counts per run in every row (release: `CASRequestAttempt` +125,6xx and `CASOtherGet` +23,200 per 800 iterations;
TSan: +37,68x per 240 iterations), so the added time is not added I/O.

Release, 8 workers x 100 iterations (`runs/rel_exp_cas_idle.out`, control `runs/rel_exp_local_idle.out`):

| idle tables | on CAS: catalog entries | CAS idle: total ms | local idle (control): total ms |
|---|---|---|---|
| 0 | 0 | 71.5 | 74.7 |
| 3000 | 3800 | 131.2 | 82.0 |
| 6000 | 7600 | 201.4 | 126.5 |

TSan, 8 workers x 30 iterations (`runs/tsan_exp_cas_idle.out`, control `runs/tsan_exp_local_idle.out`):

| idle tables | on CAS: catalog entries | CAS idle: create / drop / total ms | local idle (control): total ms |
|---|---|---|---|
| 0 | 0 | 47.2 / 195.2 / 339.7 | 329.3 |
| 1500 | 1740 | 222.9 / 548.3 / 985.3 | 414.2 |
| 3000 | 3480 | 370.3 / 842.0 / 1489.1 | 581.8 |

The difference to the control at equal table count is the catalog's own cost:
release +49 ms per iteration at 3800 entries, +75 ms at 7600 (about 12-16 ms per 1000 entries);
TSan +571 ms at 1740 entries, +907 ms at 3480 (about 260-330 ms per 1000 entries), **20x the release slope**.
Creating the idle tables shows the same: 1500 CAS tables in 25.7 s then 73.2 s under TSan, versus 2.5 s and 2.3 s on local disk.

## Experiment 3: where the CPU goes (TSan, 3000-entry catalog, `perf -g`)

`perf/tsan_w.data`, 79,400 samples over 20 s of workload; inclusive shares in `perf/tsan_w_children.txt`:

| function (inclusive) | share of all server samples |
|---|---|
| `CasRefCatalog::read` | **56.2 %** |
| `decodeRefCatalog` | 33.1 % |
| `CatalogLifeIndex::CatalogLifeIndex` | 10.4 % |
| `~Snapshot` + vector copy in `read` | 7.7 % + 6.9 % |
| `readJSONString` (inside decode) | 6.6 % |
| `__tsan::user_free` (all callers) | 15.3 % |

Callers of `CasRefCatalog::read` by sample count (`perf script` walk): `CasRefLedger::commitRefChunk` 67.1 %,
`namespaceStillLogicallyPresent` 9.8 %, `dropNamespaceImpl` 7.3 %, `resolveNamespaceLife` 6.2 %, `namespaceLife` 3.3 %,
`createNamespace` 3.2 %, `acquireReadableRefTableRuntime` 3.1 %.

`commitRefChunk` re-reads the whole catalog before every ref-log append to check this one namespace's row
(`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:3394`, `:3425`).
The decode is allocation-heavy (one `String` per field per entry), which TSan and MSan instrument at a much higher cost than
the arithmetic that dominates ordinary queries. That is why the slope is 20x release while the generic TSan factor is ~4x.

## Experiment 4: why the catalog grows, and why only sanitizers run away (TSan, GC on)

`runs/tsan_gc1/`, 8 workers x 300 iterations, 285.9 s.

| iterations | create ms | drop ms | total ms |
|---|---|---|---|
| 0-49 | 50.4 | 227.9 | 382.4 |
| 100-149 | 145.2 | 431.2 | 774.7 |
| 200-249 | 377.4 | 937.9 | 1670.3 |

Catalog 0 -> 1987 entries, all `removing`. **One** GC round completed in the whole run (21.4 s at 14:24:00).
Round 2's `pre_fold_ref_drain` phase ran from ~14:24:20 to 14:28:13 (232.7 s) and erased 413 rows only after the workload
ended at 14:28:06 (`gc_log_all.tsv`). Three `system.stack_trace` samples during the load show the GC thread inside
`CasRefCatalog::deleteCompletedRemovingAtSnapshot` sleeping in `op.pause` after a refused conditional write.
Counters over 20 s of load: foreground catalog writes through the hot-key lane (`CASHotKeyCacheStarts`) +215, i.e. ~11/s;
catalog conflicts (`CASOtherCompareSwapConflict`) +6.

**Mechanism.** Every catalog mutation of the server (`createNamespace`, `completeCreation`, `beginRemoving`, ...) goes
through `casUpdateImpl`, i.e. the pool's per-key FIFO hot-key lane (`Backend/CasHotKeys.h:21-38`), which exists precisely
because "inside one server every writer of a shared key used to race every other". The GC drain's erase
(`deleteCompletedRemovingAtSnapshot`, `Pool/CasRefCatalog.cpp:452-562`) does not: it takes a snapshot, re-encodes the
whole catalog, and calls `op.replace` conditional on the snapshot's etag. With the lane committing ~11 writes/s, any erase whose
read-to-write window exceeds ~100 ms conflicts, backs off (`Retry::backoff`, up to seconds), re-reads and conflicts again.
The window is O(catalog) CPU: short in release (the erase sometimes wins, rounds still slow), ~20x longer under TSan
(it effectively never wins while tests run). The drain is mandatory for the round (`CasGc.cpp:473-481`), so the whole GC
round -- fold, deletes, janitor -- stalls with it. Rows are erased one per full rewrite, so even an uncontended drain is
O(backlog x catalog). Positive feedback: a slow drain -> more `removing` rows -> a larger catalog -> slower foreground ops and
a longer erase window -> the drain loses more often.

## The same signature in CI

- Release CAS stateless lane (`logs/bin_cas.log`, `ci_release_default_pool_gc_rounds.txt`): the default pool completed 12 GC
  rounds in 67 min; rounds 11 and 12 took 9 and 27 min; `candidates` rose 136 -> 34,759; **10 rounds failed with
  `CAS ref catalog erase ... gave up at the policy deadline after zero attempt(s)`**, the same message as experiment 1.
- ASan CAS stateless 1/2 (`ci_asan1_default_pool_gc_rounds.txt`): 43 rounds in 1.6 h, round length 22 s -> ~10 min, one drain
  failure, `candidates` up to 19,008, `deleted` flat at 4975-4976 per round (a per-round work budget, noted, not analysed).
- TSan and MSan CAS stateless: no server log exists (cancelled jobs), so the CI side of the TSan claim rests on the per-test
  ramp of phase 1 (MergeTree tests 5x -> 23x release, tests without tables flat), which is what experiments 2-4 produce.

## What each hypothesis came to

| hypothesis | verdict | proof |
|---|---|---|
| H1 O(catalog) per op | **confirmed** | exp. 2 (linear, same request count, control flat), exp. 3 (56 % of CPU in `CasRefCatalog::read`) |
| H2 GC falls behind | **confirmed, mechanism identified** | exp. 4 (drain starved by the hot-key lane; 1 round in 286 s), CI release log (10 drain failures) |
| H3 RustFS latency | constant part, not measured locally | CI: release CAS-RustFS vs CAS-local 2.5x on MergeTree tests; not needed to produce the ramp (exp. 1 has no RustFS) |
| H4 thread count | refuted | ramp reproduced with one pool and a constant ~750 threads (`samples.tsv` column `threads`); ASan CAS fast in CI |
| H5 request count | refuted for the ramp | request counts per iteration identical across catalog sizes (exp. 2) |
| H6 lock contention | part of H2 | the contention is the drain's optimistic write against the hot-key lane, not a mutex |
| H7 GC CPU competition | refuted | tests without tables flat in CI; the stalled GC thread sleeps, it does not burn CPU, during load |
| H8 port exhaustion | refuted | no signature, and the ramp reproduces with no network |
| H9 MemoryWorker cascade | not observed | no `241` in any run |
| H10 harness | confound on level only | random query settings off in the TSan S3 lane only |
