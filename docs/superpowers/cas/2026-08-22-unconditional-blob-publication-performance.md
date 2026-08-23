---
description: 'Target-only S41 measurements and the blocked matched performance acceptance for unconditional CAS blob publication.'
sidebar_label: 'CAS blob publication performance'
sidebar_position: 91
slug: /superpowers/cas/unconditional-blob-publication-performance
title: 'CAS unconditional blob publication performance'
doc_type: 'reference'
---

# CAS unconditional blob publication performance {#cas-unconditional-blob-publication-performance}

## Decision {#decision}

Measurement status is `DONE_WITH_CONCERNS`; performance acceptance is **blocked**.

Three fresh-reset, full-scale target runs with the final `S41` card passed 135/135 verdicts and
12/12 strict CAS publication checks. Every fan-out task issued exactly one blob `HEAD`. A genuinely
fresh task published one body, made exactly one metadata-create attempt, recorded exactly one clean
create reason, and issued no metadata GET, metadata compare-and-swap, adopt-backfill, or resurrection
activity. An adopted task read metadata and avoided body publication.

There is still no valid same-environment pre-change run or binary. The values below characterize the
current target stack and its fixed first/second-leg sequence. They do not measure a code-version
latency delta, a before/after wide-insert slowdown or improvement, or the isolated cost of the
mandatory `HEAD`. The prior three target runs used the superseded metric/gating/cleanup semantics and
are retained only as superseded evidence; no number from them is blended into this report.

## Measurement contract {#measurement-contract}

| Property | Final target measurement |
|---|---|
| Evidence date | 2026-08-23 |
| Runner source | `6878d5363620e9f03fb1980f41920c151040588d` on `cas-gc-rebuild`, plus the final uncommitted `S41` fix diff; card SHA-256 `763caa2bded570147c98b52b7328b349e3ccf22fced4c34887b6343200bf5b18` in all three runs |
| Original Task 11 base | `02a67bbc27bec0c878600dc6fc879a40328cb829` |
| Tracked measurement-time dirt | The final `S41` card and the pre-existing 19-row user `RUN_HISTORY.md` diff; the history diff remained 19 additions/0 deletions with SHA-256 `7d391c61a758d3c1f791c31fffc3d228f6c6d15001d956f0857bc7509e571303` |
| ClickHouse binary | `26.6.2.20000.altinityantalya`, revision `54512`, build ID `9817EFC6DEEC0F0545E7E61313FBD2DB4B5592AF` |
| Binary source provenance | Built at `2f65aaa096783238a164c9d2fbeaf4e5157dad88`; the relevant production paths have no diff from that commit through the Task 11 base; `SANITIZE=OFF` |
| Host | AMD Ryzen 9 9950X, 16 cores/32 threads, 91.6 GiB RAM, Docker Engine `29.2.1` |
| ClickHouse node | One `ca-s41-ch1-1` container, 16 GiB limit, host HTTP port `18123` |
| Object store | One local RustFS service at `rustfs1:11121`; image `rustfs/rustfs:1.0.0-beta.12`, image ID `sha256:612a6707053c27c41816e79e5d5d30b5ba8479fb9b500ae4908cd4a723e888fa` |
| Stack | `utils/ca-soak/docker-compose-s41.yml` and `utils/ca-soak/configs/storage_conf_s41.xml`; `ca` and `s3plain` use distinct prefixes on the same endpoint |
| Small workload | 1 `UInt64` column, 1,000 deterministic rows, one forced-`Wide` part, 8,000 `system.query_log.written_bytes` |
| Wide workload | 30 deterministic mixed-type columns, 10,000,000 rows, 500 forced-`Wide` parts, 3,792,307,340 written bytes (3.531861 GiB) |
| Leg order | `small_plain_fresh`, `small_plain_duplicate`, `small_ca_fresh`, `small_ca_duplicate`, `wide_plain_fresh`, `wide_plain_duplicate`, `wide_ca_fresh`, `wide_ca_duplicate` |
| Warmth and isolation | A new compose volume for every repetition; the second leg repeats the same insert into the same table with `insert_deduplicate=0`; the order is fixed and therefore subject to carryover |
| Query controls | `max_threads=1`, `max_insert_threads=1`, one-part-aligned blocks, Real profiler at 5 ms and CPU profiler at 10 ms |
| Lifecycle controls | One outer restoration envelope covers GC stop, metrics-DB open, sampler construction/start, every measured leg, metric extraction, sampler stop, merge restart, GC restart, and DB close |
| Memory measurement | One-second container RSS sampler plus explicit boundary samples for every leg; the reported value is the maximum observed phase sample |
| Repetition/statistic | Three successful runs; median, unbiased sample variance `s²` (`n-1` denominator), and `CV = sample standard deviation / abs(mean)` |

