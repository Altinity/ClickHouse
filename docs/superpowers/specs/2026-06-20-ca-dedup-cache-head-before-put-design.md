# CA dedup cache + adaptive HEAD-before-PUT (P1+P2) — design

**Date:** 2026-06-20
**Backlog:** B168 (CA S3 op-count reduction), proposals P1 + P2.
**Status:** approved design; ready for an implementation plan.

## Goal

Eliminate the dominant write-tier waste on the content-addressed disk: the **wasted body
PUT that 412s on a blob dedup hit**. Today `Build::putBlob` always opens a conditional
create (`putIfAbsentStream`, `If-None-Match:*`), **streams the entire blob body**, and only
then learns at `finalize()` whether the content already existed. On a dedup hit (measured
**64% of blob creates**, ~$178/day of the bill) the streamed body is discarded, the
conditional PUT is billed as a `PreconditionFailed` (412), and a follow-up HEAD
(`observeAndAdmit`) admits the existing incarnation — so a dedup hit costs a wasted
body-PUT + a HEAD.

This design replaces that with **at most one cheap HEAD** on a dedup hit, driven by a
bounded local "known-present" cache (P1) plus a size threshold (P2). It makes **zero
changes** to the precommit / GC-fence / publish-gate protocol — safety is by construction.

Non-goals: the manifest-`casPut`-conflict half of the 412 traffic (that is P4 batch-publish,
deliberately deferred); trees/packs dedup (blobs only — that is where the measured 64% hit
rate and the wasted-body cost live).

## Key insight: HEAD-first is safe; skip-everything is not

A blob's key is its content hash, so "present" can never become "different content". The
only failure mode for a cache that says present is a **stale hit** (the blob was GC-deleted
since we cached it). Two ways to use a hit:

- **Skip PUT *and* HEAD (true 0-ops) — REJECTED.** Recording the dep as "present, observed
  at the current round" without observing it lets the publish gate's `revalidateDeps` *keep
  same-round deps without re-HEAD*; a stale hit could then publish a ref to a missing blob
  (**dangle**). Making it safe would require touching the precommit/revalidate protocol
  (force-revalidate the dep + actively evict on local GC-delete) — the tested core.
- **Cache drives HEAD-before-PUT (CHOSEN).** A hit means "almost certainly present" → do a
  cheap **HEAD first** instead of streaming the body. HEAD present → admit (no body upload);
  HEAD 404 (stale/new) → fall through to the normal body PUT. We **always genuinely observe
  present-at-current-round**, so a stale hit is **self-correcting** (the HEAD reveals 404 →
  we PUT). No precommit/gate changes, no cache invalidation, no GC-forget hook.

## Components

### 1. The known-present cache — a `Store` member

- One cache per content-addressed disk, owned by `Store` (the per-disk singleton,
  `shared_ptr`/`enable_shared_from_this`), shared across all queries/threads/builds.
- Reuse CH's `CacheBase<UInt128, Marker, …, LRUCachePolicy>` (`src/Common/CacheBase.h`):
  LRU, **bytes-bounded**, internally synchronized. Key = blob logical content hash
  (`UInt128`); value = a trivial presence marker (weight ≈ key + node overhead).
- **Bound:** new disk setting `content_addressed_dedup_cache_bytes` (default **67108864 =
  64 MiB** ≈ 2–3M hashes; **`0` = cache disabled**). The byte cap is a hard ceiling — the
  cache can never bloat.
- API on `Store` (thin wrappers; no-ops when disabled):
  - `bool dedupCacheContains(const UInt128 & hash) const`
  - `void dedupCacheAdd(const UInt128 & hash)`
  - No remove/invalidate: the cache is only a hint; staleness is caught by the mandatory HEAD.

### 2. `Build::putBlob` — the only changed call-site

```
head_first = store->dedupCacheContains(logical_hash)            // P1: seen before
          || source.size >= dedup_head_first_min_bytes           // P2: large-body protection

if head_first:
    HeadResult hr = store->backend().head(key)
    if hr.exists:                                  // dedup hit confirmed — NO body upload
        admitted = observeAndAdmit(Blob, logical_hash, key, hr)  // reuse hr (overload)
        store->dedupCacheAdd(logical_hash)
        return BlobRef{id, admitted}
    // hr.exists == false -> stale/new -> fall through to the PUT path

<existing putIfAbsentStream body-PUT path, unchanged>
    on PutOutcome::Done:        store->dedupCacheAdd(logical_hash)   // now known present
    on PreconditionFailed(412): observeAndAdmit(...); store->dedupCacheAdd(logical_hash)
```

