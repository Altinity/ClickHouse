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
| Promote: revalidate deps | `HEAD` | 0–`f+1` | **Superseded (EDGE-BEFORE-OBSERVE, `2026-07-09-cas-writer-gc-simplification`):** the bulk re-HEAD-of-all-deps at promote (the old `revalidateDeps`, no longer in code) is gone. Tokened deps are edge-protected and NOT re-checked at promote; only non-tokened deps get a HEAD + a per-hash `.meta` point-read GET (`Build::promote`, `CasBuild.cpp`). |

**Baseline total** (pre-optimizations, no dedup): 2·`f` + 4 `PUT`s + `f` + `D` `HEAD`s (ignoring
revalidate). At 64% dedup hit rate that is ≈ 3·`f` + 4 ops for a typical part.

### 1.2 Optimized write path (current, after ETag fix + P1/P2 dedup cache) {#write-budget-optimized}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Blob: cache HIT, HEAD-first present | `HEAD` + `GET` | `D_cache` | Cache-hit blobs: 1 cheap HEAD (replaces a body-PUT + HEAD) **plus a per-hash `.meta` point-read GET** (v3 freshness-meta, `observeAndAdmit` — condemned? → resurrect, else adopt). `D_cache` ≈ `D` after warmup (**code-derived**). The same `.meta` GET is added on the 412 dedup-follow-up path below. |
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

### 1.4 Heartbeat renewal {#write-budget-watermark}

Each writer renews **one merged heartbeat** per renewal interval: the mount lease and the build
watermark (`min_active`) ride a single `PUT` (the GC-ack `observed_gc_round` was removed in v3 — see
`04 §heartbeat-floor`), plus one `gc/state` `GET` to learn the current round (`03 §merged-heartbeat`). This is **−1 PUT** versus the
former two separate heartbeats (mount lease + standalone watermark object) and **+1 GET**. The
per-build `CasBuildPut` heartbeat was already removed earlier (B167c: redundant with the watermark).

### 1.5 Manifest `casPut` contention (write-tier 412s) {#write-budget-cas-contention}

Every `casPut` is an `If-Match` conditional PUT. On a conflict (concurrent writer or GC fence)
it 412s, re-reads the shard (1 `GET`), and retries. Measured conflict rate: **35%** of
`casPut` calls at `workers=2` (`CasRootCasConflict` 49 k / `CasRootCas` 93 k, P0 soak, at the
then-widened `root_shards=64` soak config). Each conflict costs one extra `PUT` (412d) + one
`GET` (**measured**).

**Superseded by the flat-combining shard-mutation queue (2026-07-03).** `casPut` calls on the
same `(namespace, shard)` are now grouped by a leader-caller batch builder (`Store::mutateShard`,
`CasStore.cpp:971`) into one read + one `casPut` per flush instead of one independent CAS loop
per mutation. This is now the primary contention lever, not shard fan-out: soak-validated ~2.3×
CAS-write compression with the intra-server conflict rate dropping from ~257k/h to ~11/h. The
shipped `root_shards` default was correspondingly changed to **32** (see §4), chosen to balance
per-shard journal body size and GC discovery cost rather than to spread CAS contention, since the
queue removes most of the contention argument for large shard counts (`CasStore.h:110-114`).

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

The regular round is a **single pass**: a heartbeat ack floor, one LIST discovery sweep, one fold
that runs the three-cursor merge (verify + graduate + condemn), pre-CAS deletes of previously-pending
entries, and one `gc/state` CAS (`04 §gc-round`). Let `N` = number of live/mounted servers, `S` =
active ref shards across all namespaces, `S_changed` = shards whose token advanced since last round,
`C` = newly-condemned zero-in-degree candidates this round, and `G` = entries graduated to physical
delete this round. The headline: the round is **O(delta) + O(servers)** requests plus **one** LIST
sweep — no O(universe) GET or PUT phase.

> **History (fence + recheck round, superseded 2026-07-02).** The old round ran four phases —
> fold (R1), retire (R2), fence (R3), recheck (R4) — and R3/R4 were each ~O(universe): the fence
> was one GET + one CAS-PUT on **every** present root shard (~2×O(universe) GET + O(universe)
> CAS-PUT per round), and the recheck re-read every fenced shard body plus a per-candidate
> `inDegreeInGeneration` whole-run re-read. At 100 000 tables × 8 root shards that was **~2.4 M
> requests (~$4.6) per round**, repeated every round forever. The ack-floor round replaces both
> phases with the causal floor + three-cursor merge; for the same slowly-changing pool it is
> **~2 000–3 000 requests (~$0.001–0.01) and seconds** of wall time. The subsections below describe
> the current round; the old per-phase tables are folded into this history note.

