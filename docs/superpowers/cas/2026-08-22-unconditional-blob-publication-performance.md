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

Three fresh-reset, full-scale target runs passed every `S41` verdict. Their request shape agrees with
the target protocol: every fan-out task issued exactly one blob `HEAD`; a genuinely fresh blob
published one body and created metadata without a metadata GET; an adopted blob read metadata and
did not publish a body. No common-path pre-publication metadata GET was observed.

There is no valid same-environment pre-change run or binary, however. Consequently this evidence
does not measure the latency delta caused by the mandatory `HEAD`, nor a before/after wide-insert
slowdown or speedup. The same-run `ca` versus `s3plain` values below characterize only the current
target stack. They are not a substitute for a matched code-version comparison and are not used to
accept the change.

## Measurement contract {#measurement-contract}

| Property | Target measurement |
|---|---|
| Evidence date | 2026-08-23 |
| Source base recorded by every run | `02a67bbc27bec0c878600dc6fc879a40328cb829` on `cas-gc-rebuild`; tracked dirt was the Task 11 card plus the pre-existing 19-row user history diff; unrelated untracked workspace debris was out of scope |
| ClickHouse binary | `26.6.2.20000.altinityantalya`, revision `54512`, build ID `9817EFC6DEEC0F0545E7E61313FBD2DB4B5592AF` |
| Binary source provenance | Built at `2f65aaa096783238a164c9d2fbeaf4e5157dad88`; the relevant production paths have no diff from that commit through `02a67bbc27bec0c878600dc6fc879a40328cb829`; `SANITIZE=OFF` |
| Host | AMD Ryzen 9 9950X, 16 cores/32 threads, 91.6 GiB RAM, Docker Engine `29.2.1` |
| ClickHouse node | One `ca-s41-ch1-1` container, 16 GiB limit, host HTTP port `18123` |
| Object store | One local RustFS service at `rustfs1:11121`; image `rustfs/rustfs:1.0.0-beta.12`, image ID `sha256:612a6707053c27c41816e79e5d5d30b5ba8479fb9b500ae4908cd4a723e888fa` |
| Stack | `utils/ca-soak/docker-compose-s41.yml` and `utils/ca-soak/configs/storage_conf_s41.xml`; the `ca` and `s3plain` policies use distinct prefixes on the same endpoint |
| Small workload | 1 `UInt64` column, 1,000 deterministic rows, one forced-`Wide` part, 8,000 `system.query_log.written_bytes` |
| Wide workload | 30 deterministic mixed-type columns, 10,000,000 rows, 500 forced-`Wide` parts, 3,792,307,340 written bytes (3.531861 GiB) |
| Leg order | `small_plain_fresh`, `small_plain_duplicate`, `small_ca_fresh`, `small_ca_duplicate`, `wide_plain_fresh`, `wide_plain_duplicate`, `wide_ca_fresh`, `wide_ca_duplicate` |
| Warmth and isolation | A new compose volume for every repetition; the duplicate leg repeats the same insert into the same table with `insert_deduplicate=0`; merges and background CAS GC are stopped across all measured legs |
| Query controls | `max_threads=1`, `max_insert_threads=1`, one-part-aligned blocks, Real profiler at 5 ms and CPU profiler at 10 ms |
| Memory measurement | One-second container RSS sampler plus explicit before/after samples for every leg; the reported value is the maximum observed phase sample |
| Repetition/statistic | Three successful runs; median, unbiased sample variance `s²` (`n-1` denominator), and coefficient of variation `CV = sample standard deviation / mean` |

No credential, access key, secret, authorization value, or credential-bearing configuration value is
included here.

## Target-only timing and memory {#target-only-timing-and-memory}

Each cell is `median / s² / CV`. Wall time is in seconds, query duration is in milliseconds, and
peak RSS is in GiB.