No credential, access key, secret, authorization value, or credential-bearing configuration value is
included here.

## Target-only timing and memory {#target-only-timing-and-memory}

Each cell is `median / s² / CV`. Wall time is in seconds, query duration is in milliseconds, and
peak RSS is in GiB.

| Leg | Wall seconds | Query duration ms | Peak RSS GiB |
|---|---:|---:|---:|
| Small `s3plain` first leg | 0.038 / 0.000000333 / 1.53% | 37 / 0.333 / 1.57% | 0.460 / 0.001716 / 8.89% |
| Small `s3plain` second leg | 0.032 / 0.000007 / 8.53% | 31 / 7 / 8.82% | 0.542 / 0.001064 / 6.01% |
| Small `ca` fresh first leg | 0.109 / 0.000310 / 16.99% | 108 / 310.333 / 17.16% | 0.590 / 0.001114 / 5.76% |
| Small `ca` duplicate/adopt second leg | 0.077 / 0.000252 / 23.25% | 76 / 252.333 / 23.59% | 0.606 / 0.000152 / 2.05% |
| Wide `s3plain` first leg | 34.710 / 0.898010 / 2.77% | 34709 / 897433.333 / 2.77% | 1.418 / 0.000658 / 1.79% |
| Wide `s3plain` second leg | 32.240 / 5.624524 / 7.47% | 32239 / 5624524.333 / 7.47% | 1.675 / 0.011222 / 6.32% |
| Wide `ca` cold first leg | 145.427 / 3.180332 / 1.22% | 145426 / 3180332.333 / 1.22% | 1.648 / 0.020700 / 8.44% |
| Wide `ca` duplicate/adopt second leg | 50.948 / 0.038266 / 0.38% | 50947 / 38266.333 / 0.38% | 1.879 / 0.000459 / 1.13% |

The sub-second legs are shorter than the periodic RSS interval, so their RSS maxima are the maximum
of guaranteed boundary samples rather than a continuous high-water mark. Wide legs have interior
one-second samples. The small fresh `ca - s3plain` first-leg difference was 71 ms median,
`s²=325 ms²`, CV 27.31%, and range 46–81 ms. The corresponding current-policy ratio was 2.868421
median, `s²=0.248917`, CV 18.10%, and range 2.210526–3.189189. Both include every difference between
the storage policies and cannot be attributed solely to `HEAD`.

## Paired second-leg observations {#paired-second-leg-observations}

Ordinary warmth is material, so a raw second/first `ca` ratio is not interpreted alone. For each
repetition the report defines:

- `R_ca = ca_second / ca_first`;
- `R_control = s3plain_second / s3plain_first`;
- ratio of ratios `R_ca / R_control`;
- difference in differences `(ca_second - ca_first) - (s3plain_second - s3plain_first)`.

These are paired within-target sequence adjustments. They are **not** estimates of a code-version
before/after delta. A ratio below one or a negative difference means the target `ca` second leg fell
more than its paired plain-S3 control during this fixed-order sequence; it does not establish why.
Each cell is `median / s² / CV; range` across three repetitions.

| Wall-time observation | Small workload | Wide workload |
|---|---:|---:|
| Raw `ca` second/first ratio | 0.652542 / 0.003624 / 9.20%; 0.595238–0.715596 | 0.350334 / 0.000030724 / 1.59%; 0.342027–0.352538 |
| Plain-S3 control second/first ratio | 0.864865 / 0.005619 / 9.10%; 0.736842–0.868421 | 0.974018 / 0.006142 / 8.44%; 0.838285–0.974042 |
| Control-adjusted ratio of ratios | 0.807823 / 0.001323 / 4.57%; 0.754502–0.824020 | 0.359670 / 0.001432 / 10.04%; 0.351151–0.420547 |
| Control-adjusted difference in differences | -26 ms / 41.333 ms² / 22.43%; -36 to -24 ms | -93.578 s / 18.814881 s² / 4.68%; -96.516 to -87.978 s |