### 3.1 Heartbeat ack floor {#gc-budget-floor}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Heartbeat enumeration | `LIST` (`gc/server-roots/`) + `GET` | 1 LIST + `N` GETs | LIST the server roots, GET each mount body to classify live/terminated/expired for fence-outs (`computeHeartbeatFloor`; the `observed_gc_round` ack read was removed in v3). `N` is single-digit. |
| Expired-mount fence-out | `PUT` (`If-Match`) | rare (`≤` expired count) | A token-guarded `putOverwrite` setting `gc_fenced` on a lease-expired mount; only when a server actually died. |

**Per-round floor cost:** 1 LIST + `N` GETs (+ rare fence-out PUT). This is the round's only clock
and its only server-proportional cost.

### 3.2 Discovery + fold + three-cursor merge {#gc-budget-fold}

| Step | Operation | Count | Notes |
|---|---|---|---|
| Shard discovery | `LIST` (`cas/refs/`) | ⌈`S` / 1000⌉ | The round's single LIST sweep of the ref universe (**code-derived**, `discoverUniverse`). Returns shard tokens when the backend supports them. (Currently O(N²) over `roots/` on a real backend — see §backlog GC-DISCOVERY-LIST-QUADRATIC in `08-testing-and-soak.md`.) |
| LIST-token skip (accelerator) | 0 GETs for skipped shards | `S - S_changed` | If the listed token matches the sealed folded token, the shard body GET is skipped. Clamped-coverage shards (`classification = 4`) are never skipped (`04 §three-cursor-merge`). |
| Shard body read (changed) | `GET` | `S_changed` | Only shards whose token advanced. At steady state `S_changed ≪ S`. |
| Prior retired-run read | `GET` | `gc_shards` | One GET per gc-shard for the third merge cursor (the current retired list from `retired_refs`). |
| Newly-condemned candidate HEAD | `HEAD` | `C` | One HEAD per blob the merge newly condemns, to capture its exact token. Only **new** candidates — a previously-condemned entry stays in the retired list and is re-verified from the fold, not re-HEADed. |
| In-degree run write | `PUT` | `gc_shards` | New snapshot run per gc-shard (deterministic). |
| Retired-run write | `PUT` | `gc_shards` | New current retired list per gc-shard (always, even empty; observation-bearing). |
| Fold-seal write | `PUT` | 1 | One deterministic write-once fold seal per round. |

**Per-round fold cost (steady state):** ⌈`S`/1000⌉ LIST + (`S_changed` + `gc_shards`) GETs + `C`
HEADs + (2·`gc_shards` + 1) PUTs. The condemn-time HEAD is bounded by `C` (new candidates), not by
the cumulative deleted set — the P9 node-forgetting + retained retired list keep the old 404-HEAD
storm (§4) from recurring.

### 3.3 Deletes and the single CAS {#gc-budget-deletes}

| Step | Operation | Count | Notes |
|---|---|---|---|
| `deleteExact` (previously-pending graduates) | `DELETE` | `G` | Exact-token deletes for entries published `delete_pending` by the *previous* round (two-phase graduation, `04 §two-phase-graduation`). **DELETE is free on AWS.** |
| Manifest-body `deleteExact` | `DELETE` | per cleanup | Delete-after-adopted-decrements; free. |
| Outcome-log write | `PUT` | ⌈outcomes/shard⌉ | Observation-bearing outcome log per gc-shard (`putIfAbsent`-adopt). |
| Single `gc/state` CAS | `PUT` (`If-Match`) | 1 | The **only** CAS per round: publishes `round`, adopted generation/attempt, `retired_refs`, folded cursors, `snap_pruned_through`. |

**Per-round delete/CAS cost:** `G` (+ cleanup) DELETEs (free) + ⌈outcomes/shard⌉ + 1 PUTs. Physical
deletion lags condemnation by one pass (condemn → pending → delete), an intentional
two-phase-graduation property, not extra requests.

