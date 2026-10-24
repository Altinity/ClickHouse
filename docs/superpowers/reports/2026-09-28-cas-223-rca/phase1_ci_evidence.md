# CAS-223 phase 1: CI evidence

Run: https://github.com/Altinity/ClickHouse/actions/runs/36347426173 (PR https://github.com/Altinity/ClickHouse/pull/2440,
head `58f8458c707`). All paths below are relative to `tmp/cas223/`.

## Sources

- Raw GitHub job logs (`gh api .../actions/jobs/<id>/logs`), escape codes stripped: `j<last 3 digits of job id>.txt`.
  The cancelled CAS jobs uploaded **no** Praktika artifacts (404 on every `stateless_tests_amd_{tsan,msan}_cas_s3_storage_*`
  path), so for them the job log is the only source.
- Per-test lines parsed by `parse.py` into `tests.tsv` (shard, wall timestamp, index, test, status, duration).
  Query helpers: `views.sql`, `gen.sh`, `cmp.sh`. Test classes: `test_class.tsv` (from `tests/queries/0_stateless`
  text: `mergetree` = source mentions MergeTree; `other_table` = creates a table but no MergeTree; `no_table`).
  Caveat: tests that only *read* pre-loaded MergeTree tables (TPC-DS, `test.hits`) land in `no_table`.
- Server logs that exist: `logs/asan_cas_1.log` (ASan CAS stateless 1/2), `logs/bin_cas.log` (release CAS stateless),
  `logs/stress_tsan_cas.log` (TSan CAS **stress** job, the only TSan CAS server log of this run), `logs/tsan_s3_1.log`.

## Lanes compared (all 16 CPUs / 30.6 GiB runners, `--jobs 8`, except MSan: `--jobs 6` CAS, `--jobs 4` on a 12-CPU WasmEdge runner)

| lane | default MergeTree disk | object store |
|---|---|---|
| `amd_tsan, s3 storage` | `cached_s3_encrypted` = encrypted over a 5 GiB filesystem cache over plain `s3` (local metadata) | MinIO :11111 |
| `amd_tsan, cas s3 storage` | `cas_s3` (CAS metadata, no cache, `cas_gc_interval_sec` 20) | RustFS 1.0.0-rc.3 :11121 |
| `amd_tsan, parallel` | local disk | none |
| `amd_binary, cas s3 storage` | `cas_s3` | RustFS |
| `amd_binary, cas storage` | CAS over `local` object storage | none |

Source: `tests/config/install.sh:371-393`, `tests/config/config.d/{cas_s3_storage_policy_for_merge_tree_by_default,s3_storage_policy_by_default,s3_encrypted_storage_policy_for_merge_tree_by_default}.xml`, job command lines in `j625.txt:2265`, `j646.txt`.
So "CAS vs S3" in CI also differs in object store (RustFS vs MinIO), read cache (none vs 5 GiB) and system-log placement
(system logs are on `cas_s3` in the CAS lane). The CAS lane also loads the stateful datasets (not `--no-stateful`).

## (a) Completed tests and the throughput curve

From `tests.tsv`:

| shard | tests done / total | test-phase hours | tests/h | median s | p90 s | sum of test time h |
|---|---|---|---|---|---|---|
| tsan s3 1/2 | 5636 / 5636 | 1.74 | 3239 | 2.87 | 18.4 | 13.3 |
| tsan s3 2/2 | 5653 / 5655 | 2.31 | 2447 | 3.28 | 22.1 | 17.9 |
| tsan local 1/2 | 5636 | 1.33 | 4238 | 2.13 | 14.2 | 10.5 |
| **tsan cas 1/2** | **5636 / 5636** | **5.58** | 1010 | 7.64 | 58.0 | 43.0 |
| **tsan cas 2/2** | 5344 / 5655 | 5.75 | 929 | 8.24 | 57.7 | 44.2 |
| asan cas 1/2 | 5636 | 1.64 | 3437 | 3.02 | 18.5 | 12.8 |
| asan cas 2/2 | 5653 | 2.11 | 2679 | 3.77 | 23.3 | 16.4 |
| release cas s3 (1 shard) | 11243 | 1.11 | 10129 | 1.11 | 5.9 | 8.4 |
| release cas local (1 shard) | 11243 | 0.70 | 16061 | 0.45 | 3.0 | 5.3 |
| msan cas 1/3, 2/3, 3/3 | 3076, 1592, 2872 of ~3800 | 5.8 | 534, 276, 496 | 10.6-18.5 | 91-117 | 21.7-32.8 |
| msan WasmEdge 1/4..4/4 | all | 1.1-3.7 | 776-2463 | 3.9-7.1 | 19-38 | 6.6-14.3 |