Query duration gives the same control-adjusted signal: the small ratio of ratios is 0.809013 median,
`s²=0.001355`, CV 4.62%, with a -26 ms median difference in differences; the wide ratio of ratios
is 0.359666 median, `s²=0.001432`, CV 10.03%, with a -93,578 ms median difference in differences.

The fixed order is not randomized or counterbalanced, the sample has only three repetitions, and
the small legs have high relative variance. The adjustment removes the observed paired plain-S3
second-leg effect algebraically, but cannot remove policy-specific cache state, interaction,
nonlinear carryover, or other unmeasured sequence effects. It therefore remains descriptive.

## Logical request normalization {#logical-request-normalization}

The card uses a canonical logical count map. Blob `HEAD` hit and miss counts are `CASBlobHead` and
`CASBlobHeadMiss`; their sum is the total. Successful logical body publication is
`CASBlobPut - CASMetaPut`. Avoided publication is `CASBlobBodyPutAvoided`. In this insert-only
workload `CASBlobGet` is the adjacent metadata GET because table data is never read. Metadata
attempt/reason counters are `CASMetaPut`, `CASMetaCompareSwap`, `CASMetaCreateClean`,
`CASMetaAdoptBackfill`, and `CASMetaResurrectClean`.

All three runs had identical logical counts and normalizations, so request-count variance is zero.
Every cell is `count / per part / per input GiB / per fan-out task`. The implementation emits `null`
for every affected rate when a denominator is zero; focused tests cover zero parts, zero input
bytes, and zero fan-out tasks.

| Logical class | Small fresh | Small duplicate/adopt | Wide cold first leg | Wide duplicate/adopt |
|---|---:|---:|---:|---:|
| `HEAD` hit | 0 / 0 / 0 / 0 | 2 / 2.000 / 268435.456 / 1.000000 | 20828 / 41.656 / 5897.174 / 0.518755 | 40150 / 80.300 / 11367.943 / 1.000000 |
| `HEAD` miss | 2 / 2.000 / 268435.456 / 1.000000 | 0 / 0 / 0 / 0 | 19322 / 38.644 / 5470.770 / 0.481245 | 0 / 0 / 0 / 0 |
| `HEAD` total | 2 / 2.000 / 268435.456 / 1.000000 | 2 / 2.000 / 268435.456 / 1.000000 | 40150 / 80.300 / 11367.943 / 1.000000 | 40150 / 80.300 / 11367.943 / 1.000000 |
| Body publication | 2 / 2.000 / 268435.456 / 1.000000 | 0 / 0 / 0 / 0 | 19322 / 38.644 / 5470.770 / 0.481245 | 0 / 0 / 0 / 0 |
| Body publication avoided | 0 / 0 / 0 / 0 | 2 / 2.000 / 268435.456 / 1.000000 | 20828 / 41.656 / 5897.174 / 0.518755 | 40150 / 80.300 / 11367.943 / 1.000000 |
| Metadata GET | 0 / 0 / 0 / 0 | 2 / 2.000 / 268435.456 / 1.000000 | 20828 / 41.656 / 5897.174 / 0.518755 | 40150 / 80.300 / 11367.943 / 1.000000 |
| Metadata create attempt | 2 / 2.000 / 268435.456 / 1.000000 | 0 / 0 / 0 / 0 | 19322 / 38.644 / 5470.770 / 0.481245 | 0 / 0 / 0 / 0 |
| Metadata compare-and-swap attempt | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| Clean-create reason | 2 / 2.000 / 268435.456 / 1.000000 | 0 / 0 / 0 / 0 | 19322 / 38.644 / 5470.770 / 0.481245 | 0 / 0 / 0 / 0 |
| Adopt-backfill reason | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| Resurrect-clean reason | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |

The wide first insert is cold but mixed because deterministic files are identical across partitions.
Its 19,322 misses each published one body and made one clean metadata create without a metadata GET.
Its 20,828 hits each read metadata and avoided publication. The pure-fresh small path proves the
common miss path independently. Duplicate paths published no bodies.

