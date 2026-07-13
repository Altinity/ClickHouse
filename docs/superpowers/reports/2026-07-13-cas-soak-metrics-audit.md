---
description: 'Consolidated metrics audit of the 2026-07-13 CAS 2h chaos soak: S3 op budget, CAS decomposition, per-kind and per-part attribution, GC economics, trace_log profiles, $ estimates'
sidebar_label: 'CAS Soak Metrics Audit 2026-07-13'
sidebar_position: 20260713
slug: /superpowers/reports/cas-soak-metrics-audit-2026-07-13
title: 'CAS 2h Chaos Soak — Consolidated Metrics Audit (2026-07-13)'
doc_type: 'reference'
---

# CAS 2h Chaos Soak — Consolidated Metrics Audit (2026-07-13) {#cas-soak-metrics-audit}

**Run:** task-3 closure soak, seed `2026071312`, `DURATION=2h`, 6 workers, 2 replicas (ch1/ch2) +
keeper + rustfs; binary = `cas-gc-rebuild` @ availfix (`4f4f93c6bc6`), driver @ `7fb4c952a2a`.
**Measurement window:** t+72 min (S3/CAS/workload totals) and t+100 min (GC log), mid-run snapshots;
the run was still healthy (0 failures) at both points.
**Sources and methodology:** `system.metric_log` per-second deltas summed over the whole run —
`system.events` is NOT usable under chaos (resets on every fault restart; verified:
`max(per-row delta) < current events value`, so the columns are true deltas). Audit trail:
`system.content_addressed_log`; per-round GC: `system.content_addressed_garbage_collection_log`;
attribution: `system.query_log` / `system.part_log` `ProfileEvents` maps; profiles:
`system.trace_log`.

## Workload executed (both nodes, t+72 min) {#workload}

| Metric | Value |
|---|---|
| INSERT queries / rows | 29 503 / 30.3M |
| Parts created (`NewPart`) | 44 655 |
| Merges / mutations | 14 440 / 3 854 |
| Relink fetches (`DownloadPart`) | 28 660 |
| Parts removed | 88 422 |
| GC rounds (leader, Success) | ~272 at t+72; 490 at t+100 |
| Chaos faults | kill/restart/pause/freeze_long over ch1/ch2/both/rustfs |

## S3 request totals and reconciliation {#s3-totals}

| Op | Total (both nodes) | Reconciliation |
|---|---|---|
| PUT | 1 128 370 | `CasConditionalWriteAttempts` = 1 133 550 — **1:1, every PUT is a controlled conditional write** |
| GET | 5 704 432 | merges/mutations source reads + GC fold + relink `loadMeta` + cache misses |
| HEAD | 9 582 878 | dedup probes ≈ builds × files (~7M) + part-folder validate-on-hit + GC exact-token rechecks |
| LIST | 81 633 | ≈300 pages × GC round (async lister; see attribution artifacts) + recovery/sweeps |
| DELETE | 764 224 | audit: 253k blobs + 172k manifests + ref cleanup + staging debris; free of charge |
| Multipart / Copy | 0 / 0 | small parts; S3-native staging OFF (opt-in) |

### PUT decomposition {#put-decomposition}

| Class | Count | Share |
|---|---|---|
| **Freshness tags on dedup adoption** | ~598k | **53%** |
| Blob bodies (`blob_put`) | 263k | 23% |
| Part manifests | ~175k | 16% |
| Ref-log appends (`CasRefBatchFlushes` 82 234) | 82k | 7% — **batches 3.5M logical ref ops 43:1** |
| GC state/coverage + mount leases + misc | ~2k | <1% |

Dedup adoption ratio: **69%** (598k adopts vs 264k uploads). Snapshot publishes are negligible
post-`3c7003ce190` (aged+uncovered trigger, threshold 256).

## Attribution by query kind (`query_log`, QueryFinish) {#query-kind}

| Kind | n | S3 PUT | S3 GET | S3 HEAD | avg / max |
|---|---|---|---|---|---|
| Insert | 29 567 | 401 721 | 654 164 | 1 274 326 | 372 ms / 60 s |
| Select | 3 429 | 0 | ~3 941 | 82 | 4–64 ms / 73 s |
| Optimize | 1 762 | 0 | 0 | 0 | ~1 s / 44 s |
| Alter | 352 | 0 | 0 | 0 | 25–124 ms / 18 s |

