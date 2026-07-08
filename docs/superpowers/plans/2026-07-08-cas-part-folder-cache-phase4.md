---
description: "Implementation plan, Phase 4 of the CAS part-folder cache: bounded retention with validate-on-hit, single-flight, write-through erases, settings (on by default), and the one-GET tests."
sidebar_label: "CAS Part Folder Cache Plan P4"
sidebar_position: 17
slug: "/superpowers/plans/2026-07-08-cas-part-folder-cache-phase4"
title: "CAS Part Folder Cache — Phase 4 Plan"
doc_type: "reference"
---

# CAS Part Folder Cache — Phase 4: Retention {#phase-4-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** enable the bounded in-memory retained-view map with validate-on-hit semantics — the request-count phase. Cache ON by default (64 MiB); `cas_part_folder_cache_bytes = 0` disables it.

**Architecture:** a `Common/CacheBase` LRU keyed by `PartRefKey::cacheKey`, consulted ONLY for `CachedForLoad`; every hit is validated against the fresh `resolveRef` result by comparing `(manifest_id, mutable_files)`; manifest-match/mutable-drift clones the view around the shared decode (no manifest read); `ForceFresh`/`StrictValidate` never serve a retained view (`ForceFresh` re-proves the body per call — review-critical). Write-through erases are hygiene, not correctness. Requires Phase 3 merged. Spec: `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md` §Cache Semantics, §Safety Analysis.

**Tech Stack:** C++, `Common/CacheBase` (LRU policy, byte weights, CurrentMetrics), gtest with `CountingBackend`, `std::thread` for the single-flight test (no sleeps).

## Global Constraints {#global-constraints}

- Same conventions as the Phase 1 plan §Global Constraints.
- Setting defaults are fixed by the spec: `cas_part_folder_cache_bytes = 64 MiB` (0 disables — a supported permanent operational configuration), `cas_part_folder_cache_max_entries = 10000`, `cas_part_folder_cache_max_entry_bytes = 16 MiB`.
- Never hold the single-flight mutex across `resolveRef` / `readManifestShared` / decode; `CacheBase` locks internally and is only touched with short operations.
- A failed validation/build is never cached; ref absence is never cached; `StrictValidate` neither consults nor populates.

---

### Task 1: Settings threading (on by default) {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` (the `content_addressed` registration), `.../ContentAddressed/ContentAddressedMetadataStorage.h` + `.cpp` (three new defaulted ctor params + members), `.../ContentAddressed/CachedPartFolderAccess.h` + `.cpp` (a `CacheParams` ctor argument)

**Interfaces:**
- Produces:

```cpp
    struct CacheParams
    {
        uint64_t cache_bytes = 0;            /// 0 = retention disabled (unit-test default;
                                             /// the DISK default is 64 MiB, set in the factory)
        uint64_t max_entries = 10000;
        uint64_t max_entry_bytes = 16ULL << 20;
    };
    CachedPartFolderAccess(Cas::StorePtr store_, CacheParams params_ = {});
```

- [ ] **Step 1: Add the config keys in the factory**

In `registerContentAddressedMetadataStorage`, next to `dedup_cache_bytes`:

```cpp
        /// Part-folder view cache (spec 2026-07-08-cas-part-folder-cache): ON by default;
        /// `cas_part_folder_cache_bytes = 0` disables retention — a supported permanent
        /// operational configuration (the runbook disable switch), not only a debug aid.
        const uint64_t cas_part_folder_cache_bytes = config.getUInt64(config_prefix + ".cas_part_folder_cache_bytes", 64ULL << 20);
        const uint64_t cas_part_folder_cache_max_entries = config.getUInt64(config_prefix + ".cas_part_folder_cache_max_entries", 10000);
        const uint64_t cas_part_folder_cache_max_entry_bytes = config.getUInt64(config_prefix + ".cas_part_folder_cache_max_entry_bytes", 16ULL << 20);
```

and append the three values to the `ContentAddressedMetadataStorage` construction call.

- [ ] **Step 2: Thread through the metadata storage**

`ContentAddressedMetadataStorage.h`: three new constructor parameters after `gc_max_conditional_put_bytes_`, all defaulted so unit-test constructions are unchanged:

