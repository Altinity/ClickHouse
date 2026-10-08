# Value proof: janitor batch deletes and the manifest-body memo

Branch `cas/gc-request-cuts`, code worktree `/home/mfilimonov/workspace/ClickHouse/gc-request-cuts` at `1edf28f2a9a`
(verified clean, `git status --short` empty, `git rev-parse HEAD` = `1edf28f2a9a...`; the rebuilt `build/programs/clickhouse`
has the same sha256 as `build/clickhouse-after-1edf28f2a9a`: `e39d474a...`). Nothing committed or pushed. Stack down.
BEFORE = `build/clickhouse-before-61d24118251` (sha256 `eecb2cf3...4710`), AFTER = `build/clickhouse-after-1edf28f2a9a`.
All data and scripts: `master/tmp/gc-cuts-stand/value/`.

## Measurement 1: dropping a table with a long ref stream

### Method (MEASURED)

- Stack: ca-soak compose, only `ch1` + RustFS + Keeper (binary swapped by `gen_override.py`), `cas_gc_interval_sec` 36000 so GC does not run during seeding (`value/gc-off.xml`).
- Table `t` (`MergeTree`, 100 partitions, policy `ca`), `SYSTEM STOP MERGES`, N inserts of 200 rows (200 parts, about 200 `_log` keys each) on 4 threads until the stream holds 7,000 or 20,000 `_log` keys (bucket listing, `value/m1.py`).
- `DROP TABLE t SYNC`, then `SYSTEM CAS GC RUN ca` every 10 s (the default interval) until no `_log`/`_snap` key remains under `cas/ns/`. Per round: bucket listing by kind, `system.events` deltas (process-wide, the server is idle apart from GC), the `namespace_cleanup` phase row of `system.cas_gc_log`.
- Latency: `tc netem delay 20ms` on the RustFS container's `eth0` (`value/netem.sh`), armed after seeding, before the DROP. A TCP connect plus first byte took 61 ms.
- Runs: `a` = AFTER, `b` = BEFORE. 7k: 2 runs per binary. 20k: `b20000-2` and `a20000-1` are complete (129 rounds, `a20000-2` is a second complete AFTER run); `b20000-1` was cut short by the driver's old stop rule (80 rounds, 10,000 of 15,9xx janitor keys) and is used only for its DELETE count. 20 ms: 1 run per binary, 7k.
- Dead-life keys at DROP (after the DROP finishes): 7k runs `_log` 7,312..7,330, `_snap` 9..28, `_ckpt` 1; 20k runs `_log` 20,876..20,890, `_snap` 31..73, `_ckpt` 1. `_files` is 1 before the DROP and gone after. (The extra ~300/~880 `_log` keys are the removal logs the DROP writes.)

### Finding: the janitor is not the first cleaner and not the wall-time limit

1. Round 1 after the DROP: the fold's own ref cleanup deletes about 5,000 dead-life `_log` keys in batches (BEFORE already batches this: 16 `DiskS3DeleteObjects` at 7k, 35 at 20k). The janitor deletes 0 in that round (`janitor_deleted` 0, the life is still resolvable).
2. From round 2 the janitor removes what is left, **one page of 1,000 keys per authoritative round** (page budget 1000, hard-coded at the `NamespaceJanitor` construction in `Gc::runNamespaceJanitorPage`), in both binaries.
3. Authoritative rounds are not every round. Observed in all runs: rounds 2 and 3 back to back, then one every 9th round (gaps `[1, 9, 9, ...]` between janitor-active rounds); the rounds between are `Deferred` (`defer_decision.deferred` 1, `rounds_deferred_before` counting 5..8). I did not read the code for the defer policy; this is the observed cadence. So **wall time to drain = pages x 9 rounds x 10 s, in both binaries; batching does not change it.**

### Tables

Per run (MEASURED, `m1-table.py`). Janitor phase = `namespace_cleanup` phase row of the rounds in which `janitor_deleted` > 0; requests are `DiskS3DeleteObjects` in those phase rows. HEAD in the janitor phase: 0 in both binaries (the LIST carries the etag). Process-wide HEAD over the drain is 200 in every run, identical before and after, so it is not the janitor.