- Per INSERT: ~13.6 PUT / 22 GET / 43 HEAD (~1.5 parts each). `cas_cond == s3_put` holds per kind.
- **64% of PUTs and ~88% of read-class ops are background** (merges, GC, fetches) — not queries.
- SELECTs are nearly S3-free (~1.2 GET each) — local metadata/page caches serve hot data.
- Latency maxima are chaos-window stalls ridden inside the availfix 90 s envelope (by design).

## Attribution by part operation (`part_log`) {#part-log}

| event_type | n | S3 GET | S3 HEAD | cas_cond | blob_put | avg |
|---|---|---|---|---|---|---|
| **DownloadPart (relink)** | 28 706 | **1 021 705** | **1 957 163** | 101 668 | **0** | 870 ms |
| NewPart | 46 563 | 654 343 | 1 274 677 | 402 036 | 173 678 | 238 ms |
| MergeParts | 15 028 | 321 472 | 563 743 | 259 359 | 82 479 | 284 ms |
| MutatePart | 3 967 | 92 623 | 200 360 | 38 618 | 10 625 | 350 ms |
| RemovePart | 91 371 | 0 | 0 | 0 | 0 | 0 |

- **Relink fetch is the top read consumer**: ~36 GET + 68 HEAD per fetch (per-file `loadMeta` GET +
  occupancy/adopt HEAD), zero byte copy (`blob_put=0` — relink works). 2.98M read ops total —
  more than merges and mutations combined.
- `RemovePart` = 0 S3: removal is a batched ref op; physical deletion deferred to GC (by design).
- Merge row traffic 1.13B rows vs 30.3M inserted ≈ 37× write amplification (1-row-part synthetic
  worst case; not representative of production).

## GC economics (`content_addressed_garbage_collection_log`) {#gc}

| | ch1 (leader) | ch2 (takeover) |
|---|---|---|
| Success rounds | 395 (avg 3.2 s, max 17 s) | 95 (avg 0.4 s) |
| NotALeader probes | 70 | 503 (avg 2 ms) |
| objects / manifests deleted | 263 410 / 174 471 | 1 634 / 928 |
| condemned ≈ graduated | 263 672 ≈ 263 528 | balanced |
| spared / fences / anomalies | 246 / 2 / **0** | 0 / 5 / **0** |

- Per Success round (ch1): ~1 850 GET + 4 720 HEAD + 1 320 DELETE; per deleted object ≈ 2.9 GET +
  7.1 HEAD (fail-safe exact-token verification — the deliberate trade).
- Full `ProfileEvents` map extras: **IO buffer churn 1.96 GB/round** (fresh `ReadBufferFromS3`
  per fold GET; avg useful read ~3.7 KB vs ~1 MB buffer — optimization candidate);
  `ReadBufferFromS3Bytes` 24.4 MB/round; GC conditional writes rare but fat (~1 MB snapshot-run
  records, 5.5/round); `DiskConnectionsReused` ≈ 100%.
- GC share of the bill: 13% of GET, 20% of HEAD, ~all DELETEs (free) ⇒ **~$0.65/h of the $10/h
  total (~6.5%)** — the collector is cheap; leadership failover worked across chaos (7 fence-outs,
  0 anomalies).

## trace_log profiles (t+40 min) {#trace-log}

- **CPU** (833 samples): `applyRefLogTxn` / `HeadObject` / `flushRefBatch` / `admits` /
  `GetObject`; the pre-patch `RefTableState::operator=` hotspot is GONE (copy-once fix visible).
- **Real** (204k samples): top product wait = `Store::appendRefOps` queue (19.9k samples ≈ 1.6% of
  worker wall-time) — INSERT threads queue behind the per-table flush leader; everything else is
  parked pools and S3/keeper waits proportional to load.
- **Memory** (233k alloc samples, 982 GB cumulative churn): 88% = per-part-file write buffer
  constructors (`WriteBufferFromFileBase` 606 GB + `MergeTreeWriterStream` 258 GB) — allocation
  churn from tiny parts, NOT retention (resident stable at 924 MiB; no frees are sampled by
  design).

## Cost estimate (AWS S3 standard) {#cost}