| Leg | Wall seconds | Query duration ms | Peak RSS GiB |
|---|---:|---:|---:|
| Small `s3plain` fresh | 0.040 / 0.000007 / 6.78% | 39 / 7 / 6.96% | 0.440 / 0.000324 / 4.05% |
| Small `s3plain` repeat | 0.033 / 0.000002 / 4.68% | 32 / 2.333 / 4.82% | 0.464 / 0.003886 / 12.79% |
| Small `ca` fresh | 0.127 / 0.000044 / 5.38% | 126 / 44.333 / 5.43% | 0.585 / 0.000409 / 3.49% |
| Small `ca` duplicate/adopt | 0.079 / 0.000001 / 1.45% | 78 / 1.333 / 1.47% | 0.630 / 0.000322 / 2.89% |
| Wide `s3plain` fresh | 33.118 / 7.693006 / 8.05% | 33117 / 7693006.333 / 8.05% | 1.413 / 0.011481 / 7.38% |
| Wide `s3plain` repeat | 32.538 / 0.416227 / 1.97% | 32537 / 416227 / 1.97% | 1.657 / 0.000276 / 1.00% |
| Wide `ca` cold first insert | 146.415 / 1.425577 / 0.82% | 146414 / 1425577 / 0.82% | 1.605 / 0.013102 / 6.92% |
| Wide `ca` duplicate/adopt | 50.809 / 0.381089 / 1.21% | 50808 / 381089.333 / 1.21% | 1.861 / 0.000625 / 1.34% |

The sub-second small legs are shorter than the periodic RSS interval, so their RSS maxima are the
maximum of the guaranteed boundary samples rather than a continuous high-water mark. Wide legs have
interior one-second samples.

The paired current-stack statistics use the same `median / s² / CV` convention; the range column is
the minimum and maximum across the three repetitions.

| Paired target-only observation | Median / `s²` / CV | Range |
|---|---:|---:|
| Small fresh `ca - s3plain` wall time | 86.0 ms / 17.333 ms² / 4.92% | 80.0–88.0 ms |
| Small fresh `ca / s3plain` wall ratio | 3.200 / 0.004421 / 2.10% | 3.098–3.222 |
| Small duplicate/adopt over fresh `ca` wall ratio | 0.632812 / 0.000987 / 4.87% | 0.622047–0.681034 |
| Small duplicate/adopt wall-time benefit | 36.719% / 9.867854 percentage-points² / 8.86% | 31.897–37.795% |
| Wide cold `ca / s3plain` wall ratio | 4.421010 / 0.130618 / 8.48% | 3.847466–4.515208 |
| Wide duplicate/adopt over cold `ca` wall ratio | 0.347020 / 0.000050 / 2.03% | 0.343641–0.357270 |
| Wide duplicate/adopt wall-time benefit | 65.298% / 0.503716 percentage-points² / 1.09% | 64.273–65.636% |

The current fresh small `ca` leg therefore cost a median 86 ms more wall time than its same-run
`s3plain` control. That value includes the complete difference between the two storage policies and
must not be attributed solely to the two mandatory blob `HEAD` requests. The current wide
duplicate/adopt leg was a median 65.298% faster than the current first wide `ca` insert. Both are
target-path observations, not before/after results.

## Target request shape {#target-request-shape}

The card reports non-overlapping logical classes. Blob `HEAD` is `CASBlobHead` hits plus
`CASBlobHeadMiss` misses. A successful blob body publication is `CASBlobPut - CASMetaPut`; a physical
`S3CopyObject` splits a staged copy from a streaming PUT. In this insert-only workload `CASBlobGet`
is the adjacent metadata GET because no table data is read. Metadata create and compare-and-swap are
`CASMetaPut` and `CASMetaCompareSwap`. The raw physical S3 counters remain in every retained
`report.json`, without being added to the logical counters.

All three successful runs produced identical logical counts and rates, so their request-count sample
variance is zero. A cell below is `count / per part / per input GiB`; the small per-GiB rate is large
because its denominator is only 8,000 written bytes.

| Logical request class | Small fresh | Small duplicate/adopt | Wide cold first insert | Wide duplicate/adopt |
|---|---:|---:|---:|---:|
| Blob `HEAD` | 2 / 2.000 / 268435.456 | 2 / 2.000 / 268435.456 | 40150 / 80.300 / 11367.943 | 40150 / 80.300 / 11367.943 |
| Body publication | 2 / 2.000 / 268435.456 | 0 / 0 / 0 | 19322 / 38.644 / 5470.770 | 0 / 0 / 0 |
| Body streaming PUT | 2 / 2.000 / 268435.456 | 0 / 0 / 0 | 19322 / 38.644 / 5470.770 | 0 / 0 / 0 |
| Body server-side copy | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| Metadata GET | 0 / 0 / 0 | 2 / 2.000 / 268435.456 | 20828 / 41.656 / 5897.174 | 40150 / 80.300 / 11367.943 |
| Metadata create | 2 / 2.000 / 268435.456 | 0 / 0 / 0 | 19322 / 38.644 / 5470.770 | 0 / 0 / 0 |
| Metadata compare-and-swap | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |

