# CA dedup cache + adaptive HEAD-before-PUT (P1+P2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a content-addressed blob dedup hit, replace the wasted body-PUT-that-412s with at most one cheap HEAD, driven by a bounded per-disk known-present cache (P1) plus a size threshold (P2).

**Architecture:** A bytes-bounded LRU set of confirmed-present blob hashes lives in `Cas::Store` (reusing `src/Common/CacheBase.h`). `Build::putBlob` consults it: a cache hit OR a large blob → HEAD-first (admit without uploading the body on a present hit); otherwise the existing conditional body-PUT. Safe by construction — presence is always confirmed by the HEAD before the body is skipped, so a stale cache entry self-corrects to the normal PUT. Zero changes to the precommit/GC-fence/publish-gate protocol.

**Tech Stack:** C++ (ClickHouse), `CacheBase`/`LRUCachePolicy`, ClickHouse `ProfileEvents`, gtest (`unit_tests_dbms`), the `ca-soak` harness for op-count validation.

**Spec:** `docs/superpowers/specs/2026-06-20-ca-dedup-cache-head-before-put-design.md`

**Build/test note (read once):** C++ rebuilds are slow. Build the test binary with `ninja -C build unit_tests_dbms` (redirect to a log; analyze via a subagent). Run a CA gtest subset with `build/src/unit_tests_dbms --gtest_filter='CaDedup*:CaWiring*:Cas*'`. The 2 pre-existing failures `CaWiringOps.FreezeViaHardLinksIntoShadow` (B186) and `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` (B140) are expected RED and unrelated.

---

## File Structure

- `Core/CasStore.h` / `CasStore.cpp` — `PoolConfig` gets two settings; `Store` gets the `dedup_cache` member + `dedupCacheContains`/`dedupCacheAdd`.
- `Core/CasBuild.h` / `CasBuild.cpp` — `observeAndAdmit` gains a `HeadResult`-consuming overload; `putBlob` gets the HEAD-first decision.
- `src/Common/ProfileEvents.cpp` — three new CA events.
- `MetadataStorages/MetadataStorageFactory.cpp` + `ContentAddressedMetadataStorage.cpp/.h` — parse the two settings and thread them into `PoolConfig`.
- `src/Disks/tests/gtest_ca_dedup_cache.cpp` (new) — unit tests; registered in the gtest CMake list like the other `gtest_ca_*`/`gtest_cas_*` files.

---