### 3.4 Snapshot-run reads and idle-round bytes {#gc-budget-bytes}

**Status: DONE** (T2 streaming reads + T0 reference-parent runs, 2026-07-02;
`specs/2026-07-02-cas-gc-snapshot-streaming-design.md`). Two properties of the per-gc-shard snapshot
run set the read/write byte budget (`04 §snapshot-run-reads`):

**Streaming reads — O(block) memory, fixed request profile.** Every run consumer opens a run with a
fixed request profile rather than reading the whole object into RAM:

| Step | Operation | Count | Notes |
|---|---|---|---|
| Object size | `HEAD` | 1 | `head(key)`. |
| Footer (tail suffix) | `GET` (ranged) | 1 | Ranged `get` of `min(object_size, kRunHardCapBlockSize + 64 KiB)` — carries the `footer_len` trailer + CRC'd block index. |
| Body stream | `GET` (stream) | 1 | `getStream` over the write-once body, positioned at the first block. |
| Exact footer (large runs only) | `GET` (ranged) | 0–1 | +1 exact-footer ranged `get` when the footer exceeds the tail probe. |

A linear scan is therefore **3 requests** (`head` + tail `get` + body `getStream`), or **4** for a
large-footer run. A `seek` costs **+1 ranged `get` per touched block**; the pure-linear fold path
never seeks. Resident memory is **O(block)** — the footer index plus one current block
(≤ `kRunHardCapBlockSize` = 1 MiB) — regardless of run size, replacing the former
3 × O(active edges) materialization (`get`-whole-then-`substr` + reader `full` copy + prior-edge
`std::vector`).

**Idle-round bytes — zero.** With reference-parent runs, an empty-delta gc-shard's new `fold_seal`
carries the parent generation's `RunRef` verbatim and the fold neither reads nor writes that shard's
run. A shard with an empty delta AND an empty retired list is pure ref-carry (zero run I/O); a fully
idle round (no journal changes, no retired entries) touches **zero** run objects — no GET, no PUT.
Combined with the ack-floor request profile, an idle round is one LIST sweep + `N` heartbeat GETs +
one `gc/state` CAS and nothing else. This removes the former per-round 2 × snapshot-bytes idle churn
(each pass previously re-read and rewrote a byte-identical successor run per shard).

**Remaining byte axis (T1, DESIRABLE).** A HOT pool still rewrites the full snapshot run per changed
gc-shard — O(active edges) **bytes** per round. The fix is delta-runs + periodic compaction (T1),
the NEXT spec; it builds on this spec's streaming reader, `getStream` seam, ranged `get`, and
seal-ref resolution unchanged. See `04-gc-protocol.md §snapshot-run-reads` and `ROADMAP.md`.

### 3.5 Snap prune (per round, amortized) {#gc-budget-snap-prune}

Old generation artifacts (`gc/gen/<g>/`) are reclaimed once they age past a retention window
(`snap_pruned_through` cursor). Per round up to 64 generations are pruned wholesale:

| Step | Operation | Count | Notes |
|---|---|---|---|
| LIST `gc/gen/<g>/` prefix | `LIST` | ≤ 64 × ⌈objects/1000⌉ | One LIST per generation per prune burst (**code-derived**, `CasGc.cpp:1238`). Bounded by `kMaxPrunePerRound = 64`. |
| DELETE each listed artifact | `DELETE` | objects listed | Free on AWS. |
| GC-state CAS (snap_pruned_through) | `PUT` (`If-Match`) | included in the round CAS | `snap_pruned_through` rides the single `gc/state` CAS (§3.3); the prune runs before it. |

**Previous design (removed):** an extra per-round LIST of the current-generation fold prefix to
clean up deposed-leader debris. This was eliminated in favor of the wholesale generation-retain
prune, saving one LIST per round on the common (single-leader) path (**code-derived** comment,
`CasGc.cpp:1242`).

### 3.6 GC round summary {#gc-budget-summary}

