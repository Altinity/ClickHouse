# CA S3 op-count optimization — proposals (living doc)

**Status:** collecting · **Date:** 2026-06-17 · **Branch:** `cas-mergetree-poc`
**Context:** the B167 12h soak surfaced that the CA workload's S3 request cost is large and PUT-dominated. This doc collects optimization proposals, the measurements that motivate them, and their expected impact. Complements `docs/superpowers/specs/2026-06-08-s3-ops-cost-model.md`. Ties to B157/B149 (op-count) and B167f/B167g.

## CORRECTED cost + waste breakdown (workers=2, no-chaos, ~3.3 h steady sample, both replicas)

The 36-min figure below over-extrapolated the warmup insert burst. Clean totals at ~12,040 s (per-day ×7.18):

| | ch1 (GC leader) | ch2 | combined/day | $/day |
|---|---|---|---|---|
| Read req (GET+HEAD) | 62.5 M | 9.6 M | ~517 M | $207 |
| → **404 HEADs (`read_404` ≈ `CasDbgMetaMiss`)** | **51.2 M (82%)** | ~0 | **~367 M** | **$147 (wasted)** |
| Write req (PUT+LIST) | 5.79 M | 4.33 M | ~73 M | $365 |
| → **412 (dedup `If-None-Match` + manifest `If-Match`)** | 2.45 M (42%) | 2.51 M (58%) | **~36 M** | **$178 (wasted)** |
| LIST | 20,932 | 20,974 | ~0.3 M | (in write tier) |
| **Total** | | | | **≈ $571/day (~$17.4k/mo)** |

**~57% of the bill (~$325/day) is pure op-count waste:** the GC 404-HEAD storm (read tier) + 412 CAS/dedup retries (write tier). Optimized floor for this workload ≈ $245/day.

**Key correction:** `CasDbgMetaMiss` (50.8 M) ≈ `S3ReadRequestsErrors` (50.8 M) — these 404 metadata lookups **are real, billed S3 HEADs** (not negative-cached as first guessed), and **88% of the GC leader's 58 M HEADs are 404 misses**. The storm is entirely on the GC leader (ch1: 5 lease acquires; ch2: 0, `read_404`≈0). Merge write-amplification is also large: `MergedRows` 1.66 B from 92 M written (~18×) — inherent MergeTree, but it drives the CA blob churn.

## Measured cost (workers=2, no-chaos, warmup→steady; both replicas; ~36 min window — SUPERSEDED by the corrected table above)

AWS S3 Standard (us-east-1): PUT/LIST/COPY = $0.005/1k, GET/HEAD/other = $0.0004/1k, DELETE free, same-region transfer free.

| Class | requests/day (extrapolated) | $/day |
|---|---|---|
| Read (GET+HEAD) | ~430 M | ~$172 |
| Write (PUT+LIST) | ~166 M | ~$832 |
| DELETE | many | $0 |
| Storage (~10 GB, GC-bounded) | — | ~$0.10 |
| **Total** | | **≈ $1,005 / day (~$30.6k/mo)** |

- ~83% of cost is the PUT/write tier (12.5× the read tier per request).
- **~half the write requests (~2.16M / 36 min) are 412 PreconditionFailed** — handled silently (not logged as errors) but billed. Two sources: dedup-hit `If-None-Match:*` creates, and `If-Match` manifest `casPut` conflicts (writer-vs-GC-fence, writer-vs-writer).

## Measured op attribution — `system.part_log` (table `ca_stress`, ~95 min into the run)

Read-side only (part-op threads do **not** attribute `S3PutObject` — PUTs ride the async write/commit + background GC paths):

| event_type | ops | rows | HEAD | GET | LIST | HEAD/op |
|---|---|---|---|---|---|---|
| DownloadPart (replication fetch) | 53,796 | 18.4 M | 819,050 | 826,513 | 142 | ~15 |
| NewPart (insert) | 54,053 | 5.4 M | 1,513,650 | 95,161 | 82 | ~28 |
| MergeParts | 23,914 | 648 M | 860,567 | 359,279 | 145 | ~36 |
| MutatePart | 643 | 470 M | 27,398 | 7,467 | **19,670** | ~43 |
| RemovePart | 131,947 | 1.18 B | 0 | 0 | 0 | 0 |

Not in `part_log` (background): the ~7 M GC observe-scan HEADs, the manifest `casPut` PUT churn, GC retire/fence/outcome PUTs, per-server watermark renewals.

## Proposals (prioritized; measure → highest-leverage first)

### P0 — Instrument PUTs/412s at the CA call-sites (DO FIRST; cheap, safe)
Add dedicated ProfileEvents at the CA backend call-sites — `putBlob` / `putTree` / `casPut` (manifest) / `resurrect` / GC-retire / GC-fence — counting: successful create vs overwrite, `If-None-Match` 412 (dedup miss) vs `If-Match` 412 (manifest CAS conflict). This attributes the write tier by **CA operation type**, regardless of which thread runs the upload.
**Why call-site (not part_log) attribution:** `part_log` shows `put=0` for every op because S3 PUTs run on the background object-storage write pool (`threadpool_writer_pool_size=100`) and aren't snapshotted into the part-op's ProfileEvents. Measured 2026-06-17: **inserts** *do* attribute via `query_log` (~34 PUT + ~27 HEAD per insert — blobs+tree+manifest + dedup HEADs), but **merge/mutation PUTs are invisible** to both `query_log` (the Optimize/Alter trigger query finishes before the bg task) and `part_log`. Disabling the write pool (`threadpool_writer_pool_size`) would surface them in `part_log` but needs a server restart, only fixes attribution (not cost), and may not fully work (single-part finalize still uses the pool) — so CA-call-site counters are the clean answer. **Gates P1/P4 prioritization.**