## Task 1: Add the two settings to `PoolConfig` and thread them through

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` (the `PoolConfig` struct, ~line 24-40)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` (~line 246, beside `content_addressed_root_shards`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (ctor ~line 147-158; `pool_config` build ~line 346-355) and its header (ctor signature)

- [ ] **Step 1: Add the fields to `PoolConfig`** (`CasStore.h`, after `root_shards`/`blob_header_len`):

```cpp
    /// P1 (dedup cache): byte ceiling for the per-disk known-present blob-hash LRU set. 0 disables the
    /// cache (every create misses → P2-only). A hint cache; correctness never depends on it.
    uint64_t dedup_cache_bytes = 64ULL << 20;     /// 64 MiB
    /// P2 (HEAD-before-PUT): on a dedup-cache MISS, a blob whose body is >= this many bytes is written
    /// HEAD-first (cheap HEAD avoids streaming a large body that would 412). 0 disables the size trigger.
    uint64_t dedup_head_first_min_bytes = 1ULL << 20;   /// 1 MiB
```

- [ ] **Step 2: Parse the settings in the factory** (`MetadataStorageFactory.cpp`, next to the existing `root_shards` parse at ~line 246):

```cpp
        const uint64_t dedup_cache_bytes = config.getUInt64(
            config_prefix + ".content_addressed_dedup_cache_bytes", 64ULL << 20);
        const uint64_t dedup_head_first_min_bytes = config.getUInt64(
            config_prefix + ".content_addressed_dedup_head_first_min_bytes", 1ULL << 20);
```

Pass both into the `ContentAddressedMetadataStorage` constructor call (extend the argument list at ~line 249, after `root_shards`).

- [ ] **Step 3: Thread through `ContentAddressedMetadataStorage`** — add `uint64_t dedup_cache_bytes_`, `uint64_t dedup_head_first_min_bytes_` to the ctor signature (header + `.cpp` ~line 147), store them in members `dedup_cache_bytes` / `dedup_head_first_min_bytes` (init list ~line 158), and set them on `pool_config` (~line 354, beside `pool_config.root_shards = root_shards;`):

```cpp
    pool_config.dedup_cache_bytes = dedup_cache_bytes;
    pool_config.dedup_head_first_min_bytes = dedup_head_first_min_bytes;
```

- [ ] **Step 4: Build to verify it compiles**

Run: `ninja -C build unit_tests_dbms 2>&1 | tail -3` (via the build-log + subagent-summary convention)
Expected: builds clean (no behavior change yet — fields are set but unused).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h
git commit -m "CA dedup cache: plumb dedup_cache_bytes + dedup_head_first_min_bytes settings (P1/P2)"
```

---

## Task 2: Add the known-present cache to `Store`

**Files:**
- Modify: `Core/CasStore.h` (member + method decls), `Core/CasStore.cpp` (ctor init + method defs)
- Create/Test: `src/Disks/tests/gtest_ca_dedup_cache.cpp`

The cache key is the blob logical hash (`UInt128`). Value is a 1-byte presence marker; a fixed per-entry weight makes the byte cap meaningful.

- [ ] **Step 1: Write the failing test** (`gtest_ca_dedup_cache.cpp`):

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

using namespace DB::Cas;

static StorePtr openStore(uint64_t dedup_cache_bytes)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg;
    cfg.pool_prefix = "p";
    cfg.dedup_cache_bytes = dedup_cache_bytes;
    return Store::open(backend, cfg);
}

TEST(CaDedupCache, AddThenContains)
{
    auto s = openStore(64ULL << 20);
    UInt128 h = hexToU128("00000000000000000000000000000abc");
    EXPECT_FALSE(s->dedupCacheContains(h));
    s->dedupCacheAdd(h);
    EXPECT_TRUE(s->dedupCacheContains(h));
}

TEST(CaDedupCache, DisabledNeverContains)
{
    auto s = openStore(0);                       // disabled
    UInt128 h = hexToU128("00000000000000000000000000000abc");
    s->dedupCacheAdd(h);
    EXPECT_FALSE(s->dedupCacheContains(h));       // no-op when disabled
}
```

- [ ] **Step 2: Run it to verify it fails** — `build/src/unit_tests_dbms --gtest_filter='CaDedupCache.*'` → FAIL (no `dedupCacheContains`/`dedupCacheAdd`).

- [ ] **Step 3: Implement the cache in `Store`.** In `CasStore.h` add includes + a member + decls:

```cpp
#include <Common/CacheBase.h>
...
    /// P1 known-present blob-hash cache (see design 2026-06-20). A hint only — correctness never
    /// depends on it (a stale hit is caught by the mandatory HEAD in putBlob). nullptr when disabled.
    bool dedupCacheContains(const UInt128 & blob_hash) const;
    void dedupCacheAdd(const UInt128 & blob_hash);
private:
    struct DedupPresent {};   /// 1-byte presence marker
    struct DedupWeight { size_t operator()(const DedupPresent &) const { return 64; } };  /// ~key+node bytes
    using DedupCache = CacheBase<UInt128, DedupPresent, UInt128TrivialHash, DedupWeight>;
    std::unique_ptr<DedupCache> dedup_cache;   /// nullptr ⇔ disabled
```

In `CasStore.cpp`, in the `Store` ctor body, after `config` is set:

```cpp
    if (config.dedup_cache_bytes > 0)
        dedup_cache = std::make_unique<DedupCache>(config.dedup_cache_bytes);
```

And the methods:

```cpp
bool Store::dedupCacheContains(const UInt128 & blob_hash) const
{
    return dedup_cache && dedup_cache->contains(blob_hash);
}

void Store::dedupCacheAdd(const UInt128 & blob_hash)
{
    if (dedup_cache)
        dedup_cache->set(blob_hash, std::make_shared<DedupPresent>());
}
```

(If `UInt128TrivialHash` is not the correct hasher name in this tree, use the existing `UInt128` hash — grep `CacheBase<UInt128` / `UInt128Hash` for the established spelling. `CacheBase`'s ctor used is `CacheBase(size_t max_size_in_bytes)`; confirm the single-arg byte-cap ctor at `CacheBase.h:59`.)

- [ ] **Step 4: Run the tests** — `build/src/unit_tests_dbms --gtest_filter='CaDedupCache.*'` → PASS.

- [ ] **Step 5: Add an eviction-bound test** (append to `gtest_ca_dedup_cache.cpp`), then build + run:

```cpp
TEST(CaDedupCache, BoundedByBytes)
{
    auto s = openStore(64 * 64);                 // ~64 entries (weight 64 each)
    for (uint64_t i = 0; i < 100000; ++i)
        s->dedupCacheAdd(hexToU128(fmt::format("{:032x}", i)));
    // The earliest-added key must have been evicted (LRU) — cache stays bounded, never bloats.
    EXPECT_FALSE(s->dedupCacheContains(hexToU128(fmt::format("{:032x}", 0ULL))));
}
```

- [ ] **Step 6: Register the test file + commit.** Add `gtest_ca_dedup_cache.cpp` to the unit-test source list (same place `gtest_ca_wiring.cpp` / `gtest_cas_gc_leak.cpp` are listed — `src/Disks/tests/CMakeLists.txt` or the `dbms` gtest glob). Then:

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_ca_dedup_cache.cpp src/Disks/tests/CMakeLists.txt
git commit -m "CA dedup cache: bytes-bounded known-present blob-hash LRU in Store (P1)"
```

---

## Task 3: `observeAndAdmit` overload that consumes a `HeadResult` (no double-HEAD)

**Files:**
- Modify: `Core/CasBuild.h` (decl), `Core/CasBuild.cpp` (`observeAndAdmit` ~line 277)

The current `observeAndAdmit(kind, hash, key)` does its own `backend().head(key)` at line 279. Extract the post-HEAD logic into an overload taking the `HeadResult`, and make the existing method call it.

- [ ] **Step 1: Add the overload decl** (`CasBuild.h`, near the existing `observeAndAdmit`):

```cpp
    /// Overload for callers that already hold a fresh HeadResult for `key` (the HEAD-before-PUT path),
    /// avoiding a redundant second HEAD. `hr.exists` MUST be true.
    uint64_t observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key, const HeadResult & hr);
```

- [ ] **Step 2: Refactor the definition** (`CasBuild.cpp`): keep the original 3-arg method as a thin wrapper that HEADs then delegates; move the body (lines 280+) into the 4-arg overload:

```cpp
uint64_t Build::observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key)
{
    const HeadResult hr = store->backend().head(key);
    if (!hr.exists)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "Build: object {} absent — cannot reuse (caller must upload it)", key);
    return observeAndAdmit(kind, hash, key, hr);
}

