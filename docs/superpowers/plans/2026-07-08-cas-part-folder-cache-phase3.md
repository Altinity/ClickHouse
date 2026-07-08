---
description: "Implementation plan, Phase 3 of the CAS part-folder cache: ProfileEvents/CurrentMetrics counters, the explain surface, and request-count baselines with retention still off."
sidebar_label: "CAS Part Folder Cache Plan P3"
sidebar_position: 16
slug: "/superpowers/plans/2026-07-08-cas-part-folder-cache-phase3"
title: "CAS Part Folder Cache — Phase 3 Plan"
doc_type: "reference"
---

# CAS Part Folder Cache — Phase 3: Observability {#phase-3-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** make every facade decision visible — counters, the `explain` surface, and request-count baselines — before retention (Phase 4) can hide repeated work.

**Architecture:** eight ProfileEvents + two CurrentMetrics; `CasPartFolderManifestGets` is incremented inside `Store::readManifestShared` at the actual body `get` site (the facade cannot see whether the decode cache absorbed the `GET`); `explain` is a bounded in-facade decision map, test/log-only. Requires Phase 2 merged. Spec: `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md` §Observability.

**Tech Stack:** C++, `src/Common/ProfileEvents.cpp` / `CurrentMetrics.cpp` macro tables, gtest with `CountingBackend`.

## Global Constraints {#global-constraints}

- Same conventions as the Phase 1 plan §Global Constraints (branch, Allman, no sleeps, build/test commands).
- Counter names are final here (they entered the spec as "names final at implementation") — later phases must use exactly these.

---

### Task 1: Register the counters {#task-1}

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (append to the existing `Cas*` block, near line ~739), `src/Common/CurrentMetrics.cpp`

**Interfaces:**
- Produces (used by Tasks 2-3 and Phase 4):
  ProfileEvents `CasPartFolderViewHits`, `CasPartFolderViewMutableRefreshes`, `CasPartFolderViewValidationMismatches`, `CasPartFolderViewMisses`, `CasPartFolderViewEvictions`, `CasPartFolderViewOversizedBypasses`, `CasPartFolderViewInvalidations`, `CasPartFolderManifestGets`; CurrentMetrics `CasPartFolderCacheBytes`, `CasPartFolderCacheEntries`.

- [ ] **Step 1: Add the entries**

`ProfileEvents.cpp`, after the existing `Cas*` events (keep the trailing `\` continuation style of the table):

```cpp
    M(CasPartFolderViewHits, "CA part-folder view cache validated hits (retained view matched a fresh resolve)", ValueType::Number) \
    M(CasPartFolderViewMutableRefreshes, "CA part-folder view refreshes: manifest unchanged, mutable payload drifted — view cloned, no manifest read", ValueType::Number) \
    M(CasPartFolderViewValidationMismatches, "CA part-folder view validation mismatches: the manifest changed under a retained view — rebuilt", ValueType::Number) \
    M(CasPartFolderViewMisses, "CA part-folder view cold builds (no retained entry consulted or found)", ValueType::Number) \
    M(CasPartFolderViewEvictions, "CA part-folder view LRU evictions", ValueType::Number) \
    M(CasPartFolderViewOversizedBypasses, "CA part-folder views built but not retained (estimated weight above the per-entry cap)", ValueType::Number) \
    M(CasPartFolderViewInvalidations, "CA part-folder view write-through erases (promote, mutable update, drop ref, drop namespace)", ValueType::Number) \
    M(CasPartFolderManifestGets, "CA part-manifest body GET requests issued by readManifestShared (the part-folder cache acceptance metric)", ValueType::Number) \
```

`CurrentMetrics.cpp` (same macro-table style):

```cpp
    M(CasPartFolderCacheBytes, "Estimated bytes retained by the CA part-folder view cache") \
    M(CasPartFolderCacheEntries, "Entries retained by the CA part-folder view cache") \
```

- [ ] **Step 2: Build (registration is compile-checked), commit**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -3 build_debug/build_unit_tests.log`
Expected: clean build.

```bash
git add src/Common/ProfileEvents.cpp src/Common/CurrentMetrics.cpp
git commit -m "CAS observability: part-folder view cache ProfileEvents + CurrentMetrics"
```

---

### Task 2: Facade counters and `explain` {#task-2}

**Files:**
- Modify: `.../ContentAddressed/CachedPartFolderAccess.h`, `.../CachedPartFolderAccess.cpp`, `.../Core/CasStore.cpp` (`readManifestShared` GET counter)
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

**Interfaces:**
- Produces:

```cpp
    enum class LastDecision : uint8_t
    { Hit, MutableRefresh, Mismatch, Miss, OversizedBypass, StrictBypass, ForceFreshRead, Invalidated };
    struct ExplainResult
    {
        bool retained = false;             /// false throughout Phase 3 (no retained map yet)
        LastDecision last_decision = LastDecision::Miss;
        String manifest_ref;               /// manifestRefDebugString of the last-served view
        size_t estimated_bytes = 0;
    };
    ExplainResult explain(const PartRefKey & key) const;   /// test/log-only; absent key => default
    void clearForTest();
```

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasPartFolderAccess, ExplainRecordsDecisions)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Miss);       /// cold build
    EXPECT_FALSE(access.explain(key).retained);                                    /// Phase 3: never

    access.getView(key, ContentAddressed::Freshness::ForceFresh);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::ForceFreshRead);

    access.getView(key, ContentAddressed::Freshness::StrictValidate);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::StrictBypass);

    access.dropRef(key);
    EXPECT_EQ(access.explain(key).last_decision,
              ContentAddressed::CachedPartFolderAccess::LastDecision::Invalidated);
    EXPECT_GT(access.explain(key).estimated_bytes, 0u);
}
```

- [ ] **Step 2: Run to verify compile failure** (as usual: `ninja ... ; tail -5 <log>` — `explain` undeclared).

- [ ] **Step 3: Implement**

Header: add the enum/struct/method declarations above, plus the private state:

```cpp
    /// Decision journal for explain (test/log-only; spec §Observability). Bounded by wholesale
    /// clear — debug state, never consulted by the read/write paths.
    static constexpr size_t EXPLAIN_MAX_ENTRIES = 10000;
    mutable std::mutex explain_mutex;
    mutable std::unordered_map<String, ExplainResult> explain_map;
    void recordDecision(const PartRefKey & key, LastDecision decision,
                        const PartFolderView * view) const;
