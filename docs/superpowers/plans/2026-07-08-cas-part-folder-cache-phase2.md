---
description: "Implementation plan, Phase 2 of the CAS part-folder cache: the no-retention CachedPartFolderAccess facade, full read/write routing, adoptPartFromManifest consolidation, and the check-style guard."
sidebar_label: "CAS Part Folder Cache Plan P2"
sidebar_position: 15
slug: "/superpowers/plans/2026-07-08-cas-part-folder-cache-phase2"
title: "CAS Part Folder Cache — Phase 2 Plan"
doc_type: "reference"
---

# CAS Part Folder Cache — Phase 2: No-Retention Facade And Write Routing {#phase-2-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** introduce `CachedPartFolderAccess` (no retained state yet), route every normal part/projection read and every committed part-ref mutation through it, consolidate `adoptPartFromManifest` onto `publishEntries`, delete the now-dead `Store` tree helpers, and land the check-style guard.

**Architecture:** the facade owns freshness policy and the terminal committed-ref primitives; `ContentAddressedMetadataStorage` keeps path semantics only; `ContentAddressedTransaction` keeps `Cas::Build` staging orchestration and calls facade primitives at every committed-ref boundary. `partAccess()` returns a **reference** (dot-call syntax) — load-bearing for the style rule, which bans `->`-syntax mutation tokens in wiring. Spec: `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md`. Requires Phase 1 merged.

**Tech Stack:** C++, gtest, `ci/jobs/scripts/check_style/check_cpp.sh` (bash + `rg`).

## Global Constraints {#global-constraints}

- Same branch/build/test conventions as the Phase 1 plan (`docs/superpowers/plans/2026-07-08-cas-part-folder-cache-phase1.md` §Global Constraints): Allman braces, no C++ sleeps, `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1`, gtest filter runs, praktika functional suite at phase end.
- `Freshness` mapping is fixed by the spec: `CachedForLoad` → `resolveRef(..., allow_stale=true)`; `ForceFresh`/`StrictValidate` → `allow_stale=false`; `getView` ALWAYS calls `readManifestShared` in Phase 2 (no retained views exist yet).
- Every committed part-ref mutation in wiring must end up as a dot-syntax call on `partAccess()`; raw `->dropRef(` / `->updateRefPayload(` / `->dropNamespace(` / `->promote(` must not survive in wiring files outside `CachedPartFolderAccess.*`.

---

### Task 1: `CachedPartFolderAccess` — read side {#task-1}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h`, `.../CachedPartFolderAccess.cpp`
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp` (new)

**Interfaces:**
- Consumes: `Cas::Store` (`resolveRef`, `readManifestShared`, `startBuild`, `dropRef`, `updateRefPayload`, `dropNamespace`), `PartFolderView`, `PartRefKey`, `Freshness`.
- Produces (all later tasks and phases use these exact signatures):

```cpp
namespace DB::ContentAddressed
{
class CachedPartFolderAccess
{
public:
    explicit CachedPartFolderAccess(Cas::StorePtr store_);

    std::shared_ptr<const PartFolderView> getView(const PartRefKey & key, Freshness freshness) const;
    std::optional<Cas::Resolved> resolve(const PartRefKey & key, Freshness freshness) const;
    bool existsRef(const PartRefKey & key, Freshness freshness) const;

    void promoteBuild(Cas::Build & build, const PartRefKey & key, UInt128 build_id,
                      const Cas::ManifestId & manifest_id, std::map<String, String> mutable_files);
    void publishEntries(const PartRefKey & dst, const std::vector<Cas::ManifestEntry> & entries,
                        std::map<String, String> mutable_files, Cas::ProvenanceOp op);
    bool republishRef(const PartRefKey & src, const PartRefKey & dst);
    void updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator);
    void dropRef(const PartRefKey & key);
    void dropRefIfPresent(const PartRefKey & key);
    void dropRefBestEffort(const PartRefKey & key) noexcept;
    void dropNamespace(const Cas::RootNamespace & ns);

private:
    Cas::StorePtr store;
};
}
```

(Write ops are declared here but implemented in Tasks 4-5; Task 1 implements the ctor + the three reads and stubs nothing — declare only what this task defines, then extend the header in Tasks 4-5.)

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_part_folder_access.cpp`. Committed refs are produced at the Core level through the real writer protocol (`startBuild` → `stageManifest` → `precommitAdd` → `promote`) with inline-only entries (no blob uploads needed — `adoptEvidence` and the promote gate skip inline entries):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/tests/cas_test_helpers.h>
#include <gtest/gtest.h>

namespace DB::ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int ABORTED;
}

using namespace DB;
using namespace DB::Cas::tests;

namespace
{

Cas::ManifestEntry inlineEntry(const String & path, const String & bytes)
{
    Cas::ManifestEntry e;
    e.path = path;
    e.placement = Cas::EntryPlacement::Inline;
    e.blob_hash = u128Of(bytes);
    e.blob_size = bytes.size();
    e.inline_bytes = bytes;
    return e;
}

/// Publish `entries` as committed ref `ns/ref` through the real writer protocol.
Cas::ManifestId publishPart(const Cas::StorePtr & store, const Cas::RootNamespace & ns,
                            const String & ref, std::vector<Cas::ManifestEntry> entries,
                            std::map<String, String> mutable_files = {})
{
    auto build = store->startBuild(Cas::BuildInfo{.intended_ref = ns.string() + "/" + ref,
                                                  .intended_namespace = ns, .op = Cas::ProvenanceOp::Insert});
    const Cas::ManifestId id = build->stageManifest(entries);
    build->precommitAdd(ns, ref, id);
    build->setPendingMutableFiles(std::move(mutable_files));
    build->promote(ns, ref, build->buildId(), id);
    return id;
}

}