| Phase | PUTs | HEADs | GETs | LISTs | DELETEs |
|---|---|---|---|---|---|
| Heartbeat floor | rare fence-out | 0 | `N` | 1 | 0 |
| Discover + fold + merge | 2·`gc_shards` + 1 | `C` | `S_changed` + `gc_shards` | ⌈`S`/1000⌉ | 0 |
| Deletes + CAS | ⌈outcomes⌉ + 1 | 0 | 0 | 0 | `G` (+ cleanup, free) |
| Snap prune | 0 | 0 | 0 | ≤ 64 × pages | `C_pruned` (free) |
| **Total (steady state, `S`=64, `gc_shards`=8, `N`=2, `C`=0)** | **~20** | **~0** | **~12** | **~2** | **0** |
| **Total (churned, `C`=64, `G`=64)** | **~20** | **~64** | **~76** | **~2** | **~64 (free)** |

There is no livelock-amplified O(universe) phase to blow up any more: the fence and recheck are
gone, `C` counts only **newly** condemned blobs (not the cumulative deleted set, which P9 node
forgetting keeps out of the candidate set), and the round issues exactly one CAS. On a real backend
the LIST sweep is still O(N²) over `roots/` today — the outstanding discovery-LIST fix (§backlog) —
which is the round's remaining scalability item, not a per-phase O(universe) GET/PUT.

---

## 4. Reduction History {#reduction-history}

Each row is one optimization, the problem it solved, and its measured or modeled impact.

