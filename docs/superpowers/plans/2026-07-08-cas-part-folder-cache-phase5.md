---
description: "Implementation plan, Phase 5 of the CAS part-folder cache: byte-bounding the Store's manifest decode cache (today count-bounded with a multi-GB worst case)."
sidebar_label: "CAS Part Folder Cache Plan P5"
sidebar_position: 18
slug: "/superpowers/plans/2026-07-08-cas-part-folder-cache-phase5"
title: "CAS Part Folder Cache — Phase 5 Plan"
doc_type: "reference"
---

# CAS Part Folder Cache — Phase 5: Byte-Bound `manifest_cache` {#phase-5-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** convert the Store's manifest decode cache from count-bounded (16384 entries, no byte limit — decoded manifests can each hold megabytes of inline bytes, a multi-GB worst case) to a byte-weighted `CacheBase` LRU. No relocation of `shard_decode_cache` or `dedup_cache` (spec: verified machinery stays put).

**Architecture:** replace the `unordered_map` + wholesale-clear `manifest_cache` in `Cas::Store` with a `CacheBase` keyed by the existing `ManifestCacheKey` (`(ManifestId, Token)` — the token component keeps the fail-closed re-incarnation property), weighted by decoded size. The view cache's weight keeps its conservative `manifest_size` component (a retained view can outlive the decode-cache entry, so dropping it would under-count — the spec's Phase-5 accounting note). Requires Phase 4 merged (for the Phase-4 request-count tests to keep guarding behavior), but is functionally independent of retention.

**Tech Stack:** C++, `Common/CacheBase` (LRU), gtest.

## Global Constraints {#global-constraints}

- Same conventions as the Phase 1 plan §Global Constraints.
- The decode cache's correctness contract is unchanged: keyed by `(ManifestId, Token)`; a token mismatch is a miss (fresh `GET` + decode); a failed decode is never cached; `readManifestShared`'s mandatory `HEAD` stays.
- New pool config knob: `manifest_decode_cache_bytes`, default `128 MiB`, `0` disables decode caching entirely (every read decodes fresh — a supported diagnostic mode).

---

### Task 1: `CacheBase`-backed decode cache {#task-1}

**Files:**
- Modify: `.../ContentAddressed/Core/CasStore.h` (replace the `manifest_cache` members; add the `PoolConfig` knob), `.../Core/CasStore.cpp` (`readManifestShared`, the `Store` ctor), `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` + `.../ContentAddressedMetadataStorage.h/.cpp` (thread the config key like `dedup_cache_bytes`)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: existing `ManifestCacheKey` / `ManifestCacheKeyHash`.
- Produces: `PoolConfig::manifest_decode_cache_bytes` (`uint64_t`, default `128ULL << 20`); the `Store` member becomes

```cpp
    struct PartManifestWeight
    {
        size_t operator()(const PartManifest & m) const
        {
            size_t bytes = 256;
            for (const auto & e : m.entries)
                bytes += e.path.size() + e.inline_bytes.size() + 96;
            return bytes;
        }
    };
    using ManifestDecodeCache = CacheBase<ManifestCacheKey, PartManifest, ManifestCacheKeyHash, PartManifestWeight>;
    std::unique_ptr<ManifestDecodeCache> manifest_cache;   /// nullptr <=> decode caching disabled
```

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasStore, ManifestDecodeCacheIsByteBounded)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    const DB::Cas::Layout layout("p");
    const DB::Cas::RootNamespace ns{"srv/t1"};

    /// 8 manifests x ~1 MiB of inline bytes; a 2 MiB decode-cache bound must hold while every
    /// read stays correct (evicted decodes just re-GET + re-decode).
    std::vector<DB::Cas::ManifestId> ids;
    for (int i = 0; i < 8; ++i)
    {
        const DB::Cas::ManifestRef ref{.writer_epoch = 1, .build_sequence = static_cast<uint64_t>(i + 1),
                                       .manifest_ordinal = 1};
        DB::Cas::ManifestEntry e;
        e.path = "big.txt";
        e.placement = DB::Cas::EntryPlacement::Inline;
        e.blob_hash = DB::UInt128(i + 1);
        e.inline_bytes = DB::String(1 << 20, 'a' + i);
        e.blob_size = e.inline_bytes.size();
        ids.push_back(DB::Cas::tests::writeManifestRaw(*backend, layout, ns, ref, {e}));
        DB::Cas::tests::publishCommittedTransition(*backend, layout, ns, "part_" + std::to_string(i),
                                                   std::nullopt, ref);
    }

    DB::Cas::PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1};
    config.manifest_decode_cache_bytes = 2ULL << 20;
    auto store = DB::Cas::Store::open(backend, std::move(config));

    uint64_t total_gets = 0;
    for (int round = 0; round < 2; ++round)
        for (int i = 0; i < 8; ++i)
        {
            auto resolved = store->resolveRef(ns, "part_" + std::to_string(i));
            ASSERT_TRUE(resolved.has_value());
            auto m = store->readManifestShared(resolved->manifest_id);
            ASSERT_EQ(m->entries.size(), 1u);
            EXPECT_EQ(m->entries[0].inline_bytes[0], static_cast<char>('a' + i));   /// always correct
        }
    for (const auto & id : ids)
        total_gets += backend->getCount(layout.manifestKey(id));

    /// The bound forces re-GETs (16 reads over a 2 MiB window of ~1 MiB decodes cannot all hit),
    /// proving eviction actually happens...
    EXPECT_GT(total_gets, 8u);
    /// ...and the cache reports an in-bound retained size.
    EXPECT_LE(store->manifestDecodeCacheBytesForTest(), 2ULL << 20);
}
```

Also add a small accessor for the test (public, next to the other `ForTest` seams in `CasStore.h`):

```cpp
    /// Test seam: retained bytes of the manifest decode cache (0 when disabled).
    size_t manifestDecodeCacheBytesForTest() const { return manifest_cache ? manifest_cache->sizeInBytes() : 0; }
