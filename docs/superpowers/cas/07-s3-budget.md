---
description: 'Precise per-protocol-part S3 op-count breakdown for the content-addressed (CAS) MergeTree feature: write, read, and GC budgets with reduction history.'
sidebar_label: 'S3 Op-Count Budget'
sidebar_position: 7
slug: /superpowers/cas/s3-budget
title: 'CAS S3 Op-Count Budget'
doc_type: 'reference'
---

# CAS S3 Op-Count Budget {#cas-s3-budget}

**Status:** DONE (write + GC budgets measured; read budget partially modeled). Sources: measured
`system.content_addressed_log` / `ProfileEvents` from soak runs (annotated as **measured**);
per-code-path counts derived from `CasBuild.cpp`, `CasStore.cpp`, `CasGc.cpp`,
`CasObjectStorageBackend.cpp` (annotated as **code-derived**); projections from the cost model
(`specs/2026-06-08-s3-ops-cost-model.md`) annotated as **modeled/uncertain**.

Request-price reference (AWS S3 Standard, us-east-1):

| Tier | Operations | Price / 1 M |
|---|---|---|
| Read | `GET`, `HEAD` | ~$0.40 |
| Write | `PUT`, `LIST`, `COPY` | ~$5.00 |
| Delete | `DELETE` | free |

---

## 1. Write Budget — per Part {#write-budget}

A "part" is one INSERT, MERGE, or MUTATION output. Let `F` = number of distinct blob files
(data columns, marks, primary index) with size ≥ `dedup_head_first_min_bytes` (default 1 MiB),
`f` = total blob count (including small blobs), `D` = blob dedup-hit count (content already
present in the pool from a prior part), `f - D` = novel blobs to upload. One tree object covers
the file-path listing; one part-manifest object captures the part metadata.

### 1.1 Baseline write path (no dedup cache, no ETag optimization) {#write-budget-baseline}

This was the state prior to the B168 P0/P1/P2 optimizations and the head-after-put ETag fix.