```

`CachedPartFolderAccess.cpp`:

```cpp
void CachedPartFolderAccess::recordDecision(const PartRefKey & key, LastDecision decision,
                                            const PartFolderView * view) const
{
    std::lock_guard lock(explain_mutex);
    if (explain_map.size() >= EXPLAIN_MAX_ENTRIES)
        explain_map.clear();
    auto & e = explain_map[key.cacheKey()];
    e.last_decision = decision;
    if (view)
    {
        e.manifest_ref = Cas::manifestRefDebugString(view->manifestId().ref);
        e.estimated_bytes = view->estimatedBytes();
    }
    /// `retained` stays false until Phase 4 sets it on insert/erase.
}

CachedPartFolderAccess::ExplainResult CachedPartFolderAccess::explain(const PartRefKey & key) const
{
    std::lock_guard lock(explain_mutex);
    const auto it = explain_map.find(key.cacheKey());
    return it == explain_map.end() ? ExplainResult{} : it->second;
}

void CachedPartFolderAccess::clearForTest()
{
    std::lock_guard lock(explain_mutex);
    explain_map.clear();
}
```

Wire the decisions and counters into the existing methods:

- `getView`: after a successful build, `ProfileEvents::increment(ProfileEvents::CasPartFolderViewMisses)` and `recordDecision(key, freshness == Freshness::CachedForLoad ? LastDecision::Miss : freshness == Freshness::ForceFresh ? LastDecision::ForceFreshRead : LastDecision::StrictBypass, view.get())`.
- Each write primitive (`promoteBuild`, `updateMutableFiles`, `dropRef`, `dropRefIfPresent` when it dropped, `dropRefBestEffort`, `dropNamespace`): `ProfileEvents::increment(ProfileEvents::CasPartFolderViewInvalidations)` and `recordDecision(key, LastDecision::Invalidated, nullptr)` (for `dropNamespace`, skip per-key recording — nothing enumerates the namespace cheaply until Phase 4's predicate erase).

`Core/CasStore.cpp`, `readManifestShared`: add to the ProfileEvents extern block `extern const Event CasPartFolderManifestGets;` and increment right after the successful body `get`:

```cpp
    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "manifest at {} vanished between head and get — INV-NO-DANGLE", key);
    ProfileEvents::increment(ProfileEvents::CasPartFolderManifestGets);
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.*' 2>&1 | tail -10`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS observability: facade decision counters + explain surface; manifest-GET acceptance counter"
```

---

### Task 3: Request-count baselines (retention off) {#task-3}

**Files:**
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

**Interfaces:**
- Consumes: `CountingBackend` per-key `HEAD`/`GET` counts, `Cas::Layout::manifestKey`.
- Produces: the documented no-retention baseline that Phase 4's one-`GET` tests improve on.

- [ ] **Step 1: Write the baseline test (passes immediately — it documents current behavior)**

```cpp
TEST(CasPartFolderAccess, BaselineRequestCountsWithoutRetention)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    const String manifest_key = layout.manifestKey(id);

    backend->resetCounts();
    constexpr int n = 5;
    for (int i = 0; i < n; ++i)
        ASSERT_NE(access.getView(key, ContentAddressed::Freshness::CachedForLoad), nullptr);

    /// The Phase-3 baseline (retention off): one manifest-body GET (the decode cache absorbs the
    /// rest) but a mandatory manifest HEAD per call. Phase 4's validated hits remove the HEADs;
    /// this test pins the numbers Phase 4 improves.
    EXPECT_EQ(backend->getCount(manifest_key), 1u);
    EXPECT_EQ(backend->headCount(manifest_key), static_cast<uint64_t>(n));
}
```

- [ ] **Step 2: Run to verify it passes**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.Baseline*' 2>&1 | tail -10`
Expected: PASS. (If the `HEAD` count differs, STOP: the facade routing has an extra or missing resolve/read — fix the routing, not the test.)

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS observability: no-retention request-count baseline for the part-folder facade"
```

## Phase acceptance {#phase-acceptance}

Every facade decision is visible (counters + `explain`); the manifest-`GET` acceptance counter exists at the true `GET` site; the baseline numbers are pinned by a test. All `Cas*` gtests green.