TEST(CasPartFolderAccess, GetViewServesCommittedFolder)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs"), inlineEntry("count.txt", "1")},
                {{"txn_version.txt", "v1"}});

    ContentAddressed::CachedPartFolderAccess access(store);
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    auto view = access.getView(key, ContentAddressed::Freshness::CachedForLoad);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("checksums.txt"), nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v1"));

    /// Absent ref => nullptr, never an exception, never retained (nothing to retain in Phase 2).
    EXPECT_EQ(access.getView({ns, "absent"}, ContentAddressed::Freshness::CachedForLoad), nullptr);
    EXPECT_TRUE(access.existsRef(key, ContentAddressed::Freshness::CachedForLoad));
    EXPECT_FALSE(access.existsRef({ns, "absent"}, ContentAddressed::Freshness::ForceFresh));
    ASSERT_TRUE(access.resolve(key, ContentAddressed::Freshness::ForceFresh).has_value());
}

TEST(CasPartFolderAccess, GetViewFailsClosedOnMissingBody)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    /// Physically delete the live manifest body (a protocol violation) — every getView mode must
    /// surface INV-NO-DANGLE as FILE_DOESNT_EXIST in Phase 2 (there is no retained view to hit).
    deleteManifestBody(*backend, layout, id);

    ContentAddressed::CachedPartFolderAccess access(store);
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    for (auto freshness : {ContentAddressed::Freshness::CachedForLoad,
                           ContentAddressed::Freshness::ForceFresh,
                           ContentAddressed::Freshness::StrictValidate})
        expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST, [&] { access.getView(key, freshness); });
}
```

- [ ] **Step 2: Run to verify compile failure**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `CachedPartFolderAccess.h` does not exist.

- [ ] **Step 3: Implement the read side**

`CachedPartFolderAccess.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace DB::Cas { class Build; }

namespace DB::ContentAddressed
{

/// The single facade for committed content-addressed part-folder access (spec
/// 2026-07-08-cas-part-folder-cache). Reads build immutable `PartFolderView`s; committed part-ref
/// mutations are facade methods so cache effects (Phase 4) are write-through, never a caller
/// responsibility. Phase-2 shape: NO retained state — `getView` builds a fresh view per call; the
/// call graph is already final, retention (Phase 4) only adds the retained-map consultation.
/// Thread-safe; shared by all readers and transactions of one disk.
class CachedPartFolderAccess
{
public:
    explicit CachedPartFolderAccess(Cas::StorePtr store_) : store(std::move(store_)) {}

    /// Resolve + validated manifest read, joined into a view. nullptr = the ref is absent.
    /// EVERY mode re-proves the manifest body via `readManifestShared`'s mandatory HEAD in this
    /// phase; a fresh ref resolve alone proves ref currency, NOT body existence (review 2026-07-08).
    std::shared_ptr<const PartFolderView> getView(const PartRefKey & key, Freshness freshness) const;

    /// Ref-only resolution (mutable per-part reads, part-dir existence, publish stamps): no
    /// manifest is read. `CachedForLoad` = stale-tolerant; other modes force-fresh.
    std::optional<Cas::Resolved> resolve(const PartRefKey & key, Freshness freshness) const;
    bool existsRef(const PartRefKey & key, Freshness freshness) const;

private:
    Cas::StorePtr store;
};

}
```

`CachedPartFolderAccess.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>

namespace DB::ContentAddressed
{

std::shared_ptr<const PartFolderView>
CachedPartFolderAccess::getView(const PartRefKey & key, Freshness freshness) const
{
    auto resolved = resolve(key, freshness);
    if (!resolved)
        return nullptr;   /// absence is never cached
    /// Fail-closed exactly as before: a live ref naming a missing body throws FILE_DOESNT_EXIST
    /// (INV-NO-DANGLE surfaced); corrupt bodies throw CORRUPTED_DATA and are never cached.
    return PartFolderView::make(key, *resolved, store->readManifestShared(resolved->manifest_id));
}

std::optional<Cas::Resolved>
CachedPartFolderAccess::resolve(const PartRefKey & key, Freshness freshness) const
{
    return store->resolveRef(key.ns, key.ref, /*allow_stale=*/freshness == Freshness::CachedForLoad);
}

bool CachedPartFolderAccess::existsRef(const PartRefKey & key, Freshness freshness) const
{
    return resolve(key, freshness).has_value();
}

}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.*' 2>&1 | tail -10`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS wiring: CachedPartFolderAccess facade, read side (no retention)"
```

---

### Task 2: Metadata storage owns the facade; reads routed through it {#task-2}

**Files:**
- Modify: `.../ContentAddressed/ContentAddressedMetadataStorage.h`, `.../ContentAddressedMetadataStorage.cpp`

**Interfaces:**
- Produces: `ContentAddressed::CachedPartFolderAccess & ContentAddressedMetadataStorage::partAccess() const` — a REFERENCE, so all call sites use dot syntax (`partAccess().getView(...)`), which is what lets the Task 7 style rule ban `->`-syntax mutation tokens. Throws `LOGICAL_ERROR` before `startup` (mirror of `store`). `resolveRouted` is deleted.

- [ ] **Step 1: Add the member and accessor**

Header: include `CachedPartFolderAccess.h`; in the private section next to `cas_store`:

```cpp
    /// The part-folder access facade (spec 2026-07-08-cas-part-folder-cache): the ONLY normal path
    /// for committed part/projection reads and committed part-ref mutations. Constructed in
    /// startup right after Store::open; reset in shutdown before cas_store.
    std::unique_ptr<ContentAddressed::CachedPartFolderAccess> part_access;