```cpp
        uint64_t cas_part_folder_cache_bytes_ = 64ULL << 20,
        uint64_t cas_part_folder_cache_max_entries_ = 10000,
        uint64_t cas_part_folder_cache_max_entry_bytes_ = 16ULL << 20);
```

plus three `const uint64_t` members mirroring the existing setting members. In `startup`, the facade construction becomes:

```cpp
    part_access = std::make_unique<ContentAddressed::CachedPartFolderAccess>(cas_store,
        ContentAddressed::CachedPartFolderAccess::CacheParams{
            .cache_bytes = cas_part_folder_cache_bytes,
            .max_entries = cas_part_folder_cache_max_entries,
            .max_entry_bytes = cas_part_folder_cache_max_entry_bytes});
```

- [ ] **Step 3: Add `CacheParams` to the facade ctor (no behavior change yet — Task 2 uses it)**

```cpp
    CachedPartFolderAccess(Cas::StorePtr store_, CacheParams params_ = {})
        : store(std::move(store_)), params(params_) {}
```

- [ ] **Step 4: Build, run gtests, commit**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -5`
Expected: PASS (params unused so far).

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp
git commit -m "CAS wiring: part-folder cache settings threaded (on by default, bytes=0 disables)"
```

---

### Task 2: Retained map, validate-on-hit, single-flight, write-through {#task-2}

**Files:**
- Modify: `.../CachedPartFolderAccess.h`, `.../CachedPartFolderAccess.cpp`
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp` (Task 3 carries the tests; this task lands the mechanism plus one smoke test)

**Interfaces:**
- Consumes: `CacheParams` (Task 1), Phase-3 counters, `CurrentMetrics::CasPartFolderCacheBytes/Entries`.
- Produces: the final `getView` (spec §The Validate-On-Hit Protocol) and the filled-in `eraseView` at every `/// Phase 4: eraseView(key)` marker.

- [ ] **Step 1: Write the failing smoke test**

```cpp
namespace
{
ContentAddressed::CachedPartFolderAccess::CacheParams cacheOn()
{
    return {.cache_bytes = 64ULL << 20, .max_entries = 10000, .max_entry_bytes = 16ULL << 20};
}
}

TEST(CasPartFolderAccess, RetainedHitSkipsManifestHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    for (int i = 0; i < 5; ++i)
        ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// The one-GET goal (spec acceptance 4): ONE body GET, ONE mandatory HEAD (the cold build);
    /// every subsequent CachedForLoad call is a validated hit — zero manifest ops.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), 1u);
    EXPECT_TRUE(access.explain(key).retained);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Hit);
}
```

- [ ] **Step 2: Run to verify it fails** (`headCount == 5` — no retention yet).

- [ ] **Step 3: Implement**

Header — private state (include `<Common/CacheBase.h>`, `<Common/CurrentMetrics.h>`, `<future>`):

```cpp
    struct ViewWeight
    {
        size_t operator()(const PartFolderView & v) const { return v.estimatedBytes(); }
    };
    using ViewCache = CacheBase<String, PartFolderView, std::hash<String>, ViewWeight>;

    CacheParams params;
    /// nullptr <=> retention disabled (cache_bytes == 0): same call graph, no retained map.
    std::unique_ptr<ViewCache> view_cache;

    /// Single-flight per PartRefKey for the build path: concurrent cold builders of the same key
    /// share ONE readManifestShared. NEVER held across I/O — the map only hands out futures.
    mutable std::mutex inflight_mutex;
    mutable std::unordered_map<String, std::shared_future<std::shared_ptr<const PartFolderView>>> inflight;

    std::shared_ptr<const PartFolderView> buildView(
        const PartRefKey & key, const Cas::Resolved & resolved, Freshness freshness) const;
    void eraseView(const PartRefKey & key);
```

Ctor:

```cpp
CachedPartFolderAccess::CachedPartFolderAccess(Cas::StorePtr store_, CacheParams params_)
    : store(std::move(store_)), params(params_)
{
    if (params.cache_bytes > 0)
        view_cache = std::make_unique<ViewCache>(
            "LRU", CurrentMetrics::CasPartFolderCacheBytes, CurrentMetrics::CasPartFolderCacheEntries,
            params.cache_bytes, params.max_entries, ViewCache::DEFAULT_SIZE_RATIO);
}
```