- **No double-HEAD:** `observeAndAdmit` currently does its own `backend().head(key)`
  (`CasBuild.cpp:279`). Add an overload `observeAndAdmit(kind, hash, key, const HeadResult &
  hr)` that consumes an already-fetched `HeadResult`; the HEAD-first branch passes its `hr`.
  Net per dedup hit = exactly **1 HEAD**.
- The decision uses `source.size`, which is already known before streaming
  (`header.logical_size = source.size`).
- Every create outcome (HEAD-first admit, PUT Done, PUT 412-admit) feeds `dedupCacheAdd`, so
  the cache fills from all paths.

### 3. Settings (disk-config → `Store`)

- `content_addressed_dedup_cache_bytes` — LRU byte cap; default `67108864` (64 MiB); `0`
  disables P1 (cache always misses → behavior is P2-only).
- `content_addressed_dedup_head_first_min_bytes` — P2 size threshold; default `1048576`
  (1 MiB); `0` disables the size trigger (HEAD-first then only on a cache hit).

### 4. Instrumentation (extends P0)

Add ProfileEvents at the `putBlob` decision (in the `CasInstrumentedBackend` event style):
- `CasBlobDedupCacheHit` — a `dedupCacheContains` hit.
- `CasBlobHeadFirst` — a HEAD-first attempt (hit or size-triggered).
- `CasBlobBodyPutAvoided` — a HEAD-first HEAD found the blob present → a body PUT was skipped.

These make the **baseline → P1P2 delta** measurable against the 20-min no-chaos baseline soak
captured at design time (`Cas*` events: expect `CasBlobPutDedup`↓ and total PUTs↓).

## Data flow / control flow

1. INSERT/merge produces a blob → `Build::putBlob(id, source)` with `source.size` and the
   content hash known.
2. Decide HEAD-first (cache hit OR size ≥ threshold) vs PUT-first.
3. HEAD-first + present → admit via the existing cold-reuse rule (`observeAndAdmit`, reusing
   the HEAD result); record the dep exactly as the current 412 branch does. No body upload.
4. Otherwise → the existing conditional body PUT; `Done` or `412` both end "present".
5. In all "present" outcomes, add the hash to the cache.
6. The rest of the publish path (tree, precommit, fail-closed publish, ref CAS) is
   **unchanged** — the dep recorded by `observeAndAdmit` is identical to today's.

## Error handling / safety

- **Correctness is independent of the cache.** A hit only selects HEAD-first; presence is
  always confirmed by the HEAD before the body is skipped. A stale hit → HEAD 404 → the
  unchanged PUT path. No dangle is possible from the cache.
- **B136/B137 race unchanged:** the existing `observeAndAdmit`→`resurrect` GC-race recovery
  (the blob vanishing in the HEAD→GET window) is reused as-is; the HEAD-first branch enters
  the same `observeAndAdmit` and inherits its retry/re-upload behavior.
- **Memory:** the only contract is the LRU byte cap; the cache is fixed-ceiling and evicts
  LRU. Disabled (`bytes=0`) → the code path is identical to today except for the P2 size
  trigger (also disable-able).
- **Thread-safety:** `CacheBase` is internally locked; concurrent `contains`/`add` from
  parallel builds are safe and, being hints, need no external synchronization.

## Testing

**gtests** (instrumented/mock backend counting PUT vs HEAD):
1. Cache hit → HEAD-first → **0 body PUTs**, 1 HEAD, correct `BlobRef`.
2. Stale hit (cache says present, backend 404s) → HEAD-first HEAD 404 → falls through to the
   body PUT → blob created correctly (no dangle, no exception).
3. Size threshold: a blob ≥ `dedup_head_first_min_bytes` on a cache miss → HEAD-first.
4. `dedup_cache_bytes=0` → cache always misses → behavior reduces to P2-only (size trigger).
5. Bounded eviction: inserting > cap distinct hashes keeps the cache within the byte ceiling.
6. No double-HEAD: a HEAD-first dedup hit issues exactly one backend `head` (overload used).

**Soak / oracle:** re-run the **20-min no-chaos soak** with P1/P2 enabled and compare the
`Cas*` ProfileEvents against the design-time baseline (expect `CasBlobPutDedup` and total PUT
count down, `CasBlobBodyPutAvoided` > 0), with `dangling=0` and the aggregate oracle
(`compare_aggregates`) intact. A short chaos soak confirms the GC-race path still recovers.

## Files

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h/.cpp` — the
  `dedup_cache` member + `dedupCacheContains`/`dedupCacheAdd`; settings wired at construction.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h/.cpp` —
  `putBlob` HEAD-first decision; `observeAndAdmit` overload taking a `HeadResult`.
- `CasInstrumentedBackend.cpp` (+ the ProfileEvents .cpp) — the three new events.
- Disk-config parsing for the two new `content_addressed_dedup_*` settings → `Store` ctor.
- `src/Disks/tests/gtest_*` — the unit tests above.