```

Public (next to `store()`):

```cpp
    /// The facade, by REFERENCE (dot-syntax call sites — the committed-ref style guard bans raw
    /// `->` mutation tokens in wiring). Throws LOGICAL_ERROR before startup, like store().
    ContentAddressed::CachedPartFolderAccess & partAccess() const;
```

`ContentAddressedMetadataStorage.cpp`:

```cpp
ContentAddressed::CachedPartFolderAccess & ContentAddressedMetadataStorage::partAccess() const
{
    if (!part_access)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ContentAddressedMetadataStorage: partAccess accessed before startup");
    return *part_access;
}
```

In `startup`, right after `cas_store = Cas::Store::open(...)` and the `pool_uuid` line:

```cpp
    part_access = std::make_unique<ContentAddressed::CachedPartFolderAccess>(cas_store);
```

In `shutdown`, before `cas_store.reset()`:

```cpp
    part_access.reset();
```

- [ ] **Step 2: Route all reads through the facade and delete `resolveRouted`**

Mechanical substitutions in `ContentAddressedMetadataStorage.cpp` (using `Freshness` = `ContentAddressed::Freshness`):

- Every `auto view = resolveRouted(*r);` → `auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);`
- The shadow-part-dir `listDirectory` branch builds the route inline: `auto view = partAccess().getView(Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""}.refKey(), ContentAddressed::Freshness::CachedForLoad);`
- Mutable-file force-fresh resolves (in `existsFile` and `tryGetInManifestBytes`): `auto resolved = store()->resolveRef(r->ns, r->ref);` → `auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);`
- Part-dir existence in `existsDirectory` (both the shadow and live branches): `store()->resolveRef(..., /*allow_stale=*/true).has_value()` → `partAccess().existsRef(<key>, ContentAddressed::Freshness::CachedForLoad)` where `<key>` is `Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""}.refKey()` / `r->refKey()`.
- `getLastModified`'s `resolve_stamp`: `store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true)` → `partAccess().resolve(r.refKey(), ContentAddressed::Freshness::CachedForLoad)`.
- `getPartManifestBytes`: replace the `resolveRef` + `readManifestShared` pair with a `ForceFresh` view (fresh evidence for part exchange, body re-proven by the mandatory `HEAD`):

```cpp
    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::ForceFresh);
    if (!view)
        return std::nullopt;
    return Cas::encodePartManifest(*view->manifest());
```

- Delete `resolveRouted` (header + cpp).

- [ ] **Step 3: Build, run gtests**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
git commit -m "CAS wiring: metadata storage reads route through CachedPartFolderAccess"
```

---

### Task 3: Kill the two-read traps {#task-3}

**Files:**
- Modify: `.../ContentAddressedMetadataStorage.h` (declare the `getStorageObjectsIfExist` override), `.../ContentAddressedMetadataStorage.cpp`

**Interfaces:**
- Consumes: `partAccess()` (Task 2), `PartFolderView` queries.
- Produces: `getStorageObjectsIfExist` override (one routed lookup instead of `existsFile` + `getStorageObjects`); `getFileSize` with a single routed resolve; `existsFileOrDirectory` with a single view query for in-part paths.

- [ ] **Step 1: Implement the three methods**

Header, next to `getStorageObjects`:

```cpp
    /// Single-lookup override (spec §Method Routing): the inherited default is existsFile +
    /// getStorageObjects — a two-read trap for CAS on the readFileIfExists path.
    std::optional<StoredObjects> getStorageObjectsIfExist(const std::string & path) const override;
```

`getStorageObjectsIfExist`:

```cpp
std::optional<StoredObjects> ContentAddressedMetadataStorage::getStorageObjectsIfExist(const std::string & path) const
{
    /// Non-part shapes (verbatim table files, loose mountpoint objects) are rare paths — the
    /// generic two-step is fine for them.
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (existsFile(path))
            return getStorageObjects(path);
        return std::nullopt;
    }
    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        return std::nullopt;
    auto r = route(*p);
    if (!r || r->file.empty())
        return std::nullopt;

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        /// Force-fresh (Pillar B), same contract as existsFile/tryGetInManifestBytes.
        auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);
        if (!resolved || ContentAddressed::PartFolderView::isReservedMutableName(r->file))
            return std::nullopt;
        const auto it = resolved->mutable_files.find(r->file);
        if (it == resolved->mutable_files.end())
            return std::nullopt;
        /// Sized empty-key placeholder — same shape getStorageObjects returns for in-manifest bytes.
        return StoredObjects{StoredObject("", path, it->second.size())};
    }

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
    if (!view)
        return std::nullopt;
    const auto * entry = view->findFile(r->file);
    if (!entry)
        return std::nullopt;
    if (entry->placement == Cas::EntryPlacement::Inline)
        return StoredObjects{StoredObject("", path, entry->inline_bytes.size())};
    const auto location = store()->locate(*entry);
    return StoredObjects{StoredObject(location.key, path, location.length)};
}
```