```

- [ ] **Step 2: Run to verify it fails** (compile error: `manifest_decode_cache_bytes` / accessor undeclared).

- [ ] **Step 3: Implement**

`PoolConfig` (in `CasStore.h`, next to `dedup_cache_bytes`):

```cpp
    /// Phase-5 (part-folder cache spec): byte bound for the manifest DECODE cache. The old cache
    /// was count-bounded only (16384 entries) — decoded manifests carry inline bytes, so the worst
    /// case was multi-GB. 0 disables decode caching (every read decodes fresh — diagnostic mode).
    uint64_t manifest_decode_cache_bytes = 128ULL << 20;
```

Replace the three old members (`MANIFEST_CACHE_MAX_ENTRIES`, `manifest_cache_mutex`, the `unordered_map manifest_cache`) with the `CacheBase` member above (keep `ManifestCacheKey` / `ManifestCacheKeyHash` — same key, same fail-closed token semantics). `Store` ctor, next to the `dedup_cache` construction (`CurrentMetrics::end()` placeholders, LRU, keep the old 16384 as the count bound):

```cpp
    if (config.manifest_decode_cache_bytes > 0)
        manifest_cache = std::make_unique<ManifestDecodeCache>(
            "LRU", CurrentMetrics::end(), CurrentMetrics::end(),
            config.manifest_decode_cache_bytes, /*max_count=*/16384, ManifestDecodeCache::DEFAULT_SIZE_RATIO);
```

`readManifestShared`: the lookup block becomes

```cpp
    if (manifest_cache)
        if (auto cached = manifest_cache->get(ManifestCacheKey{.manifest_id = id, .token = head.token}))
            return cached;
```

and the insert block becomes

```cpp
    auto decoded = std::make_shared<PartManifest>(std::move(body));
    if (manifest_cache)
        manifest_cache->set(ManifestCacheKey{.manifest_id = id, .token = head.token}, decoded);
    return decoded;
```

(`CacheBase::get` returns `std::shared_ptr<PartManifest>`; the method's `shared_ptr<const PartManifest>` return converts implicitly. The old wholesale-clear comment dies with the map.)

Factory threading (mirror `dedup_cache_bytes` exactly): config key `.manifest_decode_cache_bytes` in `MetadataStorageFactory.cpp`, a defaulted ctor param + member in `ContentAddressedMetadataStorage`, assigned into `pool_config.manifest_decode_cache_bytes` in `startup`.

- [ ] **Step 4: Run the new test + the full `Cas*` batch + the Phase-4 request-count battery**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10`
Expected: PASS — in particular `CasPartFolderAccess.RetainedHitSkipsManifestHead` and `BaselineRequestCountsWithoutRetention` still hold (the default 128 MiB bound never evicts in those tests).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "CAS store: byte-bounded manifest decode cache (was count-only, multi-GB worst case)"
```

---

### Task 2: Phase verification {#task-2}

- [ ] **Step 1:** Full CA stateless suite (same list/command as the Phase 1 plan, log `build_debug/test_phase5_final.log`) → all pass, no behavior change.
- [ ] **Step 2:** Read-storm sanity: re-run the Phase-4 soak lane once with the default bound (or, minimally, the gtest battery) and confirm `CasPartFolderView*` counters and anomaly baselines are unchanged from the Phase-4 run.

## Phase acceptance {#phase-acceptance}

Bounded decode memory under a many-parts read storm; zero behavior change; the view cache's conservative `manifest_size` weighting is retained (spec §Phase 5 accounting note — a retained view can outlive the decode-cache entry, so removing the over-count would under-count).