| Path | Fan-out tasks | `HEAD` hit/miss | Body avoided | `HEAD`/task | Body publication/task | Metadata GET/task | Avoided fraction |
|---|---:|---:|---:|---:|---:|---:|---:|
| Small fresh | 2 | 0 / 2 | 0 | 1.000000 | 1.000000 | 0 | 0 |
| Small duplicate/adopt | 2 | 2 / 0 | 2 | 1.000000 | 0 | 1.000000 | 1.000000 |
| Wide cold first insert | 40150 | 20828 / 19322 | 20828 | 1.000000 | 0.481245 | 0.518755 | 0.518755 |
| Wide duplicate/adopt | 40150 | 40150 / 0 | 40150 | 1.000000 | 0 | 1.000000 | 1.000000 |

The first wide insert is cold but mixed: deterministic files that are identical across partitions
are adopted later within the same insert. Its 20,828 metadata GETs correspond exactly to 20,828
`HEAD` hits; its 19,322 genuinely fresh misses have zero metadata GETs and exactly 19,322 body PUTs
plus 19,322 clean metadata creates. The pure-fresh small leg independently proves the common miss
path has no metadata GET. Duplicate adoption removes every body publication while adding one
metadata GET per hit. `CASMetaAdoptBackfill`, `CASMetaResurrectClean`, `CASMetaCompareSwap`,
`S3CopyObject`, and `DiskS3CopyObject` were zero in all four CA paths.

## Baseline provenance audit {#baseline-provenance-audit}

| Candidate | Provenance found | Classification |
|---|---|---|
| Historical `S41` history | Five rows dated 2026-07-23/24: one failed dev, two passed dev, and two passed full runs at source prefixes `29c98dcfd05c` and `a9449127f724`; all five referenced run directories are absent | Rejected: no retained raw metrics, different harness/order, no binary build ID, and no reproducible same-environment pair |
| Historical staging note | `26.6.1.20000.altinityantalya`, two replicas, 10M rows/30 columns/500 partitions, one CAS value 170.6 s and one plain-S3 value 22.4 s | Rejected: different node topology and unrecorded endpoint, binary ID, exact schema/order/warmth, run artifacts, repetitions, and variance |
| `build_asan/programs/clickhouse` | `26.6.1.20000.altinityantalya`, build ID `f8e1a56588513ff97bb0cf368ada5e029c9e66df`, `SANITIZE=address` | Rejected: unlike build mode and no matched run or source-to-binary proof |
| `tmp/docker-cas-nightly/clickhouse` | `26.6.1.1`, build ID `712fa3c6859c26b8a37b8cfb7a00bb5795e8a4e7` | Rejected: source/configuration provenance and matched environment are absent |
| Pre-change source boundary | `5147dc4e963969389a66ac446f8c8f39280bb1c7`, the parent of `907c3b5ce7d83e8f8c5f3db798de9ee67f6ceb6c` (`Publish CAS blobs after mandatory HEAD`) | Not a measurement: no verified binary or retained run exists for this source point |

The pre-existing uncommitted `RUN_HISTORY.md` diff contains 19 rows for `S34`, `S35`, `S44`, and
`S45`, not `S41`. It was read for provenance and left unchanged. The Task 11 runner suppressed only
history/backlog appenders so measurement could not modify those user-owned rows; it did not replace
any scenario, SQL, sampler, or verdict logic.

## Acceptance blocker {#acceptance-blocker}

The target protocol observation passes, but the performance decision does not. Specifically:

- measured fresh small-blob latency change: **unavailable**;
- measured before/after wide-insert slowdown or speedup: **unavailable**;
- before/after request delta: **unavailable**;
- matched before/after RSS delta: **unavailable**.

