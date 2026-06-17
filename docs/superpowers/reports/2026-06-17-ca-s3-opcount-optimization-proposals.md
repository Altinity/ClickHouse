# CA S3 op-count optimization — proposals (living doc)

**Status:** collecting · **Date:** 2026-06-17 · **Branch:** `cas-mergetree-poc`
**Context:** the B167 12h soak surfaced that the CA workload's S3 request cost is large and PUT-dominated. This doc collects optimization proposals, the measurements that motivate them, and their expected impact. Complements `docs/superpowers/specs/2026-06-08-s3-ops-cost-model.md`. Ties to B157/B149 (op-count) and B167f/B167g.

## Measured cost (workers=2, no-chaos, warmup→steady; both replicas; ~36 min window)

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

### P0 — Instrument the PUT/412 split (DO FIRST; cheap, safe)
Add ProfileEvents distinguishing: `If-None-Match` 412 (dedup miss) vs `If-Match` 412 (manifest CAS conflict) vs successful create vs overwrite; and attribute PUTs per part-op (or per CA call-site). Without this we are guessing which half of the 412s dominates. Small backend change, no behavior change. **Gates P1/P4 prioritization.**

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