## Publication transport semantics {#publication-transport-semantics}

Source inventory found no query-attributed logical `ProfileEvent` that divides successful blob-body
publications into streaming and server-side-copy counts. `BlobUploadDiagnostics` and the emitted
`CasEvent` detail carry a transport value for individual successful publications, but they are not a
logical query counter available to `S41`. Conversely, `S3CopyObject` is incremented per physical
API attempt inside the retry loop, so it cannot partition successful logical publications.

Accordingly the report exposes the logical body-publication total, marks both logical transport
subcounts unavailable, and reports physical `S3CopyObject` attempts separately. Physical copy
attempts were zero in all four CA paths in all three full runs. A retry-shaped focused regression
with four logical publications and seven physical copy attempts proves that the logical total stays
four and is never mispartitioned by subtraction.

## Fail-close and restoration controls {#fail-close-and-restoration-controls}

The pure-fresh predicate now requires all of the following for `N` fan-out tasks:

- zero `HEAD` hits and exactly `N` misses and body publications;
- zero avoided bodies and metadata GETs;
- exactly `N` metadata-create attempts and clean-create reasons;
- zero metadata compare-and-swap attempts, adopt-backfill reasons, and resurrect-clean reasons.

The outer restoration envelope records every GC or table stop **before** issuing it. Even a failed
stop with an ambiguous remote outcome therefore triggers the matching start attempt. On every exit
it independently attempts sampler stop, restart for every table stop attempted, CAS GC restart, and
metrics-DB close. If measurement and cleanup both fail, the raised exception contains the primary
failure and every cleanup failure rather than masking or skipping later restoration.

Focused fault injection proves CAS GC restoration after metrics-DB open failure and sampler-start
failure, sampler/DB cleanup after partial setup, restoration after an ambiguous table-stop failure,
and aggregation while sampler stop, table restart, GC restart, and DB close all fail.

## Baseline provenance audit {#baseline-provenance-audit}

| Candidate | Provenance found | Classification |
|---|---|---|
| Historical `S41` history | Five rows dated 2026-07-23/24: one failed dev, two passed dev, and two passed full runs at source prefixes `29c98dcfd05c` and `a9449127f724`; all five referenced run directories are absent | Rejected: no retained raw metrics, different harness/order, no binary build ID, and no reproducible same-environment pair |
| Historical staging note | `26.6.1.20000.altinityantalya`, two replicas, 10M rows/30 columns/500 partitions, one CAS value 170.6 s and one plain-S3 value 22.4 s | Rejected: different topology and unrecorded endpoint, binary ID, exact schema/order/warmth, artifacts, repetitions, and variance |
| `build_asan/programs/clickhouse` | `26.6.1.20000.altinityantalya`, build ID `f8e1a56588513ff97bb0cf368ada5e029c9e66df`, `SANITIZE=address` | Rejected: unlike build mode and no matched run or source-to-binary proof |
| `tmp/docker-cas-nightly/clickhouse` | `26.6.1.1`, build ID `712fa3c6859c26b8a37b8cfb7a00bb5795e8a4e7` | Rejected: source/configuration provenance and matched environment are absent |
| Pre-change source boundary | `5147dc4e963969389a66ac446f8c8f39280bb1c7`, parent of `907c3b5ce7d83e8f8c5f3db798de9ee67f6ceb6c` (`Publish CAS blobs after mandatory HEAD`) | Not a measurement: no verified binary or retained run exists for this source point |

The pre-existing uncommitted `RUN_HISTORY.md` diff contains 19 rows for `S34`, `S35`, `S44`, and
`S45`, not `S41`. It was read for provenance and left unchanged. The measurement wrapper suppressed
only history/backlog appenders; it did not replace scenario SQL, sampling, metrics, or verdict logic.

## Acceptance and precise blocker {#acceptance-and-precise-blocker}

The final target request protocol agrees with the expected shape. It does not clear the performance
gate. No matched pre-change binary/artifact exists, so the following code-version deltas remain
unavailable:

- fresh small-blob latency cost;
- wide-insert slowdown or improvement;
- request delta;
- peak RSS delta.