`getFileSize` — restructure to ONE routed resolve (replaces the `tryGetInManifestBytes`-first shape):

```cpp
uint64_t ContentAddressedMetadataStorage::getFileSize(const std::string & path) const
{
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto bytes = tryGetInManifestBytes(path))   /// verbatim table-level file
            return bytes->size();
        if (auto bytes = store()->getMountpointObject(serverPrefix() + "/" + path))
            return bytes->size();
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto p = ContentAddressed::parsePartFilePath(path);
    if (!p || p->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);
    auto r = route(*p);
    if (!r || r->file.empty())
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: not a part file path: {}", path);

    if (ContentAddressed::isMutablePerPartFile(r->file))
    {
        /// Force-fresh (Pillar B): read-your-writes for a just-written mutable file.
        auto resolved = partAccess().resolve(r->refKey(), ContentAddressed::Freshness::ForceFresh);
        if (resolved && !ContentAddressed::PartFolderView::isReservedMutableName(r->file))
            if (auto it = resolved->mutable_files.find(r->file); it != resolved->mutable_files.end())
                return it->second.size();
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }

    auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
    if (!view)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    if (auto size = view->fileSize(r->file))
        return *size;
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", r->file, path);
}
```

`existsFileOrDirectory` — one view query for committed in-part paths, generic fallback elsewhere:

```cpp
bool ContentAddressedMetadataStorage::existsFileOrDirectory(const std::string & path) const
{
    if (ContentAddressed::isPartFilePath(path))
    {
        auto p = ContentAddressed::parsePartFilePath(path);
        auto r = p ? route(*p) : std::nullopt;
        if (r && !r->ref.empty() && !r->file.empty() && !ContentAddressed::isMutablePerPartFile(r->file))
        {
            auto view = partAccess().getView(r->refKey(), ContentAddressed::Freshness::CachedForLoad);
            if (!view)
                return false;
            return view->hasFile(r->file) || view->hasDirectory(r->file + "/");
        }
    }
    return existsFile(path) || existsDirectory(path);
}
```

- [ ] **Step 2: Build, run gtests + the CA functional suite**

Run the gtest batch as in Task 2, then the full stateless CA list (same command as the Phase 1 plan, Task 6 Step 4, log to `build_debug/test_phase2_reads.log`).
Expected: all pass — these are behavior-preserving request-count fixes.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
git commit -m "CAS wiring: getStorageObjectsIfExist override + single-resolve getFileSize/existsFileOrDirectory"
```

---

### Task 4: Facade write primitives; transaction conversion {#task-4}

**Files:**
- Modify: `.../CachedPartFolderAccess.h`, `.../CachedPartFolderAccess.cpp` (add the five primitives), `.../ContentAddressedTransaction.h` (drop `dropRefIfPresent` declaration), `.../ContentAddressedTransaction.cpp` (all mutation sites)
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

**Interfaces:**
- Produces: `promoteBuild`, `updateMutableFiles`, `dropRef`, `dropRefIfPresent`, `dropRefBestEffort` (`noexcept`), `dropNamespace` — signatures as listed in Task 1. In Phase 2 they have no cache effects (there is no cache); each carries a `/// Phase 4: eraseView(key)` marker comment so the write-through erase lands in exactly one place per primitive.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasPartFolderAccess, WritePrimitivesRoundTrip)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    /// promoteBuild: the transaction's terminal publish step, through the facade.
    auto build = store->startBuild(Cas::BuildInfo{.intended_ref = ns.string() + "/part_1",
                                                  .intended_namespace = ns, .op = Cas::ProvenanceOp::Insert});
    const Cas::ManifestId id = build->stageManifest({inlineEntry("checksums.txt", "cs")});
    build->precommitAdd(ns, "part_1", id);
    access.promoteBuild(*build, key, build->buildId(), id, {{"txn_version.txt", "v1"}});
    ASSERT_TRUE(access.existsRef(key, ContentAddressed::Freshness::ForceFresh));

    /// updateMutableFiles is visible to a force-fresh resolve immediately.
    access.updateMutableFiles(key, [](Cas::RootRef & payload) { payload.mutable_files["txn_version.txt"] = "v2"; });
    auto resolved = access.resolve(key, ContentAddressed::Freshness::ForceFresh);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->mutable_files.at("txn_version.txt"), "v2");

    /// dropRefIfPresent: replay-safe (absent ref is success, not failure).
    access.dropRefIfPresent(key);
    EXPECT_FALSE(access.existsRef(key, ContentAddressed::Freshness::ForceFresh));
    access.dropRefIfPresent(key);                              /// second drop: no-op, no throw
    access.dropRefBestEffort(key);                             /// noexcept even when absent

    /// dropNamespace clears the whole namespace.
    publishPart(store, ns, "part_2", {inlineEntry("checksums.txt", "cs")});
    access.dropNamespace(ns);
    EXPECT_FALSE(access.existsRef({ns, "part_2"}, ContentAddressed::Freshness::ForceFresh));
}
```

- [ ] **Step 2: Run to verify compile failure**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `promoteBuild` not declared.

- [ ] **Step 3: Implement the primitives**

Header additions (public, after `existsRef`):

```cpp
    /// ==== committed part-ref writes (spec §Two-Level API, level 1) ====
    /// Each primitive performs the protocol operation and owns the cache side effect (Phase 4:
    /// erase the affected view on success; on exception cache state is untouched — except
    /// dropRefBestEffort, which erases even on a swallowed failure: in its destructor/rollback
    /// context the ref's durable state is unknown, so dropping the view is the conservative
    /// direction). Committed-ref mutations anywhere else in wiring are style-check failures.

    /// The transaction's terminal publish: pending mutable payload + the atomic owner move.
    void promoteBuild(Cas::Build & build, const PartRefKey & key, UInt128 build_id,
                      const Cas::ManifestId & manifest_id, std::map<String, String> mutable_files);
    /// Mutable-only committed update (autocommit one-shots on a COMMITTED part). NO journal event.
    void updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator);
    void dropRef(const PartRefKey & key);
    /// Idempotent removal: absent ref is success; a drop racing between resolve and the shard
    /// re-read (FILE_DOESNT_EXIST) is success too — the removal unit is replay-safe.
    void dropRefIfPresent(const PartRefKey & key);
    /// Destructor/rollback cleanup: best-effort, never throws; lingering debris is GC-reclaimed.
    void dropRefBestEffort(const PartRefKey & key) noexcept;
    void dropNamespace(const Cas::RootNamespace & ns);