### P9 — Eliminate the GC 404-HEAD storm (largest single op bucket; ~$147/day, ~367 M ops/day, all on the leader)
The GC leader's R2 observe loop HEADs every zero-in-degree candidate to capture its token before condemning — but **82% of those HEADs are 404s** (`read_404`≈`CasDbgMetaMiss`≈51 M on ch1, ~0 on ch2). The candidate set (`zeroInDegreeKnown()`) is re-HEADing objects **already deleted in prior rounds** (or deleted by the peer) that were never pruned from the in-degree snapshot, so the same dead keys are re-probed every round (~225 rounds). Fix: **prune confirmed-deleted/absent objects from the snap so they are not re-observed** (a deleted/absent key should leave the candidate set), and/or negative-cache absent keys within a GC pass, and/or skip re-HEAD of keys whose retire outcome was already `deleted`/`absent`. This is the same family as the B160-era 404-HEAD storm but on the leader's own observe loop. **Expected: removes ~367 M HEADs/day** — the single biggest op-count item; read-tier $ is modest (~$147/day) but the CPU/latency/RustFS-load relief is large and it cleans up the dominant counter.

### P1 — Local "known-present" dedup cache (eliminates the op entirely)
Bounded in-memory set of content hashes the server has confirmed present. On create, check first; on a hit, skip the conditional PUT **and** the follow-up HEAD → **0 S3 ops** for re-referenced content. Safe: content is immutable; a stale hit (GC-deleted since) is caught by the publish-gate re-validation → re-upload. Attacks the dedup-412 source (NewPart ~28 HEAD/op + the paired 412 PUT, MergeParts ~36 HEAD/op). **Expected: large cut to both the dedup-412 PUTs and the insert/merge HEADs.**

### P2 — HEAD-before-conditional-PUT, adaptive (trade write-412 for read-HEAD, 12.5× cheaper)
For cache misses, HEAD first; PUT only on 404. Converts a dedup hit from `412-PUT + HEAD` (current) into a single cheap HEAD. Make the PUT-first vs HEAD-first choice adaptive on the observed dedup-hit rate (already HEAD-first for large blobs). **Expected: moves the dedup-412 volume from write tier to read tier.**

### P3 — CA fetch-by-relink for replication `DownloadPart` (biggest read-side win)
`DownloadPart` is ~as frequent as `NewPart` (each inserted part is fetched by the peer replica) and does ~15 HEAD + ~15 GET **per part**. In a content-addressed *shared* pool the blobs already exist in S3 — a replica fetch should **adopt the manifest refs (relink), not re-HEAD/re-GET each file**. Audit whether the fetch path is CA-aware relink or the generic MergeTree download; eliminate per-file verification reads where the content hash already guarantees identity. **Expected: removes ~1.6 M reads (the DownloadPart HEAD+GET) — the largest single read bucket.**

### P4 — Batch publishes / fewer manifest `casPut` (B157/B149 — structural PUT win)
Commit multiple parts in one manifest CAS instead of one-per-part. Cuts the PUT count **and** the `casPut` contention (fewer writer-vs-writer / writer-vs-fence 412s) **and** the roots/ on-disk version churn (B167g). Highest structural lever on the write tier.

### P5 — Widen root-shard fanout `N` (existing "root_shards widen" item)
More shards → lower per-shard CAS contention → fewer `If-Match` 412s. Cheap config lever.

### P6 — Fence only dirty shards / tune GC cadence
R3 fences **every** shard each round; fence only shards with pending retires → fewer fence PUTs and fewer writer-vs-fence 412s. Tuning GC round frequency trades reclaim latency for fewer fences. (B160 productive-GC raised the fence rate.)

### P7 — Investigate `MutatePart` LISTs (~30 LIST/mutation, write-tier priced)
643 mutations issued 19,670 LISTs — the dominant LIST source. Determine why the mutation path LISTs (source-part enumeration? CA metadata scan?) and eliminate if a ref-walk/manifest read suffices. **Expected: removes the bulk of the (write-tier) LIST spend.**

### P8 — Coalesce CAS retries
On a shard `casPut` conflict, re-read once and merge pending publishes for that shard into a single retry instead of each retry being a fresh billed PUT.

## Expected impact (rough)
- P1+P2 target the **dedup-412 half** → request cost ~$1,005 → ~$500–600/day.
- P3 removes the **largest read bucket** (replication fetch) — modest $ (read tier) but big op-count + RustFS-load relief.
- P4+P5+P6+P8 attack the **`casPut`/manifest-churn half** (also the B167f/B167g driver) — compounding write-tier savings.
- P7 removes the write-tier LIST spend from mutations.

## Notes
- DELETE being free is load-bearing for the design economics (GC delete path is cost-free; all cost is on create/publish).
- All proposals are op-count work, independent of the watermark (B167), which remains clean in the soak.
- Next step: implement P0 (instrumentation), re-measure on a soak, then sequence P1/P3/P4 by measured dominance.