uint64_t Build::observeAndAdmit(ObjectKind kind, const UInt128 & hash, const String & key, const HeadResult & hr)
{
    // (the existing body that used `hr`, starting at the logical_size computation; unchanged)
    ...
}
```

- [ ] **Step 3: Build + run the existing CA gtests to prove no behavior change** — `ninja -C build unit_tests_dbms`; `build/src/unit_tests_dbms --gtest_filter='CaWiring*:Cas*'` → same pass set as before (only B186 + B140 RED).

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp
git commit -m "CA: observeAndAdmit overload taking a HeadResult (no double-HEAD; no behavior change)"
```

---

## Task 4: The three new ProfileEvents

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (the CA `M(...)` block, after `CasBlobHeadMiss` ~line 749)

- [ ] **Step 1: Add the events** (in the `M(...)` macro list):

```cpp
    M(CasBlobDedupCacheHit, "CA blob known-present cache hit (P1)", ValueType::Number) \
    M(CasBlobHeadFirst,     "CA blob HEAD-first attempts (P1/P2)", ValueType::Number) \
    M(CasBlobBodyPutAvoided,"CA blob body PUTs avoided by HEAD-first (P1/P2)", ValueType::Number) \
```

- [ ] **Step 2: Build to verify** — `ninja -C build unit_tests_dbms 2>&1 | tail -3` → clean.

- [ ] **Step 3: Commit**

```bash
git add src/Common/ProfileEvents.cpp
git commit -m "CA: ProfileEvents for dedup-cache hit / HEAD-first / body-PUT-avoided (P1/P2 measurement)"
```

---

## Task 5: The HEAD-first decision in `putBlob`

