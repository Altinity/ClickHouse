---
description: 'Measured write-path baseline for a wide CAS-on-S3 INSERT (30 columns, 500 partitions, 10M rows) and a bottleneck diagnosis'
sidebar_label: 'CAS wide-insert write-path baseline'
sidebar_position: 999
slug: /superpowers/reports/2026-07-23-cas-wide-insert-baseline
title: 'CAS wide-insert write-path baseline'
doc_type: 'reference'
---

# CAS wide-insert write-path baseline {#cas-wide-insert-write-path-baseline}

Date: 2026-07-23
Branch: `cas-gc-rebuild`, HEAD `29c98dcfd05`
Binary: `build/programs/clickhouse` (RelWithDebInfo, optimized, with symbols)
Scenario card: `utils/ca-soak/scenarios/cards/s41_wide_insert_baseline.py` (`S41`, run at `--scale full`)
Run artifacts: `utils/ca-soak/scenarios/runs/20260723T185256_S41_seed1/` (`report.md`, `report.json`)

## What was measured {#what-was-measured}

One big wide INSERT into a `MergeTree` table with 30 mixed-type columns (`UInt`/`Int` of several
widths, `Float32`/`Float64`, `LowCardinality(String)`, variable 16-80 byte `String`s, `DateTime`,
`Date`, a few `Nullable`), 10,000,000 rows, 500 partitions. The insert is fully deterministic (every
value is a pure function of the row number — no randomness), single insert thread, one part per
partition (500 parts) so the workload drives many parts x 30 columns through the serial commit path.

Two legs on the SAME single node with identical DDL and identical INSERT:

- `s3plain` — the standard S3 disk (`metadata_type=local`, data on S3, metadata local). Baseline.
- `ca` — content-addressed over the same RustFS (`metadata_type=content_addressed`). Under test.

Both measured inserts ran with the Real and CPU query profilers enabled (5 ms / 10 ms periods), so
`system.trace_log` for each insert's `query_id` yields thousands of symbolized stacks. Merges were
stopped during each measured insert (`SYSTEM STOP MERGES`) to isolate the pure write path. Single
node is deliberate: it removes the interserver-fetch / replication confound, so the INSERT-query
`ProfileEvents` + `trace_log` on the writer are the pure write path.

The run is isolated in its own docker-compose project (`ca-s41`, `docker-compose-s41.yml`) so it does
not disturb any concurrently-running `ca-soak` stack.

## Headline result {#headline-result}

| metric | plain S3 | CAS-on-S3 | ratio |
|---|---|---|---|
| INSERT wall time | 19.44 s | 58.41 s | **3.0x** |
| parts created | 500 | 500 | — |
| bytes written (logical) | 3.53 GiB | 3.53 GiB | — |
| S3 PUT | 102,002 | 119,432 | 1.17x |
| S3 HEAD | 0 | 134,400 | ∞ (CAS-only) |
| S3 GET | ~0 | 54,360 | CAS-only |
| CPU-busy / wall | 1.25 (multi-threaded) | 0.375 (single-threaded) | — |

On this workload the CAS-on-S3 wide INSERT is **3.0x slower** than the standard S3 disk. (The prior
backlog finding logged ~7.6x at 500 partitions; the smaller factor here is consistent with a
single-node measurement — no interserver replication leg — plus the `expect_continue_min_bytes`
tuning already in the disk config and the fresh optimized binary. The *shape* of the bottleneck is
identical to what the stage-1 design predicts.)

## Bottleneck diagnosis {#bottleneck-diagnosis}

The measured answers to the questions the write-path stage-1 design raises:

### (a) CA-vs-plain slowdown factor {#a-slowdown-factor}

**3.0x** (CAS 58.41 s vs plain 19.44 s), both producing 500 parts from 3.53 GiB of identical data.

### (b) Top write-path cost centers (from `trace_log`, CA leg, Real trace) {#b-cost-centers}

Real samples (wall-clock, includes off-CPU waits), classified by the nearest write-path frame:

| cost center | % of Real wall |
|---|---|
| S3 network (PUT/upload + response wait) | **87.2%** |
| HEAD-before-PUT dedup gate | **12.6%** |
| everything else (serialization, hashing, ledger/manifest, merge-tree part write) | **< 0.2%** |

**99.8% of the CAS insert's wall time is spent in the S3 PUT/HEAD network path.** Serialization,
blob hashing, ref-ledger/manifest encoding and the merge-tree part writer are together under 0.2% of
wall — they are not the bottleneck. Ground-truth top Real stacks: `poll` →
`Poco::Net::SocketImpl::receiveBytes` (blocked reading an S3 response) and
`pthread_cond_wait` → `std::condition_variable::wait` (the insert thread blocked waiting for the
serial upload to finish).

### (c) Is single-threaded blob upload the dominant bottleneck? — YES {#c-single-threaded}

- Real wall is **99.8%** in the S3 network + HEAD path.
- Those waits concentrate in **one dominant thread** (top thread holds **72.3%** of Real samples;
  only **3** distinct threads appear at all).
- CPU-busy time is only **37.5%** of wall — the other **62.5%** is off-CPU network wait.