```

Implementation (`CachedPartFolderAccess.cpp`; add `ErrorCodes::FILE_DOESNT_EXIST` extern):

```cpp
void CachedPartFolderAccess::promoteBuild(Cas::Build & build, const PartRefKey & key, UInt128 build_id,
                                          const Cas::ManifestId & manifest_id, std::map<String, String> mutable_files)
{
    build.setPendingMutableFiles(std::move(mutable_files));
    build.promote(key.ns, key.ref, build_id, manifest_id);
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::updateMutableFiles(const PartRefKey & key, std::function<void(Cas::RootRef &)> mutator)
{
    store->updateRefPayload(key.ns, key.ref, std::move(mutator));
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::dropRef(const PartRefKey & key)
{
    store->dropRef(key.ns, key.ref);
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::dropRefIfPresent(const PartRefKey & key)
{
    /// resolveRef gates the common case (a tmp ref that was never committed is a no-op, not an
    /// error); dropRef re-reads the shard inside its own CAS loop, so a concurrent drop can land in
    /// the window between our resolve and that re-read — surfacing as FILE_DOESNT_EXIST. Removal is
    /// replay-safe, so an already-gone ref is success; any other error still propagates. (Moved
    /// verbatim from ContentAddressedTransaction.)
    if (!store->resolveRef(key.ns, key.ref, /*allow_stale=*/true))
        return;
    try
    {
        store->dropRef(key.ns, key.ref);
    }
    catch (const Exception & e)
    {
        if (e.code() != ErrorCodes::FILE_DOESNT_EXIST)
            throw;
    }
    /// Phase 4: eraseView(key)
}

void CachedPartFolderAccess::dropRefBestEffort(const PartRefKey & key) noexcept
{
    try
    {
        store->dropRef(key.ns, key.ref);
    }
    catch (...) // NOLINT(bugprone-empty-catch)
    {
        /// Best-effort destructor/rollback cleanup: debris is GC-reclaimed, never a masked throw.
    }
    /// Phase 4: eraseView(key) — deliberately ALSO on the swallowed-failure path (spec §Two-Level API).
}

void CachedPartFolderAccess::dropNamespace(const Cas::RootNamespace & ns)
{
    store->dropNamespace(ns);
    /// Phase 4: erase every view whose key is in `ns`
}
```

- [ ] **Step 4: Convert every transaction mutation site**

In `ContentAddressedTransaction.cpp` (and drop the `dropRefIfPresent` member from the header — its logic moved to the facade):

- Destructor, `rename_published_refs` loop — the whole try/catch collapses to:

```cpp
    for (const auto & [ns, ref] : rename_published_refs)
        metadata_storage.partAccess().dropRefBestEffort({ns, ref});
```

- `commit`'s compensating rollback loop likewise:

```cpp
        for (const auto & [ns, ref] : created_refs)
            metadata_storage.partAccess().dropRefBestEffort({ns, ref});
```

- `publishStaging`, mutable-only branch:

```cpp
            metadata_storage.partAccess().updateMutableFiles({ns, ref}, [&](Cas::RootRef & payload)
            {
                for (const auto & [name, bytes] : st.mutable_files)
                    payload.mutable_files[name] = bytes;
                for (const auto & name : st.mutable_removed)
                    payload.mutable_files.erase(name);
            });
```

- `publishStaging`, terminal publish:

```cpp
    const bool ref_existed = metadata_storage.partAccess().existsRef({ns, ref}, ContentAddressed::Freshness::ForceFresh);
    metadata_storage.partAccess().promoteBuild(*st.build, {ns, ref}, st.build->buildId(), id, st.mutable_files);
    st.published = true;
    return !ref_existed;
```

(delete the now-unused `st.build->setPendingMutableFiles(st.mutable_files);` line — `promoteBuild` owns it).

- Every `dropRefIfPresent(<ns>, <ref>)` call (in `removeDirectory`, `removeRecursive` ×2, `moveDirectory`) → `metadata_storage.partAccess().dropRefIfPresent({<ns>, <ref>})`.
- `removeRecursive`'s three `store()->dropNamespace(...)` calls → `metadata_storage.partAccess().dropNamespace(...)`.
- `moveDirectory`'s rename-table branch `store()->dropNamespace(from_ns)` → `metadata_storage.partAccess().dropNamespace(from_ns)`.
- The mutable force-fresh source resolves in `createHardLink` and `moveFile` (`metadata_storage.store()->resolveRef(src->ns, src->ref)`) → `metadata_storage.partAccess().resolve(src->refKey(), ContentAddressed::Freshness::ForceFresh)`.

(`republishRef` still holds raw calls — it moves wholesale in Task 5.)

- [ ] **Step 5: Build, run gtests, commit**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10`
Expected: PASS.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS wiring: committed part-ref mutations go through facade primitives"
```

---

### Task 5: `publishEntries` + `republishRef` in the facade; `adoptPartFromManifest` consolidation {#task-5}

**Files:**
- Modify: `.../CachedPartFolderAccess.h`, `.../CachedPartFolderAccess.cpp`, `.../ContentAddressedTransaction.h` (delete `republishRef` member), `.../ContentAddressedTransaction.cpp` (call sites), `.../ContentAddressedMetadataStorage.cpp` (`adoptPartFromManifest`)
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

**Interfaces:**
- Produces:

```cpp
    /// The shared committed-publish sequence (spec §Two-Level API, level 2): adopt-evidence over
    /// `entries`, stage a FRESH manifest, precommit, promote. Used by republishRef and by
    /// adoptPartFromManifest (their bodies were near-duplicates).
    void publishEntries(const PartRefKey & dst, const std::vector<Cas::ManifestEntry> & entries,
                        std::map<String, String> mutable_files, Cas::ProvenanceOp op);
    /// Move a COMMITTED ref by republish + drop-source. false = absent source (nothing written).
    bool republishRef(const PartRefKey & src, const PartRefKey & dst);
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasPartFolderAccess, RepublishRefMovesCommittedRef)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);
    publishPart(store, ns, "src_part", {inlineEntry("checksums.txt", "cs")}, {{"txn_version.txt", "v1"}});

    EXPECT_FALSE(access.republishRef({ns, "absent"}, {ns, "dst"}));   /// absent source: nothing written

    ASSERT_TRUE(access.republishRef({ns, "src_part"}, {ns, "dst_part"}));
    EXPECT_FALSE(access.existsRef({ns, "src_part"}, ContentAddressed::Freshness::ForceFresh));
    auto view = access.getView({ns, "dst_part"}, ContentAddressed::Freshness::ForceFresh);
    ASSERT_NE(view, nullptr);
    EXPECT_NE(view->findFile("checksums.txt"), nullptr);
    EXPECT_EQ(view->mutableBytes("txn_version.txt"), std::optional<String>("v1"));   /// carried over
}

