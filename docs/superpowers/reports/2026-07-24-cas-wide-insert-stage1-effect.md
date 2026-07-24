---
description: 'Re-profile of the wide CAS-on-S3 INSERT after write-path stage 1 (parallel intra-part blob upload) versus the 2026-07-23 baseline, with a stage-2 gate recommendation'
sidebar_label: 'CAS wide-insert stage-1 effect'
sidebar_position: 999
slug: /superpowers/reports/2026-07-24-cas-wide-insert-stage1-effect
title: 'CAS wide-insert write-path stage-1 effect'
doc_type: 'reference'
---

# CAS wide-insert write-path stage-1 effect {#cas-wide-insert-write-path-stage-1-effect}

Date: 2026-07-24
Branch: `cas-gc-rebuild`, stage-1 tree at HEAD `a9449127f72` plus the T14 minors sweep (the `build/`
binary was rebuilt from the minors-swept working tree at 12:51; the source lands in the T14 commits).
Binary: `build/programs/clickhouse` (RelWithDebInfo, optimized, with symbols).
Scenario card: `utils/ca-soak/scenarios/cards/s41_wide_insert_baseline.py` (`S41`, `--scale full`).
Baseline compared against: `docs/superpowers/reports/2026-07-23-cas-wide-insert-baseline.md`.
Run artifacts: `utils/ca-soak/scenarios/runs/20260724T105212_S41_seed1/` (`report.md`, `report.json`).

## What changed between the two runs {#what-changed}

Identical workload, DDL, seed, and isolated single-node compose (`ca-s41`, host port 18123) as the
baseline: one wide `MergeTree` INSERT of 10,000,000 rows across 30 mixed-type columns into 500
partitions (500 parts), measured on both the standard S3 disk (`s3plain`) and the content-addressed
disk (`ca`) over the same RustFS, with the Real and CPU query profilers enabled. The ONLY difference
is the binary: the baseline ran the pre-stage-1 tree (`29c98dcfd05`); this run has write-path stage 1
— parallel intra-part blob upload on the server-wide `CasBlobUploadPool` via `fanOutBlobUploads` —
plus §2-§5 of the stage.

## Headline result {#headline-result}

| metric | baseline (pre-stage-1) | stage 1 | change |
|---|---|---|---|
| CA INSERT wall | 58.41 s | **30.26 s** | **−48% (1.93x faster)** |
| plain S3 INSERT wall | 19.44 s | 19.05 s | unchanged (path untouched) |
| CA-vs-plain factor | 3.0x | **1.59x** | gap roughly halved |
| CA parts created | 500 | 500 | — |
| CA logical bytes | 3.53 GiB | 3.53 GiB | — |

The stage-1 win on this profile is **much larger than the spec's "bounded, ~2 blobs/part" rule of
thumb** predicted for the narrow case — as the point-4 addendum foresaw, the wide profile packs many
unique blobs per part (measured `CasBlobUploadFanoutTasks / batches = 40,150 / 500 ≈ 80 blob tasks
per part`), so intra-part fan-out has a large surface to parallelize.

## The single-threaded signature is gone {#single-threaded-signature-gone}

This is the direct, mechanical evidence that the fan-out did what it was designed to do. All figures
are from `system.trace_log` Real samples on the CA INSERT query.

| Real-trace signature | baseline | stage 1 | reading |
|---|---|---|---|
| top single thread's share of Real samples | **72.3%** | **14.5%** | one dominant thread → spread |
| distinct threads appearing in the Real trace | 3 | **33** | uploads now overlap across the pool |
| CPU-busy / wall | **0.375** | **1.075** | off-CPU serial wait → genuine multi-thread overlap |
| bottleneck diagnosis (card verdict c) | "single-threaded: YES" | "PARTIAL — network-bound but NOT single-threaded" | serial upload dissolved |

CPU-busy/wall crossing 1.0 is the crux: the baseline spent 62.5% of wall off-CPU, one thread blocked
on each PUT/HEAD in turn; stage 1 now runs upload work concurrently, so aggregate CPU time exceeds
wall clock. The top-thread share collapsing from 72.3% to 14.5% says the same thing from the
thread-distribution side.

## Cost-center shape is unchanged; it is just parallel now {#cost-center-shape}

Classifying the CA Real samples by nearest write-path frame (`report.json` `trace_real_buckets`):

| cost center | baseline % of Real wall | stage 1 % of Real wall |
|---|---|---|
| S3 network (PUT/upload + response wait) | 87.2% | 86.8% |
| HEAD-before-PUT dedup gate | 12.6% | 12.1% |
| ledger / manifest / everything else | < 0.2% | ~1.1% |