TSan CAS 1/2 **finished all 5636 tests** at 06:27 UTC (`j625.txt:14392`); the job was then cancelled during the
post-test random-settings diagnostics at 06:37. The overrun is the test phase: 5.58 h against 1.74 h.

Tests per 30 minutes (`timeline_30min.tsv`):

| shard | 30-min buckets |
|---|---|
| tsan s3 1/2 | 1786 1766 1395 689 |
| tsan cas 1/2 | 765 627 740 622 552 512 440 368 415 329 234 |
| tsan cas 2/2 | 742 663 444 353 576 515 384 426 316 314 410 201 |
| msan cas 1/3 | 700 438 354 253 267 184 133 170 184 187 162 |
| msan WasmEdge 1/4 | 726 476 413 361 307 308 245 |

Both effects exist: CAS is **2.3x slower from the first 30 minutes** (765 vs 1786) and it **degrades further** (to 234-410 per 30 min).

## (b) Per-test join, TSan CAS vs TSan S3 (10,444 tests OK in both)

- Sum of test time: CAS 74.5 h, S3 27.2 h. Median ratio 1.83, p10 0.96, p90 6.86, p99 17.8.
- Loss concentration (share of total extra time held by the top-N tests by absolute loss): top 10 = 4.8 %,
  top 40 = 13.6 %, top 100 = 24 %, top 400 = 48 %, top 1000 = 67 %. **Not a long tail: the loss is spread over thousands of tests.**
- Ratio histogram: 0.8-1.25x: 2999 tests; 1.25-2x: 2266; 2-3x: 1501; 3-5x: 1480; 5-10x: 1301; >=10x: 507 (`top40.txt`).
- Top 40 by absolute and by relative loss: `top40.txt`. The absolute list is dominated by tests that ran late in the
  CAS shard (column `cas_at_h` mostly 3-5.6 h): `00564_versioned_collapsing_merge_tree` 75.6 -> 916 s, `01518_nullable_aggregate_states2` 66 -> 856 s,
  `04300_cas_projection_multiblock` 62.6 -> 875 s.

**By test class** (median CAS/S3 ratio, `gen.sh 'tsan_cas%' 'tsan_s3%'`-style queries):

| class | n | median ratio | extra hours |
|---|---|---|---|
| `mergetree` | 4285 | **3.19** | 32.6 |
| `other_table` | 1786 | 1.99 | 5.8 |
| `no_table` | 4320 | **1.19** | 8.4 |

**By CAS elapsed hour**, median ratio: `no_table` 1.12, 1.15, 1.18, 1.23, 1.33, 1.24; `mergetree` **1.99, 2.88, 3.41, 4.10, 5.74, 4.88**.

**Against the release build on the same test** (ratio of each test's sanitizer duration to its `amd_binary, cas s3` duration,
per 500 tests completed, `ratio_by_index.tsv`):

| shard | `mergetree` tests | `no_table` tests |
|---|---|---|
| tsan local 1/2 | 1.5 ... 1.5 (flat) | 3.7 ... 3.5 |
| tsan s3 1/2 | 2.1 ... 2.6 (flat) | 3.3 ... 3.3 |
| **tsan cas 1/2** | **5.1 4.6 5.6 7.2 7.7 8.0 7.8 14.2 13.3 12.8 17.3 22.9** | 4.3 ... 4.5 (flat) |
| **msan cas 1/3** | **4.7 9.7 13.8 17.9 31.2 31.0 39.2** | 5.2 ... 7.6 |
| asan cas 1/2 | 2.0 2.3 2.3 2.5 2.5 3.5 3.4 3.1 3.3 3.1 3.1 2.7 | 3.8 ... 4.2 (flat) |