TEST(CasPartFolderAccess, RepublishRefIdempotentRedriveAndConflict)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::RootNamespace ns{"srv/t1"};
    ContentAddressed::CachedPartFolderAccess access(store);

    /// Re-drive: dst already committed with the SAME content (a prior attempt's promote landed,
    /// only dropRef(src) was interrupted), and src's mutable payload drifted afterwards.
    publishPart(store, ns, "src", {inlineEntry("f", "same")}, {{"txn_version.txt", "v2"}});
    publishPart(store, ns, "dst", {inlineEntry("f", "same")}, {{"txn_version.txt", "v1"}});
    ASSERT_TRUE(access.republishRef({ns, "src"}, {ns, "dst"}));
    EXPECT_FALSE(access.existsRef({ns, "src"}, ContentAddressed::Freshness::ForceFresh));
    auto resolved = access.resolve({ns, "dst"}, ContentAddressed::Freshness::ForceFresh);
    EXPECT_EQ(resolved->mutable_files.at("txn_version.txt"), "v2");   /// re-synced from src

    /// Conflict: dst committed with DIFFERENT content — fail closed, src untouched.
    publishPart(store, ns, "src2", {inlineEntry("f", "one")});
    publishPart(store, ns, "dst2", {inlineEntry("f", "two")});
    expectThrowsCode(ErrorCodes::ABORTED, [&] { access.republishRef({ns, "src2"}, {ns, "dst2"}); });
    EXPECT_TRUE(access.existsRef({ns, "src2"}, ContentAddressed::Freshness::ForceFresh));
}
```

- [ ] **Step 2: Run to verify compile failure**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `republishRef` not a member of `CachedPartFolderAccess`.

- [ ] **Step 3: Implement**

`CachedPartFolderAccess.cpp` (extern `ErrorCodes::ABORTED` too). The bodies move from `ContentAddressedTransaction::republishRef` / `adoptPartFromManifest` with their load-bearing comments:

```cpp
void CachedPartFolderAccess::publishEntries(const PartRefKey & dst,
    const std::vector<Cas::ManifestEntry> & entries, std::map<String, String> mutable_files, Cas::ProvenanceOp op)
{
    auto build = store->startBuild(Cas::BuildInfo{.intended_ref = dst.ns.string() + "/" + dst.ref,
                                                  .intended_namespace = dst.ns, .op = op});
    /// Tokenless W-EVIDENCE dep per entry — NO pool HEAD/GET before precommit; promote re-proves
    /// each fail-closed. Inline entries record nothing (adoptEvidence skips them).
    for (const auto & entry : entries)
        build->adoptEvidence(entry);
    /// A FRESH dst manifest over the SAME entries (only blobs are content-addressed; a part is a
    /// single-owner ManifestId, so dst gets its own id), then move ownership in.
    const Cas::ManifestId id = build->stageManifest(entries);
    build->precommitAdd(dst.ns, dst.ref, id);
    promoteBuild(*build, dst, build->buildId(), id, std::move(mutable_files));
}