The workload is still S3-network-bound (~87%) — stage 1 did not remove network I/O, it overlapped it.
The dedup HEAD gate stays ~12% of wall. What changed is that this same network work is no longer
serialized behind one thread.

## S3 op budget {#s3-op-budget}

Pool-level S3 operation counts over the CA leg (500 parts, 3.53 GiB):

| op | baseline total (per part) | stage 1 total (per part) | change |
|---|---|---|---|
| PUT | 119,432 (238.9) | 119,432 (238.9) | unchanged — parallelism overlaps PUTs, never removes them |
| HEAD | 134,400 (268.8) | 105,140 (210.3) | **−22%** |
| GET | 54,360 (108.7) | 42,656 (85.3) | **−22%** |

PUT is byte-identical (deterministic workload → same set of unique blobs). The HEAD/GET drop is a
genuine secondary benefit of the fan-out: concurrent tasks populate the shared dedup cache
(`CacheBase`, insert-safe under its own lock, spec §1), so more siblings hit a live hint and skip the
HEAD-first probe — `CasBlobHeadFirst` fell from 26,680 to 20,828 (each avoided HEAD also avoids the
downstream body work its miss would trigger).

## Ref-ledger batch size — unchanged BY DESIGN {#ref-batch-size}

`CasRefBatchFlushes = CasRefBatchedMutations = 1000` over 500 parts → **average batch size 1.0**,
exactly as in the baseline. This is the stage-2 acceptance metric, NOT stage 1's — the commit is
still serial (one `commitPart` in flight at a time). Stage 1 deliberately does not touch it, and this
run confirms it was not touched. It was not chased.

## Correctness at quiesce {#correctness-at-quiesce}

The scenario passed 15/15 verdicts. End-checkpoint forced GC drove residual `unreachable` to 0 in one
round; the final detailed `ca-fsck` reported `dangling = 0`; the `ca-gc-dryrun` candidate set was a
subset of the fsck unreachable set. Peak `MemoryResident` during the wide inserts was 1.99 GB
(CA query `memory_usage` 405 MB vs plain 258 MB — CA holds more for staging + the bounded fan-out, but
within budget and not growing).

## Verdict: does the remaining bottleneck justify stage 2? {#stage-2-gate}

Stage 1 closed roughly **half** the CA-vs-plain gap (3.0x → 1.59x, a 48% wall-clock reduction on the
CA leg) and — the load-bearing part — **dissolved the single-threaded signature** the baseline
identified as the primary bottleneck: top-thread Real share 72.3% → 14.5%, CPU/wall 0.375 → 1.075.
Intra-part blob upload is now genuinely concurrent.

The residual **1.59x** gap to plain S3 has two components, both visible in these numbers:

1. **Serial cross-part commit** — the ref-ledger batch size is still 1.0 and one `commitPart` is in
   flight at a time, so only one part's blob fan-out saturates the network at once. Plain S3 gets
   cross-file upload concurrency for free from its streaming per-column `WriteBufferFromS3`. Closing
   this is exactly stage 2's target (concurrent `commitPart` dispatch in the sink) — it is where the
   remaining cross-part overlap lives.
2. **CAS-only dedup HEAD + GET traffic** — ~105k HEADs + ~43k GETs (≈12% of Real wall) that plain S3
   does not incur at all. This is a separate, already-tracked backlog item (the HEAD-before-PUT gate
   and the unconditional promote manifest GET), independent of stage 2.

Recommendation: **stage 2 remains justified** and is the right next lever for the cross-part overlap —
proceed to the stage-2 brainstorm/spec as the program's point 7 gates. But the bar is honestly lower
than it was at the baseline: stage 1 delivered a bigger single win (3.0x → 1.59x) than the spec
anticipated, so the absolute wall-clock still recoverable by stage 2 (roughly the 1.59x → ~1.0x
cross-part slice) is now comparable in magnitude to what a reduction of the dedup HEAD gate (component
2, ~12% of wall, lower-risk, no upstream-surface change) would recover. Both are worth doing; stage 2
should be scoped against that comparison rather than the original 3.0x framing.

## Reproduction {#reproduction}

```text
cd utils/ca-soak
docker compose -f docker-compose-s41.yml down -v --remove-orphans && docker volume prune -f
docker compose -f docker-compose-s41.yml up -d          # wait for http://localhost:18123/ping
export CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1
export CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1
export CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1
python3 -m scenarios.run --scenario s41 --no-reset --scale full --seed 1
```