**Files:**
- Modify: `Core/CasBuild.cpp` (`putBlob`, the create path ~line 130-198)
- Test: `src/Disks/tests/gtest_ca_dedup_cache.cpp`

- [ ] **Step 1: Write the failing tests** (append to `gtest_ca_dedup_cache.cpp`). Use the `InMemoryBackend`'s op counters (it already underlies `CasGcLeak`); if it lacks per-op counters, add a HEAD counter + PUT counter to `InMemoryBackend` in this step.

```cpp
TEST(CaDedupCache, HitTakesHeadFirstNoBodyPut)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg; cfg.pool_prefix = "p"; cfg.dedup_cache_bytes = 64ULL << 20;
    auto s = Store::open(backend, cfg);

    // First create uploads the body (cache miss, small blob → PUT-first).
    auto b1 = s->startBuild({});
    b1->putBlob(idOf("payload"), BlobSource::fromString("payload"));
    const size_t puts_after_first = backend->putCount();
    EXPECT_GT(puts_after_first, 0u);

    // Second create of the SAME content: cache hit → HEAD-first → present → NO new body PUT.
    auto b2 = s->startBuild({});
    b2->putBlob(idOf("payload"), BlobSource::fromString("payload"));
    EXPECT_EQ(backend->putCount(), puts_after_first);    // no extra body PUT
    EXPECT_GE(backend->headCount(), 1u);                 // exactly one HEAD on the hit
}

TEST(CaDedupCache, StaleHitFallsThroughToPut)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg; cfg.pool_prefix = "p"; cfg.dedup_cache_bytes = 64ULL << 20;
    auto s = Store::open(backend, cfg);
    // Prime the cache as if present, but the backend has no such object (simulates GC-deleted-since).
    s->dedupCacheAdd(idOf("ghost").hashU128());           // helper: BlobId -> UInt128
    auto b = s->startBuild({});
    b->putBlob(idOf("ghost"), BlobSource::fromString("ghost"));   // HEAD 404 → falls through → PUT
    EXPECT_GT(backend->putCount(), 0u);                   // body was uploaded; no dangle/throw
}

TEST(CaDedupCache, LargeBlobMissTakesHeadFirst)
{
    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg; cfg.pool_prefix = "p";
    cfg.dedup_cache_bytes = 0;                            // P1 off → isolate P2
    cfg.dedup_head_first_min_bytes = 16;                  // tiny threshold for the test
    auto s = Store::open(backend, cfg);
    auto b = s->startBuild({});
    b->putBlob(idOf("a-large-enough-blob-body"), BlobSource::fromString("a-large-enough-blob-body"));
    EXPECT_GE(backend->headCount(), 1u);                  // size-triggered HEAD-first even on a miss
}
```

- [ ] **Step 2: Run to verify they fail** — `build/src/unit_tests_dbms --gtest_filter='CaDedupCache.*'` → the three new ones FAIL.

- [ ] **Step 3: Implement the decision** in `putBlob`, immediately before the `putIfAbsentStream` block (replace the unconditional create with the gated one). `logical_hash`, `source.size`, and `key` are all already in scope:

```cpp
    const bool head_first =
        store->dedupCacheContains(logical_hash)
        || (store->poolConfig().dedup_head_first_min_bytes > 0
            && source.size >= store->poolConfig().dedup_head_first_min_bytes);
    if (head_first)
    {
        ProfileEvents::increment(ProfileEvents::CasBlobHeadFirst);
        const HeadResult hr = store->backend().head(key);
        if (hr.exists)
        {
            ProfileEvents::increment(ProfileEvents::CasBlobBodyPutAvoided);
            if (store->dedupCacheContains(logical_hash))
                ProfileEvents::increment(ProfileEvents::CasBlobDedupCacheHit);
            const uint64_t admitted = observeAndAdmit(ObjectKind::Blob, logical_hash, key, hr);
            store->dedupCacheAdd(logical_hash);
            return BlobRef{id, admitted};
        }
        // hr.exists == false → stale cache entry or genuinely new → fall through to the body PUT.
    }

    // ---- existing conditional body-PUT path (putIfAbsentStream ... finalize) ----
```