| run | rounds to drain | janitor-active rounds | keys the janitor deleted | janitor DELETE requests | janitor phase time, summed | drain wall (DROP+ to empty) | process DELETE requests over the drain | process GETs over the drain |
|---|---|---|---|---|---|---|---|---|
| b7000-1 | 12 | 3 | 2,333 | 2,333 | 2,551 ms | 112.2 s | 2,551 | 33,835 |
| b7000-2 | 12 | 3 | 2,332 | 2,332 | 2,374 ms | 112.2 s | 2,550 | 33,827 |
| a7000-1 | 12 | 3 | 2,335 | 3 (batch) | 1,218 ms | 112.0 s | 221 | 23,339 |
| a7000-2 | 12 | 3 | 2,325 | 3 (batch) | 1,001 ms | 111.7 s | 221 | 23,335 |
| b20000-2 | 129 | 16 | 15,913 | 15,913 | 16,798 ms | 1,307.0 s | 16,163 | 88,052 |
| a20000-1 | 129 | 16 | 15,949 | 16 (batch) | 7,102 ms | 1,307.7 s | 266 | 58,040 |
| a20000-2 | 129 | 16 | 15,944 | 16 (batch) | 9,375 ms | 1,307.6 s | 266 | 58,073 |
| b7000-lat (20 ms) | 12 | 3 | 2,357 | 2,357 | 60,808 ms (24.5 s + 23.9 s + 8.7 s at 1000/1000/357 keys) | 292.1 s | 2,575 | 33,922 |
| a7000-lat (20 ms) | 12 | 3 | 2,356 | 3 (batch) | 5,331 ms (2.3 + 1.9 + 1.2 s) | 235.4 s | 221 | 23,526 |

Round wall time of a janitor-active round (`SYSTEM CAS GC RUN` duration, MEASURED): 7k no latency 1.2..1.5 s before vs 0.8..0.9 s after (rounds 2-3); 20 ms latency 26..28 s before vs 4.2..4.7 s after; 20k 3.4..4.4 s at the start of the drain, 0.6 s at the end (after).

Time per 1,000 janitor keys (phase row): no latency 1.0..1.1 s before, 0.4..0.6 s after; 20 ms latency 24 s before, 1.9..2.3 s after.

### Verdict

