# B140-dangle: root trigger PINNED via `system.content_addressed_log`

**Date:** 2026-06-18 (12h chaos soak, SEED 20260618, B170 event log enabled)
**Status:** trigger reconstructed from the event log alone — no log-greps. The fold-bug fix
(cursor-in-snap + coherence guard) is **confirmed working**; the dangle is produced by a
**different** mechanism: a build's protection lease lapsing between *adopt* and *publish*.

## The symptom

`system.errors` on ch1: `CORRUPTED_DATA` ×1 —
> Failed to load file `serialization.json` of data part `20260618_28118_28624_64` … reading key
> `soak_pool/blobs/d3/d3e1ba56f5b80ca590a77e50594ae95b` … The specified key does not exist (499).
Raised by `ReplicatedMergeTreePartCheckThread::run` → `checkDataPart`. Part `28118_28624_64`
maps to tree `3715345a4150`; the missing blob is `d3e1ba56…` (size 747).

## The blob's full lifecycle (from `content_addressed_log WHERE object_hash='d3e1ba56…'`)

| time | round | event | meaning |
|---|---|---|---|
| 18:39:16 | 252 | `indeg_zero` (prev_indeg=1, dropped_by `strip(3715345a4150)`) | tree stripped → blob in-degree 0 |
| 18:39:23 | 253 | `gc_retire_observe present` → `gc_retire_decision condemn` → `blob_retire` | GC condemns it |
| 18:39:28 | 253 | `blob_reuse_resurrect` ("observed token condemned; resurrect fresh tag") | a build re-adopts the condemned blob |
| 18:39:29 | 253 | `blob_delete **replaced**` + `gc_recheck_verdict replaced` ("token displaced (412); fresh incarnation pins it") | **GC delete correctly REJECTED — incarnation fix works** |
| 18:39:31 | 253 | `blob_reuse_adopt` ("observed token not condemned; adopted the live incarnation") | build adopts the live blob |
| 18:39:33→18:40:34 | 254–264 | 11× `gc_retire_decision **skipped**` ("protectedByLiveBuild: owner alive, seq ≥ min_active") | GC defers — a live build protects it |
| **18:40:34** | 265 | `gc_retire_decision **condemn**` ("present and known and inDeg=0 and **not protected**") | **protection LAPSED while inDeg still 0** |
| 18:40:38 | 265 | `blob_delete **deleted**` (token_outcome=deleted) + `gc_recheck_verdict deleted` | **blob deleted for real** |
| 18:40:39 | 265 | `blob_forget forgotten` | pruned from known (P9) |
| **18:41:07** | — | `ref_publish` / `build_publish` tree `3715345a4150` | **part republished, referencing the now-deleted blob** |
| 18:41:08 | — | `CORRUPTED_DATA` (part-check thread reads the part) | dangling reference observed |

## Root cause

This is **not** a GC fold/reachability bug — at every GC decision the blob genuinely had
in-degree 0, the incarnation/412 mechanism correctly spared it during the active resurrect, and
`protectedByLiveBuild` correctly deferred for 11 rounds. The dangle is a **build-protection lease
expiry**:

1. A build *adopts* blob `d3e1ba56` (18:39:31) intending to publish a tree that references it.
2. `protectedByLiveBuild` protects the blob while the build's seq ≥ `min_active` (rounds 254–264).
3. **The protection lapses at round 265 (18:40:34) while the build is still in-flight** — the blob
   still has in-degree 0 (no tree published yet), so GC condemns and **deletes** it (18:40:38).
4. The build finally *publishes* tree `3715345a4150` at 18:41:07 — **33 s after its protection
   lapsed and 29 s after the blob was deleted** → the published tree dangles.

So the protection window (adopt→publish) was **too short for a slow build**. Builds are slow in
this run because of an `S3_ERROR` storm: ~800 k `PreconditionFailed` (HTTP 412) — the CAS
conditional-PUT/exact-token mechanism's *normal* conflict detection under a dedup-heavy workload
(`blob_reuse_adopt` 672 k), but the volume inflates build latency past the lease.

## Sub-question resolved: protection is bound to the WRONG build (adopt doesn't re-stamp owner)

Code reading settles which of the two mechanisms it is. `Gc::protectedByLiveBuild`
(CasGc.cpp:1733) protects a blob iff its `cas_owner` metadata (`<server>:<epoch>:<build_seq>`)
satisfies `owner_seq >= min_active` (the smallest in-flight build seq, `Store::minActive`,
CasStore.cpp:179). **`reuseBlob`'s adopt arm (CasBuild.cpp:322–334) moves no bytes and writes no
metadata** ("adopted the live incarnation (no bytes moved)") — only the fresh-upload path
(CasBuild.cpp:153) and `resurrect` (fresh-tag body PUT, :360) stamp `cas_owner`. So **adoption never
re-stamps ownership.**

Reconciled with the timeline: at 18:39:28 a *resurrecting* build (seq `S_res`) wrote a fresh
incarnation → `cas_owner = S_res`. At 18:39:31 build `bc6537a6` *adopted* it without re-stamping →
`cas_owner` stays `S_res`. Protection held (rounds 254–264) while `S_res >= min_active`; at round 265
(~18:40:34) **`S_res` retired**, `min_active` rose past it → protection flipped off → condemn+delete
(18:40:38) — even though `bc6537a6` was still in-flight and published the referencing tree at
18:41:07. `bc6537a6`'s own seq never left the active set (it published at 18:41:07; `retireBuildSeq`
fires only on publish/abandon/dtor, CasBuild.cpp:85–87), so this is **NOT** an adopting-build
heartbeat lapse — it is `min_active` overtaking the blob's *stale creator-owner* under a still-live
adopter.

This is wider than the reuse-decision→HEAD race the code already anticipates at CasBuild.cpp:238
(a few ms); the dangling window here is the full **adopt→publish** span (96 s, stretched by the S3
`PreconditionFailed` storm).

### Fix direction (to be brainstormed → spec → TLA+ → plan)

- **(a) re-stamp on adopt:** rewrite the adopted blob's `cas_owner` to the adopting build's triple,
  token-conditional. Correct but adds one S3 metadata op per adopt — ×672 k adopts in 20 min here.
- **(b) protect by declared dep set (preferred):** a build already records its dep set; have GC
  protect any object an in-flight build (seq ≥ `min_active`) has *declared as a dep*, independent of
  who stamped `cas_owner`. Cheaper (no extra per-adopt S3 op) and more correct — protection tracks
  the actual referencing build, not the byte-writer.

## Corroborating signals (same run)

- `ABORTED` ×13: `Build::resurrect: condemned incarnation … deleted by GC between HEAD and GET;
  retry` — the resurrect-vs-delete TOCTOU **correctly caught and retried** (fail-closed). This is
  the safe sibling of the bug above; the dangling path is the one where the build had *already
  adopted* and lost protection rather than racing a single HEAD→GET.
- Replica row gap (ch1 < ch2) + `NO_REPLICA_HAS_PART` + part-check thread: the broken part is
  expected to **self-heal via replication** (re-fetch from ch2), which is why it surfaced as a
  part-check `CORRUPTED_DATA` rather than a query failure. On a single replica (or if both adopt
  the same lease-expiry) this is durable data loss.
- Event-log accounting reconciles exactly: `retire_observe present` (392169) =
  `condemn`+`skipped`; `condemn` = `blob_retire`+`tree_retire`; `recheck deleted` (383352) =
  `blob_delete`+`tree_delete deleted` = `blob_forget`. No leak in the accounting.