The unmatched historical 170.6 s/22.4 s note, differing local binaries, the prior reviewed-metric
target runs, and the within-target second-leg adjustments are excluded from code-version acceptance.
Status remains `DONE_WITH_CONCERNS`, with performance acceptance blocked.

The required rerun must build verified pre-change
`5147dc4e963969389a66ac446f8c8f39280bb1c7` and accepted-target binaries with identical compiler,
flags, and sanitizer mode; record both build IDs; and run at least three fresh-reset pairs. It must
hold constant the host, Docker engine, node/container limit, RustFS image/endpoint, compose/config,
prefixes, schema/expressions, rows/columns/parts, leg order/warmth, insert settings, profiler periods,
RSS sampler, merge/GC controls, and checkpoint. Binary order must be balanced across pairs. Only the
resulting paired medians/variance plus an explicit decision can clear acceptance.

## Commands and retained artifacts {#commands-and-retained-artifacts}

All compose/scenario commands ran sequentially from `utils/ca-soak`. Before each repetition a fresh
volume and a new `logs_s41/ch1` directory were created. `tmp/task11_run_s41.py` invokes the ordinary
runner after replacing only the two tracked-history appenders with no-ops.

```bash
docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_fix_s41_full1_reset.log 2>&1
mkdir -p logs_s41/ch1
docker compose -f docker-compose-s41.yml up -d > ../../build/task11_fix_s41_full1_up.log 2>&1
PYTHONPATH=. CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1 CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1 python3 ../../tmp/task11_run_s41.py --scenario S41 --seed 1121 --duration 15m --scale full --no-reset > ../../build/task11_fix_s41_full1_run.log 2>&1
docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_fix_s41_full1_cleanup.log 2>&1
mv logs_s41/ch1 logs_s41/ch1_fix_full1

docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_fix_s41_full2_reset.log 2>&1
mkdir -p logs_s41/ch1
docker compose -f docker-compose-s41.yml up -d > ../../build/task11_fix_s41_full2_up.log 2>&1
PYTHONPATH=. CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1 CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1 python3 ../../tmp/task11_run_s41.py --scenario S41 --seed 1122 --duration 15m --scale full --no-reset > ../../build/task11_fix_s41_full2_run.log 2>&1
docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_fix_s41_full2_cleanup.log 2>&1
mv logs_s41/ch1 logs_s41/ch1_fix_full2

docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_fix_s41_full3_reset.log 2>&1
mkdir -p logs_s41/ch1
docker compose -f docker-compose-s41.yml up -d > ../../build/task11_fix_s41_full3_up.log 2>&1
PYTHONPATH=. CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1 CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1 python3 ../../tmp/task11_run_s41.py --scenario S41 --seed 1123 --duration 15m --scale full --no-reset > ../../build/task11_fix_s41_full3_run.log 2>&1
docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_fix_s41_full3_cleanup.log 2>&1
mv logs_s41/ch1 logs_s41/ch1_fix_full3
```

| Repetition | Seed | UTC interval | Result | Retained report and host log |
|---:|---:|---|---|---|
| 1 | 1121 | 01:51:48–02:02:16 | `PASS`, 45/45 | `utils/ca-soak/scenarios/runs/20260823T015147_S41_seed1121/report.json`; `utils/ca-soak/logs_s41/ch1_fix_full1` |
| 2 | 1122 | 02:03:02–02:13:36 | `PASS`, 45/45 | `utils/ca-soak/scenarios/runs/20260823T020301_S41_seed1122/report.json`; `utils/ca-soak/logs_s41/ch1_fix_full2` |
| 3 | 1123 | 02:14:28–02:24:34 | `PASS`, 45/45 | `utils/ca-soak/scenarios/runs/20260823T021427_S41_seed1123/report.json`; `utils/ca-soak/logs_s41/ch1_fix_full3` |

Aggregation used only those three paths:

```bash
python3 tmp/task11_s41_stats.py > build/task11_fix_s41_stats.log 2>&1
```

The superseded reports remain at `20260823T003855_S41_seed1111`,
`20260823T005013_S41_seed1112`, and `20260823T010125_S41_seed1113`. They are named in the
aggregation output under `superseded_reports_excluded` and contribute no value to any statistic.