bool CachedPartFolderAccess::republishRef(const PartRefKey & src, const PartRefKey & dst)
{
    /// Move a COMMITTED ref (rev. 15 §republish): content addressing has no rename. Force-fresh
    /// source read (RENAME/move: stale mutable_files must not carry to dst); readManifestShared's
    /// mandatory HEAD re-proves the source body (write evidence is never a cached view).
    auto resolved = store->resolveRef(src.ns, src.ref);
    if (!resolved)
        return false;
    const auto src_manifest = store->readManifestShared(resolved->manifest_id);

    /// BUG 1c: idempotent re-drive. If dst is ALREADY committed, the prior attempt's promote landed
    /// and only dropRef(src) was interrupted. Compare CONTENT (path-sorted `entries`, not the whole
    /// manifest — ref/namespace/digest legitimately differ): same content => finish the rename by
    /// dropping src; a different-content dst is a genuine conflict => fail closed (never silently
    /// drop src's content). `mutable_files` is NOT part of the idempotency key and can have drifted
    /// on src between the crashed promote(dst) and this re-drive — re-sync it onto dst.
    if (auto dst_resolved = store->resolveRef(dst.ns, dst.ref))
    {
        const auto dst_manifest = store->readManifestShared(dst_resolved->manifest_id);
        if (dst_manifest->entries != src_manifest->entries)
            throw Exception(ErrorCodes::ABORTED,
                "republishRef: destination '{}' is already committed with different content — refusing "
                "(rename/attach conflict)", dst.ns.string() + "/" + dst.ref);
        if (dst_resolved->mutable_files != resolved->mutable_files)
        {
            const std::map<String, String> current_mutable_files = resolved->mutable_files;
            updateMutableFiles(dst, [&](Cas::RootRef & payload)
            {
                payload.mutable_files = current_mutable_files;
            });
        }
        dropRef(src);
        return true;
    }

    /// Mutable files carry over (a rename is not a new part). promote stamps the dst publish clock.
    publishEntries(dst, src_manifest->entries, resolved->mutable_files, Cas::ProvenanceOp::Other);
    dropRef(src);
    return true;
}
```

- [ ] **Step 4: Convert the call sites**

`ContentAddressedTransaction`: delete the `republishRef` member (header + cpp). Call sites in `moveDirectory`:

```cpp
                for (const auto & [ref, _] : metadata_storage.store()->listRefs(from_ns))
                    metadata_storage.partAccess().republishRef({from_ns, ref}, {to_ns, ref});
```

and the part-dir move tail:

```cpp
        metadata_storage.partAccess().republishRef({src->ns, src->ref}, {dst->ns, dst->ref});
        return;
```

`ContentAddressedMetadataStorage::adoptPartFromManifest` — the `try` body collapses onto the shared sequence (the decode, receiver-namespace derivation, and catch/fallback wrapper stay exactly as they are):

```cpp
    try
    {
        /// Sender identity is NON-AUTHORITATIVE: only the entries are used. The blobs are already
        /// in the shared pool (referenced by hash) — publishEntries adopts them as tokenless
        /// W-EVIDENCE and promote re-proves each fail-closed (the proven republish sequence).
        partAccess().publishEntries({receiver_ns, part_name}, decoded.entries, mutable_files,
                                    Cas::ProvenanceOp::Attach);
        return true;
    }
```

- [ ] **Step 5: Build; run gtests + the promote/republish suite + functional suite; commit**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10`
Then the full CA stateless list (log `build_debug/test_phase2_writes.log`).
Expected: all pass — rename/attach/adopt behavior unchanged.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "CAS wiring: publishEntries/republishRef in the facade; adoptPartFromManifest consolidated onto publishEntries"
```

---

### Task 6: Delete `Store::lookupPath` / `Store::listDirectory` (manifest overload); migrate Core gtests {#task-6}

**Files:**
- Modify: `.../Core/CasStore.h`, `.../Core/CasStore.cpp` (delete both), `src/Disks/tests/gtest_cas_build.cpp`, `src/Disks/tests/gtest_cas_protocol_scenarios.cpp`, `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `Cas::findEntry` / `Cas::entryRange` (Phase 1 Task 2).
- Produces: `Cas::Store` is protocol-only — no pure file-tree queries remain on it.