- Requests: the janitor's DELETE requests fall from one per key to one per 1,000-key page: 2,333 to 3 (7k), 15,913 to 16 (20k). The process-wide DELETE count over the drain falls 2,551 to 221 (7k) and 16,163 to 266 (20k). The saving is exactly the number of keys the janitor deletes, which is stream length minus the ~5,000 the fold cleaned in round 1 (7k: 2.3k; 20k: 15.9k). It scales linearly with stream length.
- Time, no latency: the janitor phase halves (2.4..2.6 s to 1.0..1.2 s at 7k; 16.8 s to 7.1..9.4 s at 20k). About 0.4..0.6 s per 1,000 keys remains after batching (not analysed; per-key event or catalog work is my guess, unverified). The phase is 10..15% of the drain's total round time in either binary.
- Time, 20 ms: the janitor phase falls 60.8 s to 5.3 s and a janitor-active round from 26..28 s to 4.2..4.7 s (about 12x, per-key serial latency vs one request). At 20k, DELETE latency would scale the before figure to about 24 s x 16 pages = 6.4 min of pure janitor time (DERIVED, not measured).
- **Is the janitor budget-capped?** Yes, in both binaries: one 1,000-key page per authoritative round, and authoritative rounds arrive about every 9th round after the first two. Wall time is rounds x interval and is **identical**: 112 s vs 112 s at 7k, 1,307 s vs 1,307 s at 20k; with 20 ms it is 292 s vs 235 s, but that difference comes from round 1 (141.6 s vs 128.9 s, fold work outside the janitor) and the 10-s pacing, not from the janitor. Batching removes requests and per-round latency; it does not shorten the drain. Making the drain faster needs more pages per authoritative round, which is a different change.
- Value judgement input: the change is proven for request count (linear in the janitor's keys, 100x) and for round duration under object-store latency (12x at 20 ms). It has no effect on time-to-drain. With no latency, the saving is about 0.5 s per 1,000 keys per authoritative round.

### Limits

- One node, single table, RustFS on the same host, no concurrent user load; keys per stream is the only scaled dimension. Real dropped tables whose GC ran normally have few dead-life keys (the fold cleans them); the long stream requires GC to be behind, as constructed here.
- 20 ms run: n = 1 per side, 7k only. No-latency 7k: n = 2 per side; 20k AFTER n = 2, BEFORE complete n = 1 (`b20000-2`); 20k run-to-run spread on the janitor phase is large (7.1 s vs 9.4 s AFTER), so its ratio is 1.8..2.4x.
- `b20000-1` is partial (stop rule of the driver at that time: nine unchanged rounds). It is not in the tables except in this note.
- The DROP itself (2.3..2.5 s at 7k, 6.3..6.8 s at 20k, 33 s with latency) and round 1 (9..10 s at 7k, 23 s at 20k, 130..140 s with latency) are unchanged by the branch except for GET counts; the AFTER process GET count is lower by 10,500 (7k) and 30,000 (20k) over the drain, which is the memo hitting the manifest bodies deleted in round 1.
- The reason for the every-9th-round cadence was not root-caused; it was read from `cas_gc_log` (`deferred`, `rounds_deferred_before`).

## Measurement 2: reuse distance of manifest takes

### Method

- Throwaway instrumentation (applied by `value/apply_trace.py`, built as `build/clickhouse-memotrace`, then `git checkout` and rebuild; the tree is clean at `1edf28f2a9a`): in `Gc::foldManifestEdges`, when the fold's memo is present, one TSV line per consumed manifest edge: fold counter (incremented at the start of `Gc::fold`), `std::hash` of the manifest key, edge sign, number of blob entries in the body, sum of entry path bytes, and `hit` / `read` / `absent`. Written to `/var/log/clickhouse-server/memotrace.tsv`.
- Runs: S11 `--scale ci` seed 20260930 with the memotrace binary, default 10-s rounds (`runs/trace-default`) and `GC_EXTRA_CONFIG=longround/gc-long-interval.xml` (`runs/trace-long`, 300-s rounds). Only `ch1` was the GC leader in both runs (`ch2` trace is empty). Both runs PASS at RUN_RC 0.
- Simulation: `value/sim.py` (kept). Per fold, the cache starts empty (as the branch's memo does). Reuse distance = number of distinct manifests taken between two takes of the same manifest inside one fold (Fenwick tree). LRU of K manifests: hit when the manifest is in the K most recently used; `absent` takes are never cached. Outputs in `value/sim-default.md`, `value/sim-long.md`.

### Validation of the trace (MEASURED)

- Default: for the 25 rounds with edges in `cas_gc_log`, the per-round (edges, `CASRefManifestBodyMemoHits`) equals the per-fold (takes, `hit` lines) exactly in 25 of 25 rounds. The 26th fold is a round after the log snapshot. Totals: trace 75,584 edges / 25,402 hits vs `cas_gc_log` at snapshot 72,794 / 24,286.
- Long: trace 134,648 edges / 49,508 hits / 85,140 reads vs `system.events` 134,730 / 49,590 / 85,140 (`CASRefManifestBodyFoldGets`). The 82 missing lines are the unflushed tail of the trace's 4 KiB stdio buffer when the file was read before teardown (reads match exactly). Effect on every figure below: at most 0.06%.
- In both runs the branch memo never evicted: hits recorded in the trace = unbounded per-fold simulated hits (25,402 = 25,402; 49,508 = 49,508). So the simulated "unbounded" line IS the branch memo.

### Results (MEASURED from the trace, simulated offline)

Hit rate = hits / consumed manifest edges. "Share" = hits / hits of the branch memo. Entries/bytes = the most an LRU of K held at any moment (blob entries; sum of entry path bytes).

| K | default: hits | rate | share | long: hits | rate | share | peak entries held | peak path bytes held |
|---|---|---|---|---|---|---|---|---|
| 1 | 2 | 0.000 | 0.000 | 3 | 0.000 | 0.000 | 10 | 138 |
| 2 | 445 | 0.006 | 0.018 | 4,239 | 0.031 | 0.086 | 20 | 276 |
| 4 | 3,456 | 0.046 | 0.136 | 8,566 | 0.064 | 0.173 | 40 | 552 |
| 8 | 7,215 | 0.095 | 0.284 | 11,283 | 0.084 | 0.228 | 80 | 1,104 |
| 16 | 9,966 | 0.132 | 0.392 | 12,190 | 0.091 | 0.246 | 160 | 2,208 |
| 32 | 11,371 | 0.150 | 0.448 | 12,282 | 0.091 | 0.248 | 320 | 4,416 |
| 64 | 11,459 | 0.152 | 0.451 | 12,411 | 0.092 | 0.251 | 640 | 8,832 |
| 128 | 11,831 | 0.157 | 0.466 | 13,157 | 0.098 | 0.266 | 1,274 | 17,532 |
| 256 | 18,657 | 0.247 | 0.734 | 31,097 | 0.231 | 0.628 | 2,348 | 30,664 |
| 288 | 18,998 | 0.251 | 0.748 | 32,181 | 0.239 | 0.650 | 2,604 | 33,672 |
| 320 | 19,732 | 0.261 | 0.777 | 34,122 | 0.253 | 0.689 | 2,860 | 36,680 |
| 352 | 21,537 | 0.285 | 0.848 | 38,383 | 0.285 | 0.775 | 3,116 | 39,688 |
| **384** | 25,211 | 0.334 | **0.992** | 48,110 | 0.357 | **0.972** | 3,372 | 42,696 |
| 512 | 25,385 | 0.336 | 0.999 | 48,845 | 0.363 | 0.987 | 4,396 | 54,728 |
| 768 | 25,387 | 0.336 | 0.999 | 48,845 | 0.363 | 0.987 | 6,444 | 78,792 |
| 1024 | 25,402 | 0.336 | 1.000 | 48,845 | 0.363 | 0.987 | 8,492 | 102,856 |
| 4096 | 25,402 | 0.336 | 1.000 | 49,104 | 0.365 | 0.992 | 25,856 / 33,100 | 303,808 / 392,328 |
| branch memo | 25,402 | 0.336 | 1.000 | 49,508 | 0.368 | 1.000 | | |

Reuse distance (distinct manifests between two takes, hits only): default n = 25,402: p50 181, p90 367, p99 383, max 1,008. Long n = 49,508: p50 230, p90 376, p99 3,221, max 10,911.

Entries per manifest (blob placements, 50,182 / 85,140 distinct takes): p50 8, p99 8..10, max 10. Worst case held by an LRU of K = K x max: K = 384 gives 3,840 entries (measured peak 3,372 entries, 42.7 KB of path bytes).

Per fold (folds with >= 100 branch-memo hits): smallest K reaching 90% of that fold's hits, default (25 folds) `{8: 13, 16: 2, 32: 4, 288: 1, 384: 5}`; long (10 folds) `{16: 1, 256: 1, 384: 8}`. The small-K folds are small rounds.

### Verdict

- Smallest K with >= 90% of the branch memo's hits: **384 in both configurations** (99.2% default, 97.2% long). K = 352 gives 84.8% / 77.5%, K = 256 gives 73.4% / 62.8%. The hit curve is a cliff between 256 and 384 because the reuse distance is bunched at 180..380 (p90 367).
- Worst case held at K = 384 on this workload: 3,840 entries (measured 3,372 entries, 42.7 KB path bytes), because manifests here have at most 10 blob entries.
- So a fixed cache of the last ~400 bodies would keep the benefit on S11: the reuse is not "a few manifests earlier" (p50 181), but it is bounded (p90 < 400) and the memory is small. In the long-round configuration 1.3% of hits sit beyond K = 4,096 (p99 3,221, max 10,911), which a 384-cache loses (2.8% of hits).
- The number of entries is what bounds memory. This workload has manifests of <= 10 entries; a real wide part can have thousands. A cache limited by manifest count alone has no memory bound in that case (K x entries of the largest manifest), so it needs an entry or byte cap, which is a form of accounting. The trace cannot show what K real workloads need: the reuse distance comes from S11's structure (add then owner removal of the same manifest inside one fold).

### Limits

- S11 `--scale ci` only, one seed, one GC leader (`ch1`), one run per configuration. The K = 384 cliff is a property of this workload's shape, not a constant; a workload with more parts per fold moves it.
- "Entries" are blob-placement entries (what the memo stores), not body bytes. Peak path bytes count only entry paths, not `BlobRef` or `source_id` (about 32 more bytes per entry, not measured).
- The trace of the long run misses its last 82 lines (unflushed buffer); the default run's trace was read after teardown and is complete.
- The simulation ignores the branch memo's byte budget (it never triggered, see validation) and treats every K-LRU as per fold.

## Not done / disclosure

- No second 20 ms run, no 20 ms run at 20k.
- The first 20 ms attempt (`b7000-lat`/`a7000-lat` in `m1-seq.out`) failed in `netem.sh` (the health probe used `curl`, absent in the image); fixed and rerun; the failed runs left no data.
- A file `memotrace.tsv` (root-owned, in `utils/ca-soak/logs/ch1`, untracked and ignored) was created by each traced run; both were removed with a helper container. `git status` of `utils/ca-soak` equals its state before the work.
- No subagents used.