| Class | Ops | Rate | $ |
|---|---|---|---|
| PUT-class (PUT+LIST) | 1 210 003 | $0.005/1k | **$6.05** |
| GET-class (GET+HEAD) | 15 287 310 | $0.0004/1k | **$6.11** |
| DELETE | 764 224 | free | $0 |
| **Total** | | | **≈ $12.2 / 72 min ≈ $10/h** |

≈ $0.41 per 1 000 INSERTs at this synthetic worst-case load (30M rows/h in ~1-row parts, 2
replicas, chaos active). Storage cost excluded (pool ≤ ~5 GB, negligible for the run).

## Optimization levers, ranked by $ impact {#levers}

1. **Relink-fetch per-file probes** (~30% of read class): adopt via manifest hash identity without
   per-file HEAD+GET.
2. **Freshness-tag PUT per adoption** (53% of PUT class): batch or skip within a TTL window.
3. **Dedup-probe HEADs** (~7M): larger/longer dedup cache.
4. **GC LIST discovery** (~300 pages/round): known quadratic-LIST backlog item.
5. **GC fold buffer churn** (1.96 GB/round): reuse read buffers or size them to body scale.
6. `appendRefOps` queue wait (1.6% worker time): lane parallelism — lowest priority.

## Attribution artifacts (для будущих аудитов) {#artifacts}

1. `system.events` resets on every chaos restart — use `metric_log` delta sums for run totals.
2. `part_log.ProfileEvents` shows `s3_put=0` while `cas_cond>0`: `S3PutObject` increments on
   upload-pool threads outside the part-op scope; use `CasConditionalWriteAttempts` for per-part
   writes.
3. GC rounds' map contains NO `S3ListObjects` key at all: enumeration pages are fetched by the
   shared async-lister pool outside the round scope.
4. S3 DELETEs are counted under `S3WriteRequestsCount` (write-class requests).

## Erratum {#erratum}

During this audit `CasRootGet` was observed live (backend counter of GETs on the `roots/` prefix,
`ProfileEvents.cpp:795`) — the s03_s05 card fix dispatch brief had falsely declared it removed. The
oracle's counter swap to `CasRefLogBodyGets` stands on consumer-isolation grounds; the misleading
comment was corrected in `8d34bb504ed`.

## Files written vs S3 ops per part type (INSERTs) {#files-vs-s3}

`part_log` `NewPart` by `part_type` × `disk_name` revealed the storage policy is TIERED: small
insert blocks become **Compact parts on the local `default` disk** (zero S3, 4-8 ms), large blocks
go straight to **Wide parts on the `ca` disk**; merges/moves later produce Wide@ca parts.

| | Compact@default (39%) | Wide@ca (61%) |
|---|---|---|
| parts | ~18 700 | ~29 600 |
| `FileOpen` per part | 17.6 | 26.8 |
| S3 conditional writes per part | **0** | 13.6 (5.8 blob bodies + ~1 manifest + ~6.8 tags/ref) |
| S3 GET / HEAD per part | 0 / 0 | 22 / 43.6 |
| avg create time | 4–8 ms | ~370 ms |
| avg rows | 1 059–1 860 | 176–779 |

Insights:

1. **~78% of a Wide part's files never become S3 objects**: 26.8 files opened → only 5.8 blob
   bodies + 1 manifest; marks, `primary.idx`, checksums, serialization metadata and other
   sub-`INLINE_CAP` files ride INSIDE the manifest. The inline design saves ~20 PUTs per part.
2. **The local hot tier absorbs 39% of insert parts entirely** — without it every micro-insert
   would pay the full CAS publish; this is a large contributor to the modest 13.6 PUT/insert.
3. The Compact/Wide split is byte-driven (`min_bytes_for_wide_part`), not row-driven: parts with
   fat `payload` strings go Wide at ~200 rows while 1 800-row thin parts stay Compact.
4. HEAD ≈ 1.6 × files for Wide parts (per-file dedup probe + adopt/freshness verification) —
   consistent with the dedup-by-hash design.

## Final run outcome {#final-outcome}

**PHASE3 OK** — the full 2h run completed green: 38 chaos faults survived, clean teardown.
Final `AVAILABILITY`: `node_down=723` (irreducible — kill/restart faults), `aborted_persistent=20`
(compound fault windows occasionally exceeding the 90 s controller envelope — the known M3 backlog
residual), `mount_fenced=3`, `s3_transient=1`. 24 non-node-down driver retries over 2 hours of
sustained chaos.