| Step | Operation | Count | Notes |
|---|---|---|---|
| Blob: novel upload | `PUT` (`If-None-Match:*`) | `f - D` | One conditional create per novel blob (**code-derived**). |
| Blob: dedup hit (412 path) | `PUT` (`If-None-Match:*`) → 412 | `D` | Body streamed then discarded on collision (**measured**: 64% hit rate, P0 instrumentation run). |
| Blob: dedup follow-up HEAD | `HEAD` | `D` | `observeAndAdmit` HEAD after a 412 to read the existing token (**code-derived**, `CasBuild.cpp:200`). |
| Blob: head-after-put | `HEAD` | `f - D` | After each novel PUT: a follow-up HEAD to read the object ETag as the admission token, because `WriteBufferFromS3` did not capture the response ETag (**measured**: ~73% of all HEADs pre-fix, `CasObjectStorageBackend.cpp:147`). **ELIMINATED by ETag fix (B168 #1).** |
| Tree: novel upload | `PUT` (`If-None-Match:*`) | 1 | One tree object (content-addressed; dedup applies). |
| Tree: head-after-put (pre-fix) | `HEAD` | 0–1 | Same ETag path as blobs. **ELIMINATED by ETag fix.** |
| Precommit | `PUT` (`If-Match`) | 1 | One `casPut` on the target root shard (appends create-precommit event, **code-derived**, `CasBuild.cpp:582`). |
| Part-manifest | `PUT` (`If-None-Match:*`) | 1 | One unconditional content-addressed manifest object (`CasBuild.cpp:562`). |
| Promote (commit) | `PUT` (`If-Match`) | 1 | One `casPut` on the target root shard (owner precommit → committed ref, **code-derived**, `CasBuild.cpp:657`). |
| Promote: revalidate deps | `HEAD` | 0–`f+1` | Bounded re-HEAD of deps at the publish gate (`CasBuild.cpp:729`). Typically 0 on a hot shard (dedup-cache present); ≤ `f+1` cold (**code-derived**). |

**Baseline total** (pre-optimizations, no dedup): 2·`f` + 4 `PUT`s + `f` + `D` `HEAD`s (ignoring
revalidate). At 64% dedup hit rate that is ≈ 3·`f` + 4 ops for a typical part.

### 1.2 Optimized write path (current, after ETag fix + P1/P2 dedup cache) {#write-budget-optimized}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Blob: cache HIT, HEAD-first present | `HEAD` | `D_cache` | Cache-hit blobs: 1 cheap HEAD replaces a body-PUT + HEAD. `D_cache` ≈ `D` after warmup (**code-derived**, `CasBuild.cpp:131`). |
| Blob: cache HIT, HEAD-first 404 (stale) | `HEAD` + `PUT` | rare | Stale cache hit: HEAD 404, fall through to normal PUT. |
| Blob: large-body cache MISS (P2 size trigger) | `HEAD` + `PUT` | `F - D` | Blobs ≥ 1 MiB and not in cache: HEAD-first check, then PUT on miss (**code-derived**, `CasBuild.cpp:119`). |
| Blob: small-body cache MISS | `PUT` | `(f-F) - D_small` | Small blobs bypassing P2 with no cache hit go straight to conditional PUT. |
| Blob: 412 dedup follow-up HEAD | `HEAD` | near 0 | Residual 412s on concurrent-writer races; negligible at steady state. |
| Blob: head-after-put | `HEAD` | 0 | **ELIMINATED**: `WriteBufferFromS3` now returns the response ETag (**measured soak #6 t=343s**: HEAD/PUT ratio dropped to 1.20 from ~116:1, `CasObjectStorageBackend.cpp:127`). |
| Tree | `PUT` | 1 | Unchanged. ETag from response; no follow-up HEAD. |
| Precommit | `PUT` | 1 | One root-shard `casPut`. |
| Part-manifest | `PUT` | 1 | One content-addressed manifest object. |
| Promote | `PUT` | 1 | One root-shard `casPut`. |

**Optimized total** (warm cache, 64% dedup hit rate, large blobs): ≈ `D` `HEAD`s + `(f-D)+3`
`PUT`s per part. For `f=10` blobs and `D=6` dedup hits: 6 HEADs + 7 PUTs. That is a ~55%
reduction from the baseline 30+ ops.

### 1.3 Mutable files {#write-budget-mutable}

`mutable_files` (e.g. `txn_version.txt`) are inlined into the root shard's `RootRef` protobuf
(`CasRootShardCodec.cpp:121`) and written as part of the promote `casPut` — **0 extra S3 ops**
(**code-derived**).

### 1.4 Watermark renewal {#write-budget-watermark}

Each writer renews its mount-lease heartbeat via one `PUT` per renewal interval. Removed
per-build `CasBuildPut` heartbeat (B167c: redundant with the watermark, see P0 table,
`CasBuildPut` ~21 k/23 min — **modeled/uncertain** whether fully removed).

### 1.5 Manifest `casPut` contention (write-tier 412s) {#write-budget-cas-contention}

Every `casPut` is an `If-Match` conditional PUT. On a conflict (concurrent writer or GC fence)
it 412s, re-reads the shard (1 `GET`), and retries. Measured conflict rate: **35%** of
`casPut` calls at `workers=2` (`CasRootCasConflict` 49 k / `CasRootCas` 93 k, P0 soak). At
`root_shards=64` (after B168 #4) contention is reduced but not eliminated. Each conflict costs
one extra `PUT` (412d) + one `GET` (**measured**).

---

## 2. Read Budget — per Part Open {#read-budget}

A part open involves resolving the part's ref, fetching the part-manifest, then streaming blob
data. The following counts are **per query-thread part open** (not per query).

### 2.1 Ref resolution (`resolveRef`) {#read-budget-resolve}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Root-shard read | `HEAD` + `GET` (cold) | 1 + 1 | `readShardDecoded` HEADs the shard key, GETs the body on a cache miss or token mismatch (`CasStore.cpp:465`, **code-derived**). |
| Root-shard read (warm, single-flight) | 0 ops | 0 | If another thread resolved the same shard concurrently within the TTL window, the result is shared (**code-derived**, `CasStore.cpp:414` coalesced single-flight). |
| Root-shard HEAD (warm, TTL cache) | 0 ops | 0 | If the shard was validated within `allow_stale` TTL, skip the HEAD entirely (**code-derived**, `CasStore.cpp:392`). Default: staleness-tolerant reads opt in; safety-critical reads (publish gate) stay force-fresh. |

**Per distinct shard per shard-TTL window:** 1 `HEAD` + 0–1 `GET`. On a hot server with many
concurrent reads to the same shard, single-flight coalesces to 1 `HEAD` total.

**Decode-cache TTL (`shard_decode_cache_ttl_ms`, default 200):** a staleness-tolerant caller
(`allow_stale = true`) may reuse a decode validated less than `shard_decode_cache_ttl_ms` ago
**without a HEAD**; `0` disables the TTL (all callers force-fresh). Strict-freshness callers (the
publish gate) always pass `allow_stale = false` and always HEAD, regardless of this value.
**Absence is never TTL-cached:** the fast path applies only to PRESENT entries. A just-created ref
must be observable by force-fresh callers, and staleness-tolerant callers re-validate on a miss — so
a "not found" is never cached with a TTL. (Source
`specs/2026-06-14-ca-reduce-s3-op-count-design.md`, Pillar B.)

### 2.2 Part-manifest fetch (`readManifest`) {#read-budget-manifest}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Manifest HEAD | `HEAD` | 1 | Token check for the `(ManifestId, token)` decode cache (`CasStore.cpp:584`, **code-derived**). |
| Manifest GET | `GET` | 0–1 | GET + parse if no cache hit or token changed. Typically 0 on re-open of the same part (**code-derived**). |

**Per part per open:** 1 `HEAD` + 0–1 `GET`.

### 2.3 Blob reads {#read-budget-blobs}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Blob GET (ranged) | `GET` (ranged) | per-column | One ranged GET per column-file read. Count depends on query selectivity. Not tracked per-part by the CA layer (delegated to the MergeTree reader). |
| Blob dedup-cache skip | 0 ops | — | The `dedupCache` is write-path only; read path has no equivalent skip. |

**Modeled/uncertain:** the per-column GET count is determined by MergeTree's read plan, not the
CA layer. The CA layer passes blob keys to `ObjectStorage::readObject`; ranged reads reduce
data transfer but each is one billed GET.

### 2.4 Replication fetch (`DownloadPart`) {#read-budget-replication}

**Measured** (`part_log` attribution, P0 soak, ~95 min): `DownloadPart` issues ~15 HEADs + ~15
GETs per part (53,796 events). In a content-addressed shared pool the blobs already exist; a
replica fetch could relink the manifest without re-downloading (P3 proposal, **TODO/DESIRABLE**,
would eliminate ~1.6 M reads per 95 min window). **Currently the generic MergeTree download
path is used.**

---

## 3. GC Budget — per Round {#gc-budget}

A GC round runs four phases: fold (R1), retire (R2), fence (R3), recheck (R4). The pool has `S`
active ref shards across all namespaces and `C` zero-in-degree blob/tree candidates from the
fold.

### 3.1 Fold (R1) {#gc-budget-fold}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Shard discovery | `LIST` (`cas/refs/`) | ⌈`S` / 1000⌉ | One LIST page per 1000 shards (**code-derived**, `discoverUniverse`, `CasGc.cpp:1286`). Returns shard tokens when backend supports them. |
| LIST-token skip (accelerator) | 0 GETs for skipped shards | `S - S_changed` | If the listed shard token matches the sealed post-fence token, the shard body GET is skipped entirely — no re-read needed (**code-derived**, `CasGc.cpp:1430`). |
| Shard body read (changed or new) | `GET` | `S_changed` | Only shards whose token changed since last round (i.e. received new events) are fetched. At steady state `S_changed ≪ S`. |
| Fold-seal write | `PUT` | 1 | One deterministic write-once `foldSealKey` object per round (**code-derived**, `CasGc.cpp:487`). |
| GC-state CAS (fold-adopt) | `PUT` (`If-Match`) | 1 | One `casPut` advancing `snap_generation` (**code-derived**, `CasGc.cpp:495`). |

**Per-round fold cost (steady state):** ⌈`S`/1000⌉ LISTs + `S_changed` GETs + 2 PUTs. On a
pool with 64 shards and few changes: 1 LIST + a handful of GETs + 2 PUTs. On a fully churned
pool: 1 LIST + 64 GETs + 2 PUTs.

### 3.2 Retire (R2) {#gc-budget-retire}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Candidate HEAD | `HEAD` | `C` | One HEAD per zero-in-degree candidate to read the current token for `deleteExact` (**code-derived**, `CasGc.cpp:539`; **measured**: dominant HEAD source under GC livelock — see §4). |
| Retired-set write | `PUT` | ⌈`C`/shard⌉ | One write-once retired-set object per GC shard (`gc/gen/<g>/attempt/<a>/retired/<shard>`, **code-derived**, `CasGc.cpp:598`). |
| GC-state CAS (round advance) | `PUT` (`If-Match`) | 1 | One `casPut` advancing the round number (**code-derived**, `CasGc.cpp:621`). |

**Per-round retire cost:** `C` HEADs + (⌈`C` / gc_shards⌉ + 1) PUTs. When GC is livelocked
(`gc_state` CAS fails because a peer also runs), the same candidate set `C` is re-HEADed every
round. This was the dominant HEAD storm: **measured 51.2 M HEADs/3.3 h** on the GC leader
(ch1) at 82% 404 miss rate, with ~225 re-probes of the same already-deleted keys (P9 finding,
P0 soak). **Fix: P9 — prune confirmed-deleted/absent candidates from the fold snap so they are
not re-observed (TODO, highest-priority remaining op-count work).**

### 3.3 Fence (R3) {#gc-budget-fence}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Shard discovery (repeat) | `LIST` (`cas/refs/`) | ⌈`S` / 1000⌉ | Second LIST sweep to discover present shards for fencing (**code-derived**, `CasGc.cpp:649`). |
| Per-shard fence `casPut` | `PUT` (`If-Match`) | `S` | One fence CAS per present shard — appends a fence event to advance `fence_round` (**code-derived**, `CasGc.cpp:655`). |
| GC-state CAS (fence) | `PUT` (`If-Match`) | 1 | One `casPut` persisting `fence_version[round]` (**code-derived**, `CasGc.cpp:662`). |

**Per-round fence cost:** 2·⌈`S`/1000⌉ total LISTs (fold + fence) + `S` + 1 PUTs.
**Desirable optimization:** fence only dirty shards (those with pending retires) rather than all
`S` shards (P6 proposal — **TODO/DESIRABLE**).

### 3.4 Recheck (R4) {#gc-budget-recheck}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Post-fence shard token read | `HEAD` or LIST-cached | `S_fenced` | Reads the post-fence token for each fenced shard (**code-derived**, `CasGc.cpp:721`). Can use the fence `casPut` response token where the backend returns it; otherwise a HEAD. |
| Shard body re-read (fold-through-fence) | `GET` | `S_fenced` | Re-reads each fenced shard body to fold the events between cursor and fence (**code-derived**). |
| `deleteExact` (blob/tree deletes) | `DELETE` | `C_confirmed` | Exact-token deletes for confirmed zero-in-degree candidates (**code-derived**, `CasObjectStorageBackend.cpp:522`). **DELETE is free on AWS.** |
| Completion-seal write | `PUT` | 1 | One deterministic write-once `completionSealKey` object (**code-derived**, `CasGc.cpp:992`). |
| GC-state CAS (completion) | `PUT` (`If-Match`) | 1 | One `casPut` finalizing the round (**code-derived**, `CasGc.cpp:1007`). |

**Per-round recheck cost:** `S_fenced` HEADs + `S_fenced` GETs + `C_confirmed` DELETEs (free) +
2 PUTs.

### 3.5 Snap prune (per round, amortized) {#gc-budget-snap-prune}

Old generation artifacts (`gc/gen/<g>/`) are reclaimed once they age past a retention window
(`snap_pruned_through` cursor). Per round up to 64 generations are pruned wholesale:

| Step | Operation | Count | Notes |
|---|---|---|---|
| LIST `gc/gen/<g>/` prefix | `LIST` | ≤ 64 × ⌈objects/1000⌉ | One LIST per generation per prune burst (**code-derived**, `CasGc.cpp:1238`). Bounded by `kMaxPrunePerRound = 64`. |
| DELETE each listed artifact | `DELETE` | objects listed | Free on AWS. |
| GC-state CAS (snap_pruned_through) | `PUT` (`If-Match`) | included in recheck CAS | Folded into the same `casPut` as the completion seal (**code-derived**). |

**Previous design (removed):** an extra per-round LIST of the current-generation fold prefix to
clean up deposed-leader debris. This was eliminated in favor of the wholesale generation-retain
prune, saving one LIST per round on the common (single-leader) path (**code-derived** comment,
`CasGc.cpp:1242`).

### 3.6 GC round summary {#gc-budget-summary}

| Phase | PUTs | HEADs | GETs | LISTs | DELETEs |
|---|---|---|---|---|---|
| Fold (R1) | 2 | 0 | `S_changed` | ⌈`S`/1000⌉ | 0 |
| Retire (R2) | ⌈`C`/gc_shards⌉ + 1 | `C` | 0 | 0 | 0 |
| Fence (R3) | `S` + 1 | 0 | 0 | ⌈`S`/1000⌉ | 0 |
| Recheck (R4) | 2 | `S_fenced` | `S_fenced` | 0 | `C_confirmed` (free) |
| Snap prune | 0 | 0 | 0 | ≤ 64 × pages | `C_pruned` (free) |
| **Total (steady state, `S`=64, `C`=0)** | **~70** | **~64** | **~64** | **~3** | **0** |
| **Total (livelocked, `C`=10k)** | **~80** | **~10,064** | **~64** | **~3** | **0** |

The livelock scenario (P9 finding) shows why `C` must be bounded by pruning confirmed-deleted
candidates from the in-degree snapshot.

---

## 4. Reduction History {#reduction-history}

Each row is one optimization, the problem it solved, and its measured or modeled impact.

| ID | Optimization | Eliminated ops | Status | Source |
|---|---|---|---|---|
| **ETag fix (#1)** | `WriteBufferFromS3` captures the response ETag from `PutObject`/`CompleteMultipartUpload`; `nativeConditionalPut` uses it directly — no follow-up HEAD needed | ~73% of all HEADs (head-after-put was the single largest HEAD source, **measured**: `CasDbgMetaHit` 458 k ≈ `S3WriteRequestsCount` 340 k at 1:1 ratio) | **DONE** (commit `2a13fe5cc0f`) | `reports/2026-06-15-unattended-night-opcount-fixes.md` §#1; `CasObjectStorageBackend.cpp:127` |
| **P1 dedup cache** | LRU byte-bounded set of known-present content hashes; on a cache hit the blob PUT is replaced by a cheap HEAD-first check | Eliminates body-PUT + follow-up HEAD on 64% of blob creates (dedup hit rate, **measured**, P0 instrumentation) | **DONE** (B168; `CasStore.cpp:57`) | `specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` |
| **P2 adaptive HEAD-before-PUT** | For blobs ≥ `dedup_head_first_min_bytes` (default 1 MiB), send a HEAD first even on a cache miss — converts a body-PUT+HEAD (write tier) into a single HEAD (read tier) on a dedup hit | Downgrades large-blob dedup cost from write-tier PUT to read-tier HEAD; protects against broken-pipe storms on large-body 412s (B187) | **DONE** (B168; `CasBuild.cpp:119`) | `specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` |
| **root_shards widen** | Raised default `root_shards` from 8 to 64 | Spreads per-key CAS contention + per-key 64-permit I/O cap (RustFS 503 congestion) across 64 shards; GC retire-contention failure rate dropped from ~78% to ~21% (**measured**, soak #6 t=343s) | **DONE** (commit `d0194412d0b`) | `reports/2026-06-15-unattended-night-opcount-fixes.md` §#4 |
| **LIST-token skip** | Fold compares each shard's LIST-returned token against the sealed post-fence token; if equal, skips the shard body GET entirely | Reduces fold GETs to `S_changed` rather than `S` at every round; at steady state `S_changed ≪ S` | **DONE** (`CasGc.cpp:1430`) | `specs/2026-06-14-ca-reduce-s3-op-count-design.md` §2 |
| **Snap prune LIST elimination** | Removed per-round LIST of current-generation fold prefix for deposed-leader debris; wholesale generation-retain prune handles it lazily | Saves 1 `LIST` per round on the common single-leader path | **DONE** (`CasGc.cpp:1242`) | Code comment in `CasGc.cpp` |
| **P9 — GC 404-HEAD storm** | Prune confirmed-deleted/absent candidates from the fold in-degree snapshot so they are not re-HEADed on subsequent rounds | Removes the dominant HEAD source: **~90% of all read ops** on the GC leader = ~3.8 M / 23 min = ~367 M / day at `workers=2` (**measured**, P0 soak `CasBlobHeadMiss` + `CasTreeHeadMiss`); eliminates the livelock amplification | **TODO** (P9 proposal) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P9 |
| **P3 — replication relink** | Replica `DownloadPart` adopts the existing manifest refs rather than re-downloading each blob | Removes ~15 HEADs + 15 GETs per replicated part (**measured**, ~53 k parts / 95 min at `workers=2`) | **DESIRABLE** (P3 proposal) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P3 |
| **P4 batch publishes** | Commit multiple parts in one manifest CAS | Cuts `casPut` count + 35% CAS-conflict rate (**measured**) + per-shard write churn | **DESIRABLE** (B157/B149) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P4 |
| **P6 dirty-only fence** | Fence only shards with pending retires rather than all `S` shards | Removes `S - S_dirty` fence PUTs per round | **DESIRABLE** (P6 proposal) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P6 |

---

## 5. Measured vs Modeled {#measured-vs-modeled}

| Section | Confidence | Method |
|---|---|---|
| ETag-fix HEAD reduction (~73%) | **Measured** | `CasDbgMetaHit` / `S3WriteRequestsCount` ratio, instrumented soak (2026-06-15, seed 20260616) |
| Blob dedup hit rate (64%) | **Measured** | `CasBlobPutDedup` / (`CasBlobPutDedup` + `CasBlobPut`), P0 instrumentation run (~23 min) |
| GC HEAD storm magnitude | **Measured** | `CasBlobHeadMiss` + `CasTreeHeadMiss` ~3.81 M / 23 min (ch1 only); `S3HeadObject` 23.7 M / 3.2 h (run #5 aged pool) |
| 412 manifest-CAS conflict rate (35%) | **Measured** | `CasRootCasConflict` 49 k / `CasRootCas` 93 k, P0 soak |
| Per-part PUT count (novel path) | **Code-derived** | `CasBuild.cpp`: 1 PUT per blob + 1 tree + 1 precommit + 1 manifest + 1 commit = `f` + 4 |
| LIST-token skip savings | **Code-derived** | `computeDiscoverDecisions` logic, `CasGc.cpp:1430`; not yet separately instrumented |
| Replication HEAD+GET per part (~15+15) | **Measured** | `part_log` attribution, `DownloadPart` 53,796 events / ~95 min (P0 soak) |
| Mutable-files S3 cost (0 ops) | **Code-derived** | `CasRootShardCodec.cpp:121` inline encoding |
| P9 estimated savings (~90% of read ops) | **Measured basis** | `CasBlobHeadMiss`+`CasTreeHeadMiss` ≈ 90% of all `S3HeadObject`, P0 soak; fix not yet implemented |
| revalidateDeps per-promote HEAD count | **Modeled/uncertain** | `CasBuild.cpp:729`; exact count depends on dep count + cache state; not separately instrumented |

---

## 6. Cost Summary (Corrected, `workers=2`, no-chaos, ~3.3 h steady) {#cost-summary}

Extrapolated to per-day (×7.18) using the corrected P0 soak measurement:

| Bucket | Requests/day | $/day | Waste? |
|---|---|---|---|
| Read (GET + HEAD) | ~517 M | ~$207 | ~$147 of that = GC 404-HEAD storm (82% miss, P9 target) |
| Write (PUT + LIST) | ~73 M | ~$365 | ~$178 = 412-waste (dedup body-PUT + manifest-CAS retries) |
| DELETE | many | $0 | — |
| **Total (pre-P1/P2/P9)** | | **~$571/day** | **~57% is eliminable waste** |
| **Estimated floor after P1/P2/P9** | | **~$245/day** | Modeled/uncertain |

Figures are for the soak workload (`workers=2`, `root_shards=64`, no chaos, MergeTree with 18× merge
amplification). A production workload's per-request ratios depend heavily on dedup hit rate, merge
amplification factor, and GC round cadence.

---

## 7. References {#references}

- `docs/superpowers/specs/2026-06-08-s3-ops-cost-model.md` — pricing tier reference.
- `docs/superpowers/specs/2026-06-14-ca-reduce-s3-op-count-design.md` — Pillar A (incremental GC) + Pillar B (`resolveRef` decode cache) design.
- `docs/superpowers/specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` — P1/P2 dedup-cache + adaptive HEAD-before-PUT design.
- `docs/superpowers/reports/2026-06-15-ca-soak-opcount-and-rustfs-findings.md` — ground-fix research; instrumented soak A1b definitive attribution.
- `docs/superpowers/reports/2026-06-15-unattended-night-opcount-fixes.md` — #1 ETag fix + #4 root_shards widen + soak #6/#7 results.
- `docs/superpowers/reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` — P0–P9 proposals + corrected cost table.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` — write path; `putBlob`, `precommitAdd`, `promote`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` — `readShardDecoded`, `resolveRef`, `readManifest`, dedup-cache API.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — GC round: `fold`, `retire`, `fence`, `recheck`, `snapPruneOldGenerations`, `discoverUniverse`, `computeDiscoverDecisions`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.cpp` — `nativeConditionalPut` (ETag capture), `deleteExact`.