The final `getView` (replaces the Phase-2 body; freshness-to-resolve mapping unchanged):

```cpp
std::shared_ptr<const PartFolderView>
CachedPartFolderAccess::getView(const PartRefKey & key, Freshness freshness) const
{
    /// Step 1 (spec §Validate-On-Hit): the SAME resolve every read already pays today. Absence is
    /// never retained.
    auto resolved = resolve(key, freshness);
    if (!resolved)
        return nullptr;

    /// Step 2: retained views serve ONLY CachedForLoad. ForceFresh must re-prove the manifest BODY
    /// (a fresh ref resolve proves ref currency, not body existence — review 2026-07-08);
    /// StrictValidate bypasses retention entirely.
    if (freshness == Freshness::CachedForLoad && view_cache)
    {
        if (auto cached = view_cache->get(key.cacheKey()))
        {
            if (cached->manifestId() == resolved->manifest_id)
            {
                if (cached->mutableFiles() == resolved->mutable_files)
                {
                    ProfileEvents::increment(ProfileEvents::CasPartFolderViewHits);
                    recordDecision(key, LastDecision::Hit, cached.get(), /*retained=*/true);
                    return cached;
                }
                /// 2b: manifest unchanged, mutable-only drift (txn_version bumps) — clone around
                /// the SAME shared decode; no manifest operation at all.
                auto refreshed = std::make_shared<PartFolderView>(
                    key, resolved->manifest_id, resolved->manifest_size,
                    resolved->published_at_ms, resolved->mutable_files, cached->manifest());
                if (refreshed->estimatedBytes() <= params.max_entry_bytes)
                    view_cache->set(key.cacheKey(), refreshed);
                ProfileEvents::increment(ProfileEvents::CasPartFolderViewMutableRefreshes);
                recordDecision(key, LastDecision::MutableRefresh, refreshed.get(), /*retained=*/true);
                return refreshed;
            }
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewValidationMismatches);
            /// fall through to rebuild — the stale entry is superseded by the insert below
        }
    }

    auto view = buildView(key, *resolved, freshness);

    /// Step 4: retain (StrictValidate never populates; oversized views are served, not retained).
    bool retained = false;
    if (freshness != Freshness::StrictValidate && view_cache)
    {
        if (view->estimatedBytes() <= params.max_entry_bytes)
        {
            /// CacheBase stores mutable pointers; views are logically const (never mutated).
            view_cache->set(key.cacheKey(), std::const_pointer_cast<PartFolderView>(view));
            retained = true;
        }
        else
            ProfileEvents::increment(ProfileEvents::CasPartFolderViewOversizedBypasses);
    }
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewMisses);
    recordDecision(key,
        freshness == Freshness::CachedForLoad ? (retained ? LastDecision::Miss : LastDecision::OversizedBypass)
        : freshness == Freshness::ForceFresh  ? LastDecision::ForceFreshRead
                                              : LastDecision::StrictBypass,
        view.get(), retained);
    return view;
}

std::shared_ptr<const PartFolderView> CachedPartFolderAccess::buildView(
    const PartRefKey & key, const Cas::Resolved & resolved, Freshness freshness) const
{
    /// Fresh modes must not coalesce onto another caller's read (each ForceFresh/StrictValidate
    /// call owns its mandatory HEAD); only cold CachedForLoad builds single-flight.
    if (freshness != Freshness::CachedForLoad)
        return PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id));

    std::promise<std::shared_ptr<const PartFolderView>> promise;
    std::shared_future<std::shared_ptr<const PartFolderView>> future;
    bool leader = false;
    {
        std::lock_guard lock(inflight_mutex);
        if (auto it = inflight.find(key.cacheKey()); it != inflight.end())
            future = it->second;                          /// follower: share the leader's build
        else
        {
            leader = true;
            future = promise.get_future().share();
            inflight.emplace(key.cacheKey(), future);
        }
    }
    if (!leader)
        return future.get();                              /// rethrows the leader's failure, if any

    SCOPE_EXIT({
        std::lock_guard lock(inflight_mutex);
        inflight.erase(key.cacheKey());
    });
    try
    {
        auto view = PartFolderView::make(key, resolved, store->readManifestShared(resolved.manifest_id));
        promise.set_value(view);
        return view;
    }
    catch (...)
    {
        promise.set_exception(std::current_exception());  /// followers see the leader's failure
        throw;
    }
}
```