The historical 170.6 s and 22.4 s values and the two old local binaries are intentionally excluded
from every median, variance, ratio, and conclusion above. Comparing any of them with this target
would mix stacks, binaries, topology, measurement order, or retained evidence. The change therefore
has no performance acceptance yet.

## Required matched rerun {#required-matched-rerun}

Build a verified pre-change binary from
`5147dc4e963969389a66ac446f8c8f39280bb1c7` and a target binary from the accepted target source with
the same compiler, build flags, sanitizer mode, and container mount. Record both full build IDs.
Then run at least three successful fresh-reset pairs while holding all of the following constant:

- physical host, Docker engine, one ClickHouse node, container limit, RustFS image and endpoint;
- compose/config files, object prefixes, schema, deterministic expressions, rows, columns, part
  counts, leg order, and same-table duplicate sequence;
- insert settings, profiler periods, RSS sampling, stopped merges, stopped background CAS GC, and
  end checkpoint;
- per-pair volume reset and warmth, with the binary order balanced across pairs to expose time drift.

The target protocol verdict must remain strict. If the old request contract cannot satisfy that
target-only verdict, label its protocol observation as baseline measurement rather than changing the
workload or silently accepting it. Report paired medians and sample variance for small latency, wide
wall/query time, peak RSS, request ratios, and duplicate benefit. Acceptance requires an explicit
decision over that matched pair; a target-only rerun cannot clear this blocker.

## Commands and retained artifacts {#commands-and-retained-artifacts}

All commands ran from `utils/ca-soak`, sequentially. Before each run the log directory was preserved,
then a new writable `logs_s41/ch1` was created. `tmp/task11_run_s41.py` calls the ordinary scenario
runner after replacing only the two tracked-history appenders with no-ops.

```bash
docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_s41_full1_reset.log 2>&1
docker compose -f docker-compose-s41.yml up -d > ../../build/task11_s41_full1_up.log 2>&1
PYTHONPATH=. CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1 CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1 python3 ../../tmp/task11_run_s41.py --scenario S41 --seed 1111 --duration 15m --scale full --no-reset > ../../build/task11_s41_full1_run.log 2>&1

docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_s41_full2_reset.log 2>&1
docker compose -f docker-compose-s41.yml up -d > ../../build/task11_s41_full2_up.log 2>&1
PYTHONPATH=. CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1 CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1 python3 ../../tmp/task11_run_s41.py --scenario S41 --seed 1112 --duration 15m --scale full --no-reset > ../../build/task11_s41_full2_run.log 2>&1

docker compose -f docker-compose-s41.yml down -v --remove-orphans > ../../build/task11_s41_full3_reset.log 2>&1
docker compose -f docker-compose-s41.yml up -d > ../../build/task11_s41_full3_up.log 2>&1
PYTHONPATH=. CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1 CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1 CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1 python3 ../../tmp/task11_run_s41.py --scenario S41 --seed 1113 --duration 15m --scale full --no-reset > ../../build/task11_s41_full3_run.log 2>&1
```

| Repetition | Seed | UTC interval | Result | Retained report and host log |
|---:|---:|---|---|---|
| 1 | 1111 | 00:38:56–00:49:01 | `PASS`, 45/45 verdicts | `utils/ca-soak/scenarios/runs/20260823T003855_S41_seed1111/report.json`; `build/task11_s41_full1_run.log` |
| 2 | 1112 | 00:50:14–01:00:38 | `PASS`, 45/45 verdicts | `utils/ca-soak/scenarios/runs/20260823T005013_S41_seed1112/report.json`; `build/task11_s41_full2_run.log` |
| 3 | 1113 | 01:01:26–01:11:36 | `PASS`, 45/45 verdicts | `utils/ca-soak/scenarios/runs/20260823T010125_S41_seed1113/report.json`; `build/task11_s41_full3_run.log` |

Aggregation command and retained output:

```bash
python3 tmp/task11_s41_stats.py > build/task11_s41_stats.log
```

Each run directory retains `report.json`, `report.md`, `metrics.sqlite`, raw event summaries, traces,
the final state, and the generated configuration. Host server logs are retained as
`utils/ca-soak/logs_s41/ch1_full1`, `ch1_full2`, and `ch1_full3` after normal compose cleanup.
