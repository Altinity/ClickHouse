# B140-dangle RECURRED under the fix — quick-soak diagnostic report

**Date:** 2026-06-18 · **Branch:** `cas-mergetree-poc` · **Severity:** HIGH (data loss, INV-NO-LOSS)
**Status:** the cursor-in-snap fix (commits `ee86bf7f4a0`…`d4e79c148a3`) is **INSUFFICIENT** — the dangle reproduced on the first validation soak. Root-level trigger not yet pinned (instrumentation in progress).
**Evidence:** `tmp/b140_quick_repro/` (captured `gc/snap` gens 240–246, `gc/state`, `roots/`, full server logs, decode output, token/part lifecycle, the throwaway decoder + `GcSnap` debug-accessor patch).

## TL;DR

A quick validation soak (fixed binary, **no chaos**, workers=2, ~20 min, seed 20260617) failed at the `gc_checkpoint` with `fsck dangling=10`. Decoding the captured generations proves: **GC stripped a LIVE merge part's tree (`…_1993_2679_102`) at gen 243 and deleted its 10 sole-owned blobs**; the part then 404'd on read → marked broken → detached. The fail-closed coherence guard never fired. So B140-dangle (GC deletes a blob a live part references) still happens; the fix closed the cursor-skip abstraction the TLA+ model/gtest exercised, but **not** the real soak producer (a root→tree strip of a still-needed part).

## The run

- `SEED=20260617 DURATION=20m WORKERS=2 NO_CHAOS=1 MAX_POOL_GB=40` via `run_keepalive.sh` (cluster left up).
- Binary: `build/programs/clickhouse` built with the full fix.
- Failure: at the `gc_checkpoint` stage (t+420–480 s), `fsck` reported `reachable≈7132, dangling=10, unreachable=0`. The entry-gate `fsck` timed out (B146, >180 s) so the pool-consistency gate was skipped — but the dangling is **persistent** (the 10 blobs are still absent now, long after the run; `mc stat` 404).

## Methodology correction (recorded honestly)

My first pass concluded "GC exonerated; this is a B144/B145 durability finding." **That was wrong** — it was built on host-side `grep`/`zcat` of `logs/ch1` which is `syslog:syslog 0640` (unreadable by my user), with `2>/dev/null` swallowing *Permission denied* as empty output. Re-run **via `docker exec`** (root inside the container) reversed every result:
- `CAGCDEL` count = **78,502** (not 0). All **10/10** dangling blobs show `CAGCDEL … outcome=Deleted` at **round 218, gen 243**.
- Guard-fire count = 0 (verified via `docker exec`).
**Lesson:** never read the bind-mounted `syslog`-owned logs from the host; always `docker exec`. All log evidence below is via `docker exec`.

## Token lifecycle (the precise correlation)

Each of the 10 blobs, on ch1 (the GC leader), exactly:
```
round 209 (13:32:26 UTC)  CAREUSE adopt   blob=<h> token=T      ← a build/merge on ch1 dedup-adopted the blob
round 218 (13:32:58 UTC)  CAGCDEL Deleted blob=<h> token=T  gen=243   ← ch1 GC deleted the SAME incarnation
```
Same node, adopt-before-delete, exact token match. 0 lines on ch2 (single GC leader). (`evidence/token_lifecycle_10.txt`, `evidence/cagcdel_10blobs.txt`.)

## Decode of the captured generations (the mechanism — CERTAIN)

`fsck --detail` → all 10 dangling are `reachable_from = …/detached/…/broken_20260618_1993_2679_102`. That ref's tree is `T_cur = a8db07933ce4678d121a9ad7e9f415a0`, which has 31 child edges **including all 10 blobs**. Per-generation (throwaway decoder over `tmp/b140_quick_repro/snap_<g>/0`):

| gen | the 10 blobs | `T_cur` (a8db0793) |
|----|----|----|
| 240–242 | `known=Y, inDeg=1` | `known=Y, expanded=Y`, edges→all 10 blobs |
| **243** | `known=Y, inDeg=0` | **`known=n, expanded=n, edges=0` (STRIPPED)** |
| 244–245 | `known=n` (forgotten post-delete) | gone |
| 246 | `known=Y, inDeg=1` (but physically deleted) | `known=Y, expanded=Y`, edges back |