Reading: the sanitizer CPU multiplier on work that does not touch MergeTree is ~3.5-4.5x (TSan, ASan) and ~5-8x (MSan),
flat. MergeTree work on CAS starts near that multiplier and then **ramps 4x (TSan) and 7x (MSan) with the amount of work done**,
while ASan CAS ramps only 2.0 -> ~3.1 and the TSan plain-S3 and local lanes do not ramp at all.
ASan CAS MergeTree tests relative to release (2.0-3.5) are *below* the ASan CPU multiplier (3.8-4.2): under ASan
the CAS cost is dominated by sanitizer-independent latency (the object store), not CPU.

Release CAS over RustFS vs release CAS over local object storage: `mergetree` median 2.49x flat (2.25, 2.82, 2.82 per 30 min),
`no_table` 1.46x. This is the RustFS round-trip tax, present without any sanitizer and without a ramp.

## (c) Which tests use CAS

In the CAS lane the default MergeTree policy is `cas_s3`, so every MergeTree table without an explicit policy, every
system log table (the config deliberately keeps them there), and the stateful datasets loaded before the run are on the
one default CAS pool. 51 test files declare their own inline disk (`test_class.tsv`, column `inline_disk`; they also run in
the S3 lane and did *not* lose more than the rest: median ratio 1.71). Each inline CAS disk creates its own pool with its
own GC thread (the TSan stress log shows `BlobKillerThread` loggers for more than 12 distinct inline pools).

## (d) Server-log signatures

No server log exists for the TSan or MSan CAS stateless shards of this run (cancelled jobs upload nothing).
Counts in the available logs (`signatures.txt`; columns: ASan CAS stateless 1/2, release CAS stateless,
TSan CAS stress, TSan S3 stateless 1/2):

| signature | asan cas | release cas | tsan cas stress | tsan s3 |
|---|---|---|---|---|
| `CasWriteRetryLater` / retry later | 4 | 31 | **2294** | 8 |
| `Attempt k/N failed` (S3 retry) | 40 | 17 | 0 | 0 |
| `EADDRNOTAVAIL` / errno 99, `SlowDown` | 0 | 0 | 0 | 0 |
| `Waiting for table ... to be finally dropped` | 7697 | 17203 | 864 | 7717 |
| `Repointed committed ref` | 31096 | 56504 | 5941 | 1593 |
| HEAD-404 signer lines (`signature check failed`) | 142078 | 173836 | 73005 | 132 |

Per-part removal step (gap between consecutive `CachedPartFolderAccess: Repointed committed ref .../delete_tmp_*` lines of
one table on one thread, `repoint.py`), p50 / p90 per 30 min:

| log | 0-0.5 h | 0.5-1 h | 1-1.5 h | 1.5-2 h |
|---|---|---|---|---|
| release CAS stateless | 141 / 578 ms | 168 / 31732 ms | 177 / 634 ms | |
| ASan CAS stateless 1/2 | 191 / 852 ms | 564 / 3184 ms | 297 / 1444 ms | 356 / 1616 ms |
| TSan CAS stress | 573 / 1542 ms | 706 / 2189 ms | | |

Even the release build pays ~140 ms per removed part, serially. No connection or port exhaustion signature in any CAS log.

## (e) Metrics artifacts

None. No `metric_log`, `trace_log`, `cas_log`, `cas_gc_log` or thread census was exported by any CAS sanitizer job of this run;
the ASan and release CAS jobs uploaded only the server logs. GC round durations, catalog size and backlog cannot be read from CI.

## MSan

MSan CAS vs MSan WasmEdge (local disk) per test: `mergetree` median 4.72x, ramping 1.76 -> 10.1 per 30 min; `no_table` 1.34x
(1.04 -> 1.63). Same shape as TSan, steeper. The WasmEdge lane also degrades over time on its own (726 -> 245 tests per 30 min
on 1/4), so part of the MSan ramp is not CAS.

## Conclusions to test in phase 2

1. The loss is a **per-MergeTree-operation** cost on CAS; tests without tables lose ~1.2x (TSan).
2. It has a **constant part** (~2.5x on MergeTree tests, visible even in release as the RustFS round-trip tax) and a
   **growing part** that grows with work done and is steep only under TSan and MSan.
3. It is **not** thread count or sanitizer cost in general: ASan CAS with the same pools and threads runs as fast as TSan plain S3.
4. The CI "S3 vs CAS" comparison is confounded by RustFS vs MinIO and by the S3 lane's 5 GiB filesystem cache.
