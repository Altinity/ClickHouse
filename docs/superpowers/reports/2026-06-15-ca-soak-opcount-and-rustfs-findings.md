# CA soak — op-count + RustFS-interaction findings (ground-fix research)

**Date:** 2026-06-15. **Context:** run #5 (12h aggressive soak, `root_shards=8`, 6 workers, single-disk RustFS `1.0.0-beta.8`, scanner+heal disabled). Evidence gathered by direct measurement (system.events, on-disk inspection, boto3 LIST probe, gdb, tcpdump, an isolated RustFS experiment). This doc is the input to a brainstorming → spec → plan cycle for the ground fix. It deliberately separates **backend-agnostic CA op-count problems** from **RustFS-specific interactions** and from **test-harness config**.

## TL;DR

The CA design's manifest/blob access pattern is **op-count-heavy in two backend-agnostic ways**, and that pattern **collides pathologically with RustFS** (and with our own scanner-off test config). Priority order for a ground fix:

1. **Blob/tree existence-HEAD storm** (biggest, backend-agnostic). ~23.7M HEADs in 3.2h, ~80% → 404, ~100:1 HEAD:write. Pillar B optimized only shard/`roots` reads; blob+tree existence probes are un-addressed.
2. **Manifest-overwrite op-count** (backend-agnostic write cost; RustFS-specific *leak*). Every publish rewrites a `roots/` shard via `casPut`; on `root_shards=8` that is ~15k overwrites per key. On RustFS this leaks a dead data dir per overwrite (74 GB / 96% of pool); on real S3 it would not leak but the PUT/contention cost remains.
3. **`root_shards=8` fanout** concentrates all of the above on 8 keys → per-key 64-permit congestion (503s) + per-key orphan pileup (un-listable `roots/`). Cheap to widen.

## Evidence