Reading:
- **inDeg=1, not ≥2** ⇒ the 10 blobs are **sole-owned by `T_cur`** (not dedup-shared with another live tree). So this is **not** the originally-framed "shared blob with a second live parent" shape.
- gen 242→243: **`T_cur` (a live merge part's tree) is stripped** — its root edge went to 0 in the snap — so each blob drops `inDeg 1→0` and GC deletes it.
- gen 245→246: `T_cur` and the blobs **reappear** (`inDeg=1`) — this is the **detach** re-publishing the now-broken part (`republishCommittedPartIntoDetached`), re-pinning blobs that are already physically deleted ⇒ `fsck` walks the live (detached) ref → 404 → `dangling`.

`…_1993_2679_102` is a real merge (7 parts → it, level 102, committed; `evidence/part_1993_2679_lifecycle.txt`). It was then superseded by a `…_1993_2685_103` merge. So GC removed `1993_2679`'s root edge (a folded `Remove`/re-point of the superseded part) and reclaimed its blobs — but the part's blobs were still needed (the part was read after the delete → 404 → broken). 

## Why the fix did not prevent it

- The fix's **structural half** (cursor-in-snap) makes the *fold cursor* coherent with the snap edges, closing the **cursor-skip** abstraction the TLA+ model and `CasGcDangle` gtest exercise. But the real producer here is a **root→tree strip**: a `Remove`/re-point of a still-needed part's ref drives `removeRootEdge`→`stripTree`, reclaiming its blobs. Cursor coherence does not stop that.
- The **guard** (`assertSnapJournalCoherent`) only checks that a *latest-for-ref `Add`* at/below the cursor has its tree in `known`. When the latest record for the ref is a folded **`Remove`** (supersede), the guard correctly skips it — so it cannot catch "a part whose blobs are still needed had its ref removed and reclaimed."
- Net: the unit/TLA+ evidence remains valid for what it modeled; it simply did not model this producer.

## What is NOT the cause
- **Not** B144/B145 (store/write-path durability): GC deleted the blobs (`CAGCDEL outcome=Deleted`), they were not lost by RustFS nor written-but-unacked.
- **Not** the empty-file artifact: 10 *distinct* blob hashes (empty content would dedup to one).
- **Not** chaos/restart: the run had none.

## Candidate root-level triggers (to pin)
1. **GC-vs-merge-read race** — the superseding merge (`…1993_2685_103`) removed `1993_2679`'s ref (superseded); GC reclaimed its blobs **while that merge was still reading them** → the read 404'd → `1993_2679` marked broken/detached. The blobs of a superseded part are GC-eligible the instant its ref is removed, with no read-time pin.
2. **Spurious root-edge removal** of a live part (a fold/re-point defect emitting a `Remove`/displacement for a part that was not actually dropped).

Distinguishing them requires the `roots/` **journal at delete time** (the captured `roots/` is gen 309, journal trimmed). Next step: add `CASTRIP`/`CAROOTREM` audit logging (every `stripTree`/`removeRootEdge`/root-edge re-point with ref name + `at_version` + the driving journal record), rebuild, run a short repro, capture the `roots/` journal at the delete round, and pin the trigger — then re-open brainstorm→spec→TLA+ for a fix targeting the *real* producer.

## Evidence index (`tmp/b140_quick_repro/`)
- `snap_240..246/0` — captured `gc/snap` shards; `state` — `gc/state`; `roots/` — current manifests.
- `evidence/decode_gen_240_246.log` — the gen-by-gen decode (table above).
- `evidence/cagcdel_10blobs.txt`, `evidence/token_lifecycle_10.txt` — GC delete + adopt lifecycle.
- `evidence/part_1993_2679_lifecycle.txt` — merge/supersede/broken/detach server-log trace.
- `evidence/fsck_detail.txt` — `fsck --detail` with `reachable_from`.
- `evidence/ch1_serverlog/`, `evidence/ch2_serverlog/` — full server logs (gzipped).
- `evidence/gtest_cas_b140_decode_TEMP.cpp`, `evidence/gcsnap_debug_accessors.patch` — the throwaway decoder (reproducible).