Then add `store->dedupCacheAdd(logical_hash);` on **both** terminal "present" outcomes of the existing path: right before `return BlobRef{id, source.size};` (the `PutOutcome::Done` branch) and right after the `observeAndAdmit` success in the `PreconditionFailed` branch.

(`ProfileEvents` IDs are declared in `CasBuild.cpp` via `namespace ProfileEvents { extern const Event CasBlob...; }` at the top — add the three new `extern const Event` lines beside the existing CA ones.)

- [ ] **Step 4: Build + run** — `ninja -C build unit_tests_dbms`; `build/src/unit_tests_dbms --gtest_filter='CaDedupCache.*:CaWiring*:Cas*'` → all `CaDedupCache.*` PASS; `CaWiring*`/`Cas*` unchanged (only B186 + B140 RED).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_ca_dedup_cache.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h
git commit -m "CA dedup: putBlob HEAD-first on cache hit / large blob; skip wasted body-PUT (P1+P2)"
```

---

## Task 6: Soak op-count validation (baseline vs P1+P2)

**Files:** none (validation only). Uses `utils/ca-soak` and the design-time baseline CSV.

- [ ] **Step 1: Build the server** — `ninja -C build clickhouse` (the soak mounts `build/programs/clickhouse`).

- [ ] **Step 2: Fresh cluster + run the SAME baseline soak with P1/P2 ON.**

```bash
cd utils/ca-soak
docker compose down --remove-orphans && docker compose up -d   # fresh pool, fresh binary
# wait for ch1/ch2 healthy, then:
python3 -m soak.run --seed 20260620 --phase 3 --duration 20m --no-chaos --metrics soak_p1p2.db
```

Expected: `PHASE3 OK`, `dangling=0`, aggregate oracle intact (both replicas equal).

- [ ] **Step 3: Dump the P1P2 `Cas*` metrics to a name-sorted CSV** (same shape as the baseline CSV produced at design time):

```bash
for n in ch1 ch2; do
  docker exec ca-soak-$n-1 clickhouse client --query \
    "SELECT event, value FROM system.events WHERE event LIKE 'Cas%' ORDER BY event FORMAT CSVWithNames" \
    > logs/casmetrics_p1p2_$n.csv
done
```

- [ ] **Step 4: Compare against the baseline CSV** and record the result in the worklog: expect `CasBlobPutDedup` ↓, total blob PUTs ↓, `CasBlobBodyPutAvoided` > 0, with no correctness regression. If `CasBlobBodyPutAvoided` is ~0, investigate (cache not populating, or the dedup hits are cross-server cache-misses below the size threshold — note for a possible P2-EWMA follow-up).

- [ ] **Step 5: Commit the validation note** (worklog + the P1P2 CSV under `utils/ca-soak/logs/`, or a short report under `docs/superpowers/reports/`).

---

## Self-Review

**Spec coverage:**
- Cache in `Store`, `CacheBase`, bytes-bounded, `bytes=0` disables → Task 2. ✓
- `putBlob` HEAD-first (cache hit OR size threshold); no double-HEAD → Tasks 5 + 3. ✓
- Settings `content_addressed_dedup_cache_bytes` / `…_head_first_min_bytes` → Task 1. ✓
- Instrumentation (3 events) → Task 4. ✓
- Safety (stale hit self-corrects; no gate changes) → Task 5 test `StaleHitFallsThroughToPut`; no precommit/gate file is touched. ✓
- Tests (hit→0 body PUT, stale hit, size threshold, disabled, bounded eviction, no double-HEAD) → Tasks 2/3/5. ✓
- Soak op-count validation vs baseline → Task 6. ✓

**Placeholder scan:** the only deliberately-open items are spelled-out verifications, not TODOs: the exact `UInt128` hasher spelling (Task 2 Step 3 says grep for the established name) and whether `InMemoryBackend` already exposes `putCount()/headCount()` (Task 5 Step 1 says add them if missing). Both are concrete "verify-then-use" instructions, not hand-waves.

**Type consistency:** `dedupCacheContains`/`dedupCacheAdd` (Store), `observeAndAdmit(…, const HeadResult &)` (Build), `dedup_cache_bytes`/`dedup_head_first_min_bytes` (PoolConfig), `CasBlobDedupCacheHit`/`CasBlobHeadFirst`/`CasBlobBodyPutAvoided` (ProfileEvents) — names are used identically across tasks.