`eraseView` + filling in every `/// Phase 4: eraseView(key)` marker from the Phase-2 primitives:

```cpp
void CachedPartFolderAccess::eraseView(const PartRefKey & key)
{
    if (view_cache)
        view_cache->remove(key.cacheKey());
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
    recordDecision(key, LastDecision::Invalidated, nullptr, /*retained=*/false);
}
```

- `promoteBuild`, `updateMutableFiles`, `dropRef`, `dropRefIfPresent` (also on the early-return absent path — cheap and harmless): `eraseView(key);` after the store call.
- `dropRefBestEffort`: `eraseView(key);` AFTER the try/catch — deliberately also on the swallowed-failure path (spec: the ref's durable state is unknown; dropping the view is the conservative direction).
- `dropNamespace`:

```cpp
void CachedPartFolderAccess::dropNamespace(const Cas::RootNamespace & ns)
{
    store->dropNamespace(ns);
    if (view_cache)
    {
        const String prefix = ns.string() + '\0';
        view_cache->remove([&](const String & k, const auto &) { return k.starts_with(prefix); });
    }
    ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations);
}
```

Also extend `recordDecision` with the `bool retained` parameter (updating the Phase-3 signature and its call sites) so `explain().retained` is truthful, and make `clearForTest` also `view_cache->clear()`.

- [ ] **Step 4: Run the smoke test + all facade tests**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.*' 2>&1 | tail -10`
Expected: PASS, including the Phase-3 baseline test (it constructs the facade WITHOUT `cacheOn()`, so retention is off there and its numbers are unchanged).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS wiring: retained part-folder views with validate-on-hit, single-flight, write-through erases"
```

---

### Task 3: The semantics test battery {#task-3}

**Files:**
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

Each test below is one checkbox: write it, run it (`--gtest_filter='CasPartFolderAccess.<Name>'`), see PASS (or fix the Task-2 mechanism — these encode the spec's acceptance criteria), then one commit at the end.

- [ ] **Step 1: `MutableRefreshWithoutManifestRead` + `WriteThroughEraseThenRebuild`** — the two mutable-drift shapes. The CLONE path (protocol step 2b) triggers only when a retained entry SURVIVES a mutable change, so the first test mutates through the RAW store (the facade's own `updateMutableFiles` would write-through-erase the entry — that shape is the second test):

```cpp
TEST(CasPartFolderAccess, MutableRefreshWithoutManifestRead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")}, {{"txn_version.txt", "v1"}});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    /// RAW-store mutation: the write bypasses the facade (validate-on-hit must cope — this is the
    /// mutation-site-not-routed / foreign-writer shape the compare exists for). The retained entry
    /// survives, so the next read exercises the manifest-match + mutable-drift CLONE path.
    store->updateRefPayload(ns, "part_1", [](Cas::RootRef & p) { p.mutable_files["txn_version.txt"] = "v2"; });
    backend->resetCounts();

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v2"));   /// read-your-writes
    EXPECT_EQ(backend->headCount(manifest_key), 0u);                                 /// clone: no HEAD
    EXPECT_EQ(backend->getCount(manifest_key), 0u);                                  /// and no GET
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::MutableRefresh);
}