### A. Op-count storm (system.events, ch1, uptime 11,525 s ≈ 3.2 h)
- `S3HeadObject` = **23.7M** (~2,055/s); `S3ReadRequestsErrors` = **20.6M** (~80% of all reads error — almost all 404).
- `S3PutObject` = 1.75M; `S3GetObject` = 1.96M; `S3ListObjects` = only 11.7k.
- tcpdump (90 s, run #5 live): `HEAD /blobs` **304,212**, `HEAD /trees` **131,865**, `HEAD /roots` 4,326. So the HEAD storm is **blobs (~3,400/s) + trees (~1,465/s)**; `roots` HEADs are low (Pillar B decode cache working). HEAD:PUT ≈ **116:1 on blobs**, **~1,500:1 on trees**. HTTP codes in the window: 431k × 404, 15k × 200 → the existence probes mostly MISS.
- **Interpretation:** the dominant cost is content-addressed existence/dedup/reachability HEAD verification on blobs+trees, mostly for absent objects. This loads ANY backend and is the #1 fix target.
- **Caller traced:** the HEAD engine is `Build::observeAndAdmit` → `store->backend().head(key)` (`CasBuild.cpp:243`), invoked on every blob/tree dep from `putBlob` (160/192), `putTree`/tree-admit (379/448/501/548), and the **adopt/merge/reuse** paths (307/596/648). The ~100:1 HEAD:PUT amplification is the **adopt/merge/reuse** case: it HEAD-verifies every dep of every adopted part but uploads nothing (the caller holds no body), so merges (which adopt many parts) re-HEAD large dep sets. `putBlob`'s pre-upload dedup HEAD is ~1:1 with PUT and is the minority.
- **KEY DESIGN QUESTION (not yet resolved — needs instrumenting the `observeAndAdmit` callers):** why are ~80% of these dep HEADs **404**? Deps referenced by committed parts SHOULD be present (200). Either (a) they are benign — inline/in-manifest/tokened deps that legitimately have no standalone object, so the HEAD is wasted and should be skipped; or (b) something is re-verifying genuinely-absent deps. (a) → the fix is "don't HEAD deps that the manifest already proves reachable / are inline"; (b) → there's a deeper reachability concern. This classification is the first design-phase task.

### B. Manifest-overwrite leak (on-disk, RustFS)
- `roots/` = **74 GB for 18 logical keys**; each `roots/<table>/<ns>/<shard>` object dir holds **~14,900 orphan `<uuid>/part.1` dirs** (one per overwrite), `xl.meta` correctly points at the current one. `blobs/` = 2.8 GB for 184k immutable 1:1 keys.
- **Isolated experiment (E3):** fresh RustFS, **scanner ENABLED**, 2000 overwrites of one 256 KB key → **2001 dirs / 516 MB, zero reclamation at t+0 and t+100 s**, no scan/reclaim activity logged. (A 4 KB key inlined into `xl.meta` and showed 1 dir — confound; the non-inlinable 256 KB key is the valid test.) → **RustFS beta.8 does not delete the old dataDir on un-versioned overwrite (no inline `renameData`-delete like MinIO), and the scanner does not reclaim on any soak-relevant timescale.** So the leak is a RustFS defect *triggered by* the CA overwrite pattern; it is NOT merely our `RUSTFS_SCANNER_ENABLED=false`. Real S3 reclaims overwrites inline → would not leak (but the overwrite PUT/contention cost remains).

### C. `root_shards=8` congestion + listability
- RustFS enforces a **per-object 64-permit I/O cap**; the 8 hot `roots/` keys hit 58–63/64 (`I/O queue congestion detected`) → **503 ServiceUnavailable** (4,223 in ~9 min; CH retries them, up to 501×). RustFS CPU 3.6% throughout — **congestion, not load**.
- A *recursive* `ListObjectsV2` of `roots/` 500s (`Io error: timeout`) because the lister walks ~228k orphan dirs for 18 live keys (3.5 s for ONE shard dir); a *shallow/delimiter* list is **instant** (0.025 s). A boto3 probe lists immutable `blobs/` fully at **~7,640 obj/s during writes** — listing-during-writes is NOT broken.
- `clickhouse-disks fsck` "hangs" only because it draws a transient RustFS LIST 500 and the AWS SDK retry-loops it (the CA layer does NOT recursively list `roots/`; `listNamespaces` is a registry GET, `runFsck` lists only blobs/trees/packs + the tiny `_files/`). Mitigations: per-LIST timeout in `runFsck`/`CommandFsck` (surface the 500 instead of looping) + progress output + run against a QUIESCED pool ([[B154]]).

## Fix levers (to brainstorm)

| Lever | Addresses | Backend-agnostic? | Cost |
|---|---|---|---|
| **Cut blob/tree existence-HEADs** (presence cache; trust the manifest instead of re-HEADing reachable deps; batch; skip speculative HEADs on inline/in-manifest entries) | #1 HEAD storm (dominant) | YES (real win everywhere) | design + careful invariant work (don't mask INV-NO-LOSS) |
| **Reduce manifest-overwrite count** (coalesce/batch publishes; fewer `casPut` per commit; the deferred publish-path redesign [[B148]]/[[B149]]) | #2 leak source + roots congestion | YES | larger design |
| **Raise `root_shards`** (8 → 256) | per-key congestion (503) + `roots/` listability (spreads orphans) | YES (config) | cheap; does NOT reduce TOTAL leak/op-count, only spreads |
| **`fsck` per-LIST timeout + progress; run quiesced** | fsck usability | tool | small |
| **Browsable CA disk tree** ([[B159]]) | introspection | tool | small–medium |
| **Test-env: real S3 / MinIO / periodic pool recreate / scanner on** | RustFS-specific leak only | env | env |

## Open question for the design phase
The single highest-leverage item is **#1 (blob/tree existence-HEAD reduction)** — it is ~10× the write op-count and backend-agnostic. The exact caller(s) of the 23.7M HEADs (80% 404) must be traced before designing the fix (presence cache vs. eliminate-the-probe vs. batch). #2/#3 are the next tier and are mostly the already-scoped op-count/publish-path work ([[B148]]/[[B149]]).