Contrast with the plain-S3 leg, which spends **24.3 s of CPU over 19.4 s of wall** (CPU/wall = 1.25):
plain S3 overlaps its uploads across threads (streaming `WriteBufferFromS3` per column file), so
upload latency is hidden behind concurrency. CAS instead stages every blob to local scratch first and
then uploads the staged blobs in a **serial loop** (`ContentAddressedTransaction::uploadPendingBlobs`),
and commits parts one at a time — so one thread does the orchestration and blocks on each PUT/HEAD in
turn. The ref-ledger confirms the fully serial commit: `CasRefBatchFlushes` = `CasRefBatchedMutations`
= 1000 over 500 parts, i.e. an **average batch size of exactly 1.0**.

This directly validates the write-path stage-1 direction (parallel blob upload within a part) and
shows the larger win is stage 2 (concurrent `commitPart` dispatch): with the commit serial, only
~1-3 threads ever touch the network at once, versus plain S3's higher effective upload concurrency.

### (d) HEAD-before-PUT dedup-gate share {#d-dedup-head-gate}

The dedup gate issues a HEAD before uploading a blob body. On this fresh-pool insert:

- `CasBlobHeadFirst` = 26,680, `CasBlobBodyPutAvoided` = 26,680 — i.e. 26,680 blob bodies were
  deduplicated away (identical low-entropy content — `LowCardinality` dictionaries, small
  fixed-width columns, the constant partition column — recurs across the 500 parts), so their body
  upload was correctly avoided; the HEAD was still paid.
- Total S3 HEADs = **134,400** (268.8 per part) — a cost class the plain S3 disk does **not** incur
  at all (plain S3 HEAD = 0).
- The dedup HEAD gate accounts for **12.6%** of the CAS insert's Real wall time.

So the HEAD-before-PUT gate is a real, measurable secondary cost (~1/8 of wall and 134k extra S3
ops), but it is not the primary bottleneck — the serial body PUT path is.

### (e) S3 op budget {#e-s3-op-budget}

CAS leg, 500 parts, 3.53 GiB written:

| op | total | per part | per GiB |
|---|---|---|---|
| PUT | 119,432 | 238.9 | 33,816 |
| HEAD | 134,400 | 268.8 | 38,054 |
| GET | 54,360 | 108.7 | 15,391 |

Plain S3 leg, same data: 102,002 PUT (204 per part), **0** HEAD, ~0 GET. CAS's extra S3 traffic over
plain is therefore ≈ 134,400 HEADs + 54,360 GETs + ~17,000 extra PUTs (manifests/refs) ≈ 206k extra
operations — all of it in the serial single-threaded path, which is why it converts almost linearly
into wall time.

## Verdict for the stage-1 gate {#verdict-for-stage-1-gate}

The standing hypothesis is **confirmed**: the CAS-on-S3 wide-insert write path is bound by serial,
single-threaded blob upload. 99.8% of wall time is S3 PUT/HEAD network I/O, concentrated in one
thread, with CPU busy only 37.5% of wall; the plain S3 disk is 3.0x faster on identical data purely
because it uploads with concurrency the CAS path lacks. The ref-ledger batches at exactly 1.0,
confirming the serial commit path the design describes.

This gates the write-path stage-1 design (`docs/superpowers/specs/2026-07-22-cas-writepath-stage1-internal-design.md`)
as **worth implementing**, with two caveats grounded in these numbers:

- Stage 1 (parallel blob upload *within* a part) helps, but on this profile CAS packs ~77 unique
  blob PUTs per part on average (38,644 real PUTs / 500 parts) plus dedup HEADs, so intra-part
  fan-out has a large surface to parallelize — more than the design's "~2 blobs/part" rule-of-thumb
  suggested for the narrow case. The bounded-win caveat in the design applies to narrow parts, not
  to this wide profile.
- The serial *commit* (batch size 1.0, one part in flight) means stage 2 (concurrent `commitPart`
  dispatch) is where cross-part upload concurrency — and therefore the bulk of the closing of the
  3.0x gap toward plain S3's overlapped-upload throughput — will come from.

The HEAD-before-PUT dedup gate (12.6% of wall, 134k HEADs) is a real secondary target (a separate
backlog item), but not the primary one.

## Reproduction {#reproduction}

```text
# Bring up the isolated single-node stack (own docker-compose project `ca-s41`, host port 18123):
cd utils/ca-soak
docker compose -f docker-compose-s41.yml down -v --remove-orphans
mkdir -p logs_s41/ch1 && chmod 777 logs_s41/ch1
docker compose -f docker-compose-s41.yml up -d          # wait for http://localhost:18123/ping

# Point the framework at the isolated stack and run the card (no-reset: the stack is already up):
export CA_SOAK_NODE_COUNT=1 CA_SOAK_NODE1_PORT=18123 CA_SOAK_NODE1_CONTAINER=ca-s41-ch1-1
export CA_SOAK_RUSTFS_CONTAINER=ca-s41-rustfs1-1 CA_SOAK_CH_CONTAINERS=ca-s41-ch1-1
export CA_SOAK_FSCK_CONTAINER=ca-s41-ch1-1
python3 -m scenarios.run --scenario s41 --no-reset --scale full --seed 1
```

Storage config: `utils/ca-soak/configs/storage_conf_s41.xml` (defines both the `ca` content-addressed
disk and the `s3plain` standard S3 disk over distinct pool prefixes). fsck config:
`utils/ca-soak/configs/fsck_only_s41.xml`.