- [ ] **Step 1: Migrate the gtest call sites**

Exact replacements (line numbers as of Phase 1 completion — locate by pattern):

- `gtest_cas_build.cpp` (~1055, ~1464) and `gtest_cas_protocol_scenarios.cpp` (~90, ~572, ~588):
  `auto entry = s->lookupPath(manifest, "<name>");` → `const auto * entry = Cas::findEntry(manifest.entries, "<name>");`
  and each `entry->has_value()` / `entry.has_value()` assertion → `entry != nullptr`; dereferences `entry->field` stay valid.
- `gtest_cas_store.cpp` (~438-448, ~518-552): same `lookupPath` replacement; the two `listDirectory` uses:

```cpp
    auto [proj_first, proj_last] = Cas::entryRange(manifest.entries, "p.proj/");
    std::vector<Cas::ManifestEntry> proj(proj_first, proj_last);
    /// ...assertions unchanged...
    auto [all_first, all_last] = Cas::entryRange(manifest.entries, "");
    std::vector<Cas::ManifestEntry> all(all_first, all_last);
```

- [ ] **Step 2: Delete the Store methods**

Remove the `lookupPath` and `listDirectory(const PartManifest &, const String &)` declarations from `CasStore.h` and their definitions from `CasStore.cpp` (`Store::lookupPath` at ~1038, `Store::listDirectory` at ~1048).

- [ ] **Step 3: Build (whole tree — any remaining caller fails here), run gtests**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10`
Expected: compiles (no callers left), PASS.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_build.cpp src/Disks/tests/gtest_cas_protocol_scenarios.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "CAS store: drop pure tree queries (lookupPath/listDirectory) — findEntry/entryRange replace them"
```

---

### Task 7: Check-style guard {#task-7}

**Files:**
- Modify: `ci/jobs/scripts/check_style/check_cpp.sh` (new numbered block before the final `wait`)
- Modify: `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md` (already corrected to this path during plan review — verify, don't re-edit if done)

**Interfaces:**
- Produces: CI failure on raw committed-ref mutation tokens in wiring. Best-effort textual guard (spec §Mechanical Enforcement); the review rule stays normative.

- [ ] **Step 1: Add the check block**

Insert before the `# Wait for all parallel checks` line, using the next free block number (19 as of writing — bump if taken):

```bash
# 19: CAS wiring must not mutate committed refs through raw Cas::Store / Cas::Build (spec
# docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md): committed part-ref mutations
# go through CachedPartFolderAccess (dot-syntax on the partAccess() reference). Best-effort textual
# guard: Core/ (the protocol implementation) and the facade itself are exempt.
{
find $ROOT_PATH/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed -maxdepth 1 \( -name '*.h' -or -name '*.cpp' \) 2>/dev/null |
    grep -v 'CachedPartFolderAccess' |
    xargs -r rg -n -e '->(dropRef|updateRefPayload|dropNamespace|promote)\(' &&
    echo "Committed part-ref mutations in CAS wiring must go through CachedPartFolderAccess (docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md)"
} > "$O.19" 2>&1 &
```

- [ ] **Step 2: Verify the rule is quiet on the converted tree, and fires on a violation**

```bash
bash ci/jobs/scripts/check_style/check_cpp.sh 2>/dev/null | grep -c "CachedPartFolderAccess" ; # expected: 0
# Negative check: plant a violation, expect the message, revert.
echo 'void f(DB::Cas::StorePtr s) { s->dropRef({}, ""); }' >> src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h
bash ci/jobs/scripts/check_style/check_cpp.sh 2>/dev/null | grep -c "CachedPartFolderAccess" ; # expected: >= 1
git checkout -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h
```

- [ ] **Step 3: Commit**

```bash
git add ci/jobs/scripts/check_style/check_cpp.sh
git commit -m "CAS style check: ban raw committed-ref mutation tokens in wiring outside CachedPartFolderAccess"
```

---

### Task 8: Phase verification {#task-8}

- [ ] **Step 1: Full gtest batch** — `./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10` → PASS.
- [ ] **Step 2: Full CA stateless suite** (same list/command as the Phase 1 plan, log `build_debug/test_phase2_final.log`) → all pass.
- [ ] **Step 3: Style check** — `bash ci/jobs/scripts/check_style/check_cpp.sh > build_debug/style_check.log 2>&1; grep -c CachedPartFolderAccess build_debug/style_check.log` → 0.
- [ ] **Step 4:** No commit needed if all green; otherwise fix-forward with new commits.

## Phase acceptance {#phase-acceptance}

Same behavior; no normal-path bypass around the facade (style check green); every committed part-ref mutation in wiring goes through facade methods; `resolveRouted`, transaction `republishRef`/`dropRefIfPresent`, and the `Store` tree queries are gone.