TEST(CasPartFolderAccess, WriteThroughEraseThenRebuild)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")}, {{"txn_version.txt", "v1"}});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);
    access.updateMutableFiles(key, [](Cas::RootRef & p) { p.mutable_files["txn_version.txt"] = "v2"; });
    backend->resetCounts();

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v2"));
    EXPECT_EQ(backend->headCount(manifest_key), 1u);   /// erase => cold rebuild re-HEADs...
    EXPECT_EQ(backend->getCount(manifest_key), 0u);    /// ...but the decode cache absorbs the GET
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Miss);
}
```

- [ ] **Step 2: `MismatchRebuildAfterRepublish`** — drop + republish the same ref name with DIFFERENT content through the raw Core protocol (no facade => no erase), then `CachedForLoad`:

```cpp
    store->dropRef(ns, "part_1");
    publishPart(store, ns, "part_1", {inlineEntry("f", "DIFFERENT")});
    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("f"), nullptr);
    EXPECT_EQ(view->findFile("f")->inline_bytes, "DIFFERENT");    /// never the stale view
```

and assert the `CasPartFolderViewValidationMismatches`-recorded decision via `explain` (`Miss` after rebuild) plus one new manifest `GET`.

- [ ] **Step 3: `ForceFreshFailsClosedWhileRetainedViewExists`** — THE review-critical test (spec Testing bullet):

```cpp
TEST(CasPartFolderAccess, ForceFreshFailsClosedWhileRetainedViewExists)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("f", "x")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);   /// retained
    deleteManifestBody(*backend, layout, id);   /// protocol violation: live body vanishes

    /// Write-evidence and strict paths surface INV-NO-DANGLE immediately (mandatory HEAD)...
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, ContentAddressed::Freshness::ForceFresh); });
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, ContentAddressed::Freshness::StrictValidate); });

    /// ...while a validated CachedForLoad hit still serves the immutable decode — the documented
    /// residual delta (spec §Staleness Equivalence): detection deferred, never for write evidence.
    EXPECT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);
}
```

- [ ] **Step 4: `AbsenceIsNeverRetained`** — `dropRef` via facade → `getView` nullptr; re-publish → `getView` non-null immediately (both `CachedForLoad`).

- [ ] **Step 5: `OversizedViewServedNotRetained`** — facade with `max_entry_bytes = 1`: `getView` returns a valid view, `explain().retained == false`, second call re-HEADs (headCount grows), `CasPartFolderViewOversizedBypasses` recorded via `explain` = `OversizedBypass`.

- [ ] **Step 6: `DisabledModeKeepsBaseline`** — facade with `CacheParams{}` (bytes 0): re-assert the exact Phase-3 baseline numbers (1 `GET`, N `HEAD`s), proving `bytes=0` restores no-retention behavior.

- [ ] **Step 7: `SingleFlightColdBuild`** — K=8 `std::thread`s calling `getView(key, CachedForLoad)` on a cold cache (barrier: `std::latch`); join; assert `getCount(manifest_key) == 1` and every thread got a non-null view. No sleeps — the latch releases all threads at once and the single-flight future does the rest.

- [ ] **Step 8: `DropNamespaceErasesAllViews`** — publish two parts, warm both views, `access.dropNamespace(ns)`, re-publish one part, assert `getView` for it works and `explain(other).retained == false`.

- [ ] **Step 9: Run the whole battery + all `Cas*` gtests + the CA functional suite** (same stateless list as the Phase 1 plan, log `build_debug/test_phase4_final.log`). Expected: all pass.

- [ ] **Step 10: Commit**

```bash
git add src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS wiring: retention semantics battery — validate-on-hit, fail-closed ForceFresh, single-flight, disable switch"
```

---

### Task 4: Soak validation (out-of-CI gate) {#task-4}

- [ ] **Step 1:** Run one `ca-soak` lane (CA-local + CA-S3, per `utils/ca-soak` conventions) with retention enabled (default) and one with `<cas_part_folder_cache_bytes>0</cas_part_folder_cache_bytes>` in the disk config; compare `system.content_addressed_log` anomaly baselines and the new `CasPartFolderView*` counters between the two runs. Expected: no new anomaly classes; hit-rate counters nonzero in the enabled run.
- [ ] **Step 2:** Record the outcome in `utils/ca-soak/scenarios/RUN_HISTORY.md` (existing convention).

## Phase acceptance {#phase-acceptance}

The one-`GET` goal holds (Task 2 smoke + Task 3 battery); staleness-equivalence tests pass; `ForceFresh` re-proves the body per call; the cache is on by default and `bytes=0` restores the exact baseline; all counters live; soak lanes clean.