| ID | Optimization | Eliminated ops | Status | Source |
|---|---|---|---|---|
| **ETag fix (#1)** | `WriteBufferFromS3` captures the response ETag from `PutObject`/`CompleteMultipartUpload`; `nativeConditionalPut` uses it directly — no follow-up HEAD needed | ~73% of all HEADs (head-after-put was the single largest HEAD source, **measured**: `CasDbgMetaHit` 458 k ≈ `S3WriteRequestsCount` 340 k at 1:1 ratio) | **DONE** (commit `2a13fe5cc0f`) | `reports/2026-06-15-unattended-night-opcount-fixes.md` §#1; `CasObjectStorageBackend.cpp:127` |
| **P1 dedup cache** | LRU byte-bounded set of known-present content hashes; on a cache hit the blob PUT is replaced by a cheap HEAD-first check | Eliminates body-PUT + follow-up HEAD on 64% of blob creates (dedup hit rate, **measured**, P0 instrumentation) | **DONE** (B168; `CasStore.cpp:57`) | `specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` |
| **P2 adaptive HEAD-before-PUT** | For blobs ≥ `dedup_head_first_min_bytes` (default 1 MiB), send a HEAD first even on a cache miss — converts a body-PUT+HEAD (write tier) into a single HEAD (read tier) on a dedup hit | Downgrades large-blob dedup cost from write-tier PUT to read-tier HEAD; protects against broken-pipe storms on large-body 412s (B187) | **DONE** (B168; `CasBuild.cpp:119`) | `specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` |
| **root_shards widen (soak config)** | Raised the soak fanout `root_shards` from 8 to 64 | Spreads per-key CAS contention + per-key 64-permit I/O cap (RustFS 503 congestion) across 64 shards; GC retire-contention failure rate dropped from ~78% to ~21% (**measured**, soak #6 t=343s) | **DONE** (commit `d0194412d0b`) | `reports/2026-06-15-unattended-night-opcount-fixes.md` §#4 |
| **root_shards factory default fix** | Factory default for `root_shards` was silently 8 (mismatched `PoolConfig`'s already-32 default) — fixed to **32**, weighed against per-shard journal body size vs. GC discovery cost | Shipped default is now 32, not 64: with the flat-combining queue (below) removing most of the CAS-contention argument for large shard counts, 32 keeps a hot table's journal tail small without over-multiplying discovery `LIST` keys | **DONE** (commit `aa04ac9c3fb`) | `MetadataStorageFactory.cpp`, `CasStore.h:110-114` |
| **Flat-combining shard-mutation queue** | Group concurrent `casPut` mutations to the same `(namespace, shard)` into one leader-driven read + one `casPut` per flush (`MutationScope`, `Store::mutateShard`) | Soak-validated ~2.3× CAS-write compression; intra-server conflicts dropped from ~257k/h to ~11/h (**measured**) | **DONE** (spec `2026-07-03-cas-shard-mutation-queue`) | `CasStore.h`/`CasStore.cpp` (`runShardQueueLeader`); `03-writer-protocol.md §shard-mutation-queue` |
| **LIST-token skip** | Fold compares each shard's LIST-returned token against the sealed post-fence token; if equal, skips the shard body GET entirely | Reduces fold GETs to `S_changed` rather than `S` at every round; at steady state `S_changed ≪ S` | **DONE** (`CasGc.cpp:1430`) | `specs/2026-06-14-ca-reduce-s3-op-count-design.md` §2 |
| **Snap prune LIST elimination** | Removed per-round LIST of current-generation fold prefix for deposed-leader debris; wholesale generation-retain prune handles it lazily | Saves 1 `LIST` per round on the common single-leader path | **DONE** (`CasGc.cpp:1242`) | Code comment in `CasGc.cpp` |
| **P9 — GC 404-HEAD storm** | Prune confirmed-deleted/absent candidates from the fold in-degree snapshot (`GcSnap::forget`) so they are not re-HEADed on subsequent rounds | Removes the dominant HEAD source: **~90% of all read ops** on the GC leader = ~3.8 M / 23 min = ~367 M / day at `workers=2` (**measured**, P0 soak `CasBlobHeadMiss` + `CasTreeHeadMiss`); eliminates the livelock amplification; idle-round `HEAD`s went from ~46k to ~0 | **DONE** (TLA+-verified, soak-validated) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P9; `04-gc-protocol.md §node-pruning` |
| **P3 — replication relink** | Replica `DownloadPart` adopts the existing manifest refs rather than re-downloading each blob | Removes ~15 HEADs + 15 GETs per replicated part (**measured**, ~53 k parts / 95 min at `workers=2`) | **DESIRABLE** (P3 proposal) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P3 |
| **P4 batch publishes** | Commit multiple parts in one manifest CAS | Cuts `casPut` count + 35% CAS-conflict rate (**measured**) + per-shard write churn | **Largely subsumed** by the flat-combining shard-mutation queue (2026-07-03), which batches concurrent publishes into one `casPut` per shard; remaining delta is a deliberate multi-part single-commit API (B157/B149) | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P4 |
| **P6 dirty-only fence** | Fence only shards with pending retires rather than all `S` shards | Would have removed `S - S_dirty` fence PUTs per round | **SUPERSEDED** by the ack-floor round | `reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` §P6 |
| **Ack-floor GC round** | Replace the per-round all-shard fence + fold-through-fence recheck with a causal ack floor + one three-cursor merge; deletion gated by `condemn_round < min_ack` over live-server heartbeats | Removes both O(universe) phases: ~2×O(universe) GET + O(universe) CAS-PUT per round → O(delta)+O(servers) + 1 LIST sweep. ~2.4 M req/round → ~2–3 k req/round at 100k tables × 8 shards (**modeled**) | **DONE** (`cas-gc-ack-floor-fence`; soak validation TODO) | `specs/2026-07-02-cas-gc-ack-floor-fence-redesign.md` |
| **Merged heartbeat** | Fold the per-server build-watermark PUT into the mount-lease beat; add one `gc/state` GET | −1 PUT / +1 GET per writer beat | **DONE** (`cas-gc-ack-floor-fence`) | same spec, Task 6 |

---

## 5. Measured vs Modeled {#measured-vs-modeled}

| Section | Confidence | Method |
|---|---|---|
| ETag-fix HEAD reduction (~73%) | **Measured** | `CasDbgMetaHit` / `S3WriteRequestsCount` ratio, instrumented soak (2026-06-15, seed 20260616); `CasDbgMetaHit` was a throwaway diagnostic ProfileEvent, since removed (commit `87d97826711`) — the ratio is a historical measurement, not a live-queryable metric |
| Blob dedup hit rate (64%) | **Measured** | `CasBlobPutDedup` / (`CasBlobPutDedup` + `CasBlobPut`), P0 instrumentation run (~23 min) |
| GC HEAD storm magnitude | **Measured** | `CasBlobHeadMiss` + `CasTreeHeadMiss` ~3.81 M / 23 min (ch1 only); `S3HeadObject` 23.7 M / 3.2 h (run #5 aged pool) |
| 412 manifest-CAS conflict rate (35%) | **Measured** | `CasRootCasConflict` 49 k / `CasRootCas` 93 k, P0 soak |
| Per-part PUT count (novel path) | **Code-derived** | `CasBuild.cpp`: 1 PUT per blob + 1 tree + 1 precommit + 1 manifest + 1 commit = `f` + 4 |
| LIST-token skip savings | **Code-derived** | `computeDiscoverDecisions` logic, `CasGc.cpp:1430`; not yet separately instrumented |
| Replication HEAD+GET per part (~15+15) | **Measured** | `part_log` attribution, `DownloadPart` 53,796 events / ~95 min (P0 soak) |
| Mutable-files S3 cost (0 ops) | **Code-derived** | `CasRootShardCodec.cpp:121` inline encoding |
| P9 savings (~90% of read ops) | **Measured basis (historical)** | `CasBlobHeadMiss`+`CasManifestHeadMiss` (`CasTreeHeadMiss` was renamed) ≈ 90% of all `S3HeadObject`, P0 soak. NOTE: the specific `GcSnap::forget` node-pruning mechanism this row credited was later REMOVED — superseded by the source-edge-set GC model (no persisted node registry to forget from); see `04-gc-protocol.md` |
| revalidateDeps per-promote HEAD count | **Removed** | `revalidateDeps` no longer exists (EDGE-BEFORE-OBSERVE, `2026-07-09-cas-writer-gc-simplification`): tokened deps are edge-protected; only non-tokened deps get a HEAD + `.meta` GET at promote |

---

## 6. Cost Summary (Corrected, `workers=2`, no-chaos, ~3.3 h steady) {#cost-summary}

Extrapolated to per-day (×7.18) using the corrected P0 soak measurement:

| Bucket | Requests/day | $/day | Waste? |
|---|---|---|---|
| Read (GET + HEAD) | ~517 M | ~$207 | ~$147 of that = GC 404-HEAD storm (82% miss, P9 target) |
| Write (PUT + LIST) | ~73 M | ~$365 | ~$178 = 412-waste (dedup body-PUT + manifest-CAS retries) |
| DELETE | many | $0 | — |
| **Total (pre-P1/P2/P9)** | | **~$571/day** | **~57% is eliminable waste** |
| **Estimated floor after P1/P2/P9** | | **~$245/day** | Modeled/uncertain; P1/P2/P9 have since all shipped (see §4) but this table has not been re-run against the fixed pipeline |

Figures are for the soak workload at the time of measurement (`workers=2`, `root_shards=64` — the
then-widened soak fanout, not today's shipped default of 32; see §4), no chaos, MergeTree with 18×
merge amplification. A production workload's per-request ratios depend heavily on dedup hit rate,
merge amplification factor, and GC round cadence.

### 6.1 Post-fix live measurement, per operation kind (2026-07-19, `workers=6`, WITH chaos) {#cost-summary-per-kind}

Re-run of §6 against the post-P1/P2/P9 pipeline, from the `cas-gc-rebuild` 5h soak (v11), ~2h45m
in, `workers=6` insert/mutate workers + 4 SELECT workers, chaos stage active (kill/restart/pause
faults firing). **Measured**, per operation *unit* rather than extrapolated per-day, using a
different attribution method from §5/§6: grouping `ProfileEvents` in `system.query_log` (by
`query_kind`), `system.part_log` (by `event_type`), and
`system.content_addressed_garbage_collection_log` (`outcome='Success'` rounds only) — see
`analyzing-cas-health` skill for the exact queries and the `blob_storage_log`
`query_id` decoding technique used to attribute `MergeParts`/`MutatePart` PUTs (`part_log`'s own
`ProfileEvents` never captures a nonzero PUT for those event types; the upload count comes from
matching `blob_storage_log`'s synthetic `<table_uuid>::<part_name>` `query_id` against `part_log`
by part name instead).

| Unit | Count | PUT/unit | GET/unit | HEAD/unit | DELETE/unit | $/unit |
|---|---:|---:|---:|---:|---:|---:|
| `INSERT` | 44,753 | 13.3 | 15.8 | 30.7 | 0 | $0.000085 |
| `MergeParts` | 16,530 | 21.0 | 23.4 | 40.6 | 0 | $0.000130 |
| `MutatePart` | 4,964 | 10.6 | 14.0 | 33.3 | 0 | $0.000072 |
| `DownloadPart` (replication fetch) | 44,157 | 0 | 4.0 | 5.0 | 0 | $0.0000036 |
| GC round (`outcome='Success'`) | 416 | ~0 | 5,305 | 12,812 | 4,156 | $0.00725 |

A GC round costs **~55–100× more per unit** than a single insert/merge/mutation — not from PUT (GC
issues almost none), but from GET+HEAD volume in the retire/recheck verification pipeline (matches
§3.2/§3.6's modeled GC-round shape). `DownloadPart` is the cheapest unit by far (~24× cheaper than
an `INSERT`) — confirms §2.4's CAS-relink model: a replication fetch is a lightweight
metadata-level operation reading an already-shared blob directly from S3, not a full byte transfer
between replicas.

Aggregate request cost for the measured window (request pricing only, no storage/data-transfer):

| Source | PUT | GET | HEAD | DELETE | $ |
|---|---:|---:|---:|---:|---:|
| `INSERT` | 594,236 | 707,851 | 1,371,247 | 0 | $3.80 |
| `MergeParts` | 346,309 | 387,304 | 671,832 | 0 | $2.16 |
| `MutatePart` | 52,683 | 69,363 | 165,315 | 0 | $0.36 |
| `DownloadPart` | 0 | 176,477 | 220,600 | 0 | $0.16 |
| GC rounds (416) | 0 | 2,206,977 | 5,330,042 | 1,728,828 | $3.02 |
| CAS-internal housekeeping (`.meta` blob sidecars, `cas/manifests`, `cas/refs`, `gc/gen` attempt/outcome records — PUT only, no query/part attribution by design) | 1,128,865 | — | — | — | $5.64 |
| **Total (request cost only)** | | | | | **≈$15.13** |

The CAS-internal housekeeping bucket is the single largest line item, larger than `INSERT` itself
— dominated by the per-blob `.meta` sidecar object (one small PUT written alongside, not instead
of, every real content blob's PUT). This is a distinct cost driver from anything in §1–§3's
per-code-path models above and was not previously broken out on its own; see
`utils/ca-soak/scenarios/BACKLOG.md`'s "S3 PUT budget" entry (2026-07-19) for the full attribution
methodology and the query_id-decoding technique that resolved it.

---

## 7. References {#references}

- `docs/superpowers/specs/2026-06-08-s3-ops-cost-model.md` — pricing tier reference.
- `docs/superpowers/specs/2026-06-14-ca-reduce-s3-op-count-design.md` — Pillar A (incremental GC) + Pillar B (`resolveRef` decode cache) design.
- `docs/superpowers/specs/2026-06-20-ca-dedup-cache-head-before-put-design.md` — P1/P2 dedup-cache + adaptive HEAD-before-PUT design.
- `docs/superpowers/reports/2026-06-15-ca-soak-opcount-and-rustfs-findings.md` — ground-fix research; instrumented soak A1b definitive attribution.
- `docs/superpowers/reports/2026-06-15-unattended-night-opcount-fixes.md` — #1 ETag fix + #4 root_shards widen + soak #6/#7 results.
- `docs/superpowers/reports/2026-06-17-ca-s3-opcount-optimization-proposals.md` — P0–P9 proposals + corrected cost table.
- `utils/ca-soak/scenarios/BACKLOG.md`, "RESOLVED — S3 PUT budget..." entry (2026-07-19) — the
  `query_id`-decoding / `part_name`-matching methodology behind §6.1.
- `~/.claude/skills/analyzing-cas-health/SKILL.md` — the executable checklist this section's
  measurement technique was drawn from (step 6/8/10's exact queries).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasBuild.cpp` — write path; `putBlob`, `precommitAdd`, `promote`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasStore.cpp` — `readShardDecoded`, `resolveRef`, `readManifest`, dedup-cache API.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` — GC round: `runRegularRound` (heartbeat floor → fold + three-cursor merge → pre-CAS deletes → single CAS), `snapPruneOldGenerations`, `discoverUniverse`, `computeDiscoverDecisions`. `computeHeartbeatFloor` in `CasServerRoot.cpp`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp` — `nativeConditionalPut` (ETag capture), `deleteExact`.

## Shard-mutation queue effect (2026-07-03) {#shard-queue-budget}

Ref-shard `casPut` traffic is grouped per `(namespace, shard)` by the flat-combining queue
(`03-writer-protocol.md §shard-mutation-queue`): concurrent part publishes/drops on one shard cost
ONE read + ONE `casPut` per batch instead of one CAS loop per mutation, and intra-server CAS
conflicts (measured 40-92% of attempts under load, each re-reading the full shard body) are
structurally eliminated — the only remaining conflict source is the other replica's GC trim.
