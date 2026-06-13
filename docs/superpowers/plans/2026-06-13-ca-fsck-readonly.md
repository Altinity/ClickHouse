# CA Introspection — Read-Only Disk Mode + `clickhouse-disks fsck` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a read-only open mode to the content-addressed (CA) disk and a `clickhouse-disks fsck` / `ca-gc-dryrun` command that independently verifies pool reachability (INV-NO-LOSS) and cross-checks GC's delete decisions.

**Architecture:** A `read_only` flag threads `PoolConfig` → `Cas::Store::open` (skip the mutating capability probe) and `ContentAddressedMetadataStorage` (no GC scheduler / heartbeat; mutations fail-closed). A new `Cas::runFsck(Store&)` walks the authoritative refs (`listNamespaces → listRefs → resolveRef → readTree → locate`), diffs against a raw `backend.list` of the content planes, and classifies every object `reachable`/`dangling`/`unreachable`. A write-free `Cas::Gc::previewDeletes()` derives the would-delete set from durable `gc/snap`+`gc/state`. Two thin `clickhouse-disks` commands consume these.

**Tech Stack:** C++ (ClickHouse `DB::Cas` core + `programs/disks`), gtest (`src/Disks/tests/`, auto-globbed), Allman braces, build logs to file + subagent summaries (per `CLAUDE.md`).

**Spec:** `docs/superpowers/specs/2026-06-13-ca-fsck-readonly-design.md`.

**Conventions reminder:** build with `ninja -C build unit_tests_dbms > build/build_<tag>.log 2>&1` then have a subagent summarize the log; run gtests as `./build/src/unit_tests_dbms --gtest_filter='<F>' > tmp/<tag>.log 2>&1`. Allman braces. No `no-*` test tags. Commit only on the `cas-mergetree-poc` branch (already checked out) — never master; add new commits, never amend/rebase.

---

## File Structure

- `Core/CasStore.h` / `CasStore.cpp` — add `PoolConfig::read_only`; `Store::open` skips the probe when set.
- `ContentAddressedMetadataStorage.h` / `.cpp` — derive `read_only` from `object_storage->isReadOnly()`; gate probe/scheduler/heartbeat; fail-closed mutations; `isReadOnly()` returns the flag.
- `Core/CasFsck.h` / `CasFsck.cpp` (**new**) — `FsckReport`, `FsckObject`, `runFsck(Store&, bool detail)`. The independent reachability checker.
- `Core/CasGc.h` / `CasGc.cpp` — add write-free `previewDeletes()` + `struct PreviewEntry`.
- `programs/disks/CommandFsck.cpp` (**new**), `CommandCaGcDryRun.cpp` (**new**) — thin CLI wrappers; declare `makeCommandFsck()`/`makeCommandCaGcDryRun()` in `ICommand.h`; register in `DisksApp.cpp`; add to `programs/disks/CMakeLists.txt` if sources are listed explicitly.
- Tests: `src/Disks/tests/gtest_cas_store.cpp` (read_only open), `gtest_ca_wiring.cpp` (CAMS read-only mode), `gtest_cas_fsck.cpp` (**new**, classification), `gtest_cas_gc_round.cpp` (previewDeletes).

---

## Task 1: `PoolConfig::read_only` — `Store::open` skips the capability probe

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` (PoolConfig)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` (`Store::open`)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

The probe (`runCapabilityProbe`, called unconditionally in `Store::open` at `CasStore.cpp:49`) *writes and deletes* two throwaway keys under `pool_prefix + "/_probe"`. A read-only open must skip it (fsck only reads, and must never mutate a live pool it inspects).

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_store.cpp`. This uses an in-memory backend write-counter to prove the read-only open performs zero writes (the probe would write 2 keys). Put the counting backend in an anonymous namespace near the top of the file if not already present.

```cpp
namespace
{
/// Counts mutating backend calls so a test can assert an open path is write-free.
class WriteCountingBackend final : public DB::Cas::Backend
{
public:
    explicit WriteCountingBackend(std::shared_ptr<DB::Cas::Backend> inner_) : inner(std::move(inner_)) {}
    size_t writes = 0;

    std::optional<DB::Cas::GetResult> get(const DB::String & k, DB::Cas::Range r = {}) override { return inner->get(k, r); }
    DB::Cas::HeadResult head(const DB::String & k) override { return inner->head(k); }
    DB::Cas::ListPage list(const DB::String & p, const DB::String & c, size_t l) override { return inner->list(p, c, l); }
    DB::Cas::PutOutcome putIfAbsent(const DB::String & k, const DB::String & b, DB::Cas::Token * t = nullptr) override { ++writes; return inner->putIfAbsent(k, b, t); }
    DB::Cas::WriteSinkPtr putIfAbsentStream(const DB::String & k) override { ++writes; return inner->putIfAbsentStream(k); }
    DB::Cas::PutOutcome putOverwrite(const DB::String & k, const DB::String & b, const DB::Cas::Token & e, DB::Cas::Token * t = nullptr) override { ++writes; return inner->putOverwrite(k, b, e, t); }
    DB::Cas::CasOutcome casPut(const DB::String & k, const DB::String & b, const std::optional<DB::Cas::Token> & e, DB::Cas::Token * t = nullptr) override { ++writes; return inner->casPut(k, b, e, t); }
    DB::Cas::DeleteOutcome deleteExact(const DB::String & k, const DB::Cas::Token & t) override { ++writes; return inner->deleteExact(k, t); }
private:
    std::shared_ptr<DB::Cas::Backend> inner;
};
}

TEST(CasStore, ReadOnlyOpenSkipsProbe)
{
    auto shared = std::make_shared<DB::Cas::InMemoryBackend>();

    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(1);
    /// Writable open: creates _pool_meta and runs the probe (which writes+cleans up).
    DB::Cas::Store::open(std::make_shared<WriteCountingBackend>(shared), cfg);

    /// Read-only re-open over the SAME data must perform ZERO writes (no probe, meta already present).
    auto counter = std::make_shared<WriteCountingBackend>(shared);
    DB::Cas::PoolConfig ro = cfg;
    ro.read_only = true;
    auto store = DB::Cas::Store::open(counter, ro);
    EXPECT_EQ(counter->writes, 0u);
    ASSERT_NE(store, nullptr);
}
```

- [ ] **Step 2: Run test to verify it fails**

Build, then run:
```bash
ninja -C build unit_tests_dbms > build/build_t1.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CasStore.ReadOnlyOpenSkipsProbe' > tmp/t1.log 2>&1
```
Expected: compile error (`PoolConfig` has no `read_only`) — or, once the field exists but the probe still runs, FAIL with `counter->writes` ≥ 2.

- [ ] **Step 3: Add the field and gate the probe**

In `CasStore.h`, add to `struct PoolConfig` (after `background_heartbeats`):
```cpp
    bool read_only = false;   /// observe-only open: skip the mutating capability probe; reads only
```

In `CasStore.cpp` `Store::open`, gate the probe:
```cpp
StorePtr Store::open(BackendPtr backend, PoolConfig config)
{
    Layout layout(config.pool_prefix);
    /// The probe writes+deletes throwaway keys to prove conditional-op enforcement. A read-only open
    /// must never mutate the pool it inspects; fsck only reads, so skip it. (Pool meta below is read-
    /// only when the pool already exists; a missing pool meta on a read-only backend fails closed.)
    if (!config.read_only)
        runCapabilityProbe(*backend, config.pool_prefix + "/_probe");
    PoolMeta meta = PoolMeta::createOrValidate(*backend, layout, config.root_shards, config.blob_header_len);
    StorePtr store(new Store(std::move(backend), std::move(config), std::move(meta)));
    store->retire_view.refresh();
    return store;
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/build_t1.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CasStore.*' > tmp/t1.log 2>&1
```
Expected: PASS (the whole `CasStore` suite stays green).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "CA fsck T1: PoolConfig.read_only — Store::open skips the capability probe"
```

---

## Task 2: `ContentAddressedMetadataStorage` read-only (observe-only) mode

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Test: `src/Disks/tests/gtest_ca_wiring.cpp`

Trigger: the underlying object storage's `isReadOnly()` (set by the disk's `<readonly>` config). When read-only: `pool_config.read_only=true` (skip probe via Task 1), `background_heartbeats=false`, no `gc_scheduler`, `isReadOnly()` returns true, and `createTransaction()` / `adoptPart()` fail closed.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_ca_wiring.cpp` (near the other `CaWiring*` write tests; the `ErrorCodes` block there already declares `LOGICAL_ERROR`/`CORRUPTED_DATA` — add `READONLY`). First extend the `ErrorCodes` namespace block in that file:
```cpp
namespace DB::ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int CORRUPTED_DATA;
    extern const int READONLY;
}
```
Then the test. It populates a pool through a writable storage, then opens a SECOND storage over the same on-disk root with a **read-only** `LocalObjectStorage`, and asserts reads work while writes fail closed:
```cpp
TEST(CaWiringReadOnly, ObserveOnlyOpenReadsButRejectsWrites)
{
    /// 1. Writable storage publishes a part into a fixed root.
    const auto root = (std::filesystem::temp_directory_path()
                       / ("ca_ro_" + std::to_string(::getpid()))).string();
    std::error_code ec; std::filesystem::remove_all(root, ec); std::filesystem::create_directories(root, ec);
    auto writable_os = std::make_shared<DB::LocalObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));
    {
        auto w = std::make_shared<DB::ContentAddressedMetadataStorage>(
            writable_os, "pool", "srv1", std::filesystem::temp_directory_path() / "ca_ro_scratch", nullptr);
        w->startup();
        auto tx = w->createTransaction();
        writeThroughTransaction(*tx, "uui/uuid-1/all_1_1_0/data.bin", "ro-bytes");
        tx->commit(DB::NoCommitOptions{});
    }

    /// 2. Read-only object storage over the SAME root => observe-only metadata storage.
    auto ro_os = std::make_shared<DB::LocalObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/true));
    auto ro = std::make_shared<DB::ContentAddressedMetadataStorage>(
        ro_os, "pool", "srv2", std::filesystem::temp_directory_path() / "ca_ro_scratch2", nullptr);
    ro->startup();   /// must NOT throw (probe skipped — a probe write would fail on a read-only os)

    EXPECT_TRUE(ro->isReadOnly());
    /// Reads work:
    EXPECT_TRUE(ro->existsFile("uui/uuid-1/all_1_1_0/data.bin"));
    EXPECT_EQ(ro->getFileSize("uui/uuid-1/all_1_1_0/data.bin"), 8u);
    /// Writes fail closed:
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::READONLY,
        [&] { ro->createTransaction(); });
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::READONLY,
        [&] { ro->adoptPart("uuid-1", "tmp-fetch", std::string(32, '0'), {}); });
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/build_t2.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CaWiringReadOnly.*' > tmp/t2.log 2>&1
```
Expected: FAIL — `isReadOnly()` returns false (hardcoded) and/or `startup()` throws when the probe tries to write to the read-only object storage, and `createTransaction()` does not throw.

- [ ] **Step 3: Implement the read-only mode**

In `ContentAddressedMetadataStorage.h`: add `READONLY` to the `.cpp` `ErrorCodes` block (Step 3b), add a member and change `isReadOnly()`:
```cpp
    bool isReadOnly() const override { return read_only; }
```
Add to the private members (near `gc_scheduler`):
```cpp
    bool read_only = false;   /// observe-only: object storage is read-only (WORM mounter / fsck)
```

In `ContentAddressedMetadataStorage.cpp`:

3a. Ensure the `ErrorCodes` block declares `READONLY`:
```cpp
namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
    extern const int CORRUPTED_DATA;
    extern const int READONLY;
}
```

3b. In `startup()`, derive the flag and gate side effects. Set it before `Store::open`, and pass it into the pool config. Replace the heartbeat/scheduler region:
```cpp
    read_only = object_storage->isReadOnly();
    pool_config.background_heartbeats = (context != nullptr) && !read_only;
    pool_config.read_only = read_only;
    cas_store = Cas::Store::open(std::move(backend), std::move(pool_config));
    ...
    /// The background GC scheduler is a WRITER role: never on a read-only mounter.
    if (context && gc_enabled && !read_only)
    {
        gc_scheduler = std::make_unique<ContentAddressed::CasGcScheduler>(...);  // unchanged args
        gc_scheduler->start();
    }
```
(Keep the existing `EmulatedSingleProcess` warning and key-prefix logic intact — only add the `read_only` gating.)

3c. Fail-closed mutations. At the top of `createTransaction()`:
```cpp
MetadataTransactionPtr ContentAddressedMetadataStorage::createTransaction()
{
    if (read_only)
        throw Exception(ErrorCodes::READONLY, "Content-addressed disk is opened read-only: writes are rejected");
    ...
}
```
And at the top of `adoptPart(...)`:
```cpp
    if (read_only)
        throw Exception(ErrorCodes::READONLY, "Content-addressed disk is opened read-only: adoptPart is rejected");
```
(Every write/commit/hardlink/move/remove flows through `createTransaction`'s transaction object, so guarding `createTransaction` + `adoptPart` covers the mutating surface.)

- [ ] **Step 4: Run test to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/build_t2.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CaWiring*:Cas*' > tmp/t2.log 2>&1
```
Expected: PASS, full CA battery green.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "CA fsck T2: read-only (observe-only) metadata-storage mode from object-storage readonly"
```

---

## Task 3: `Cas::runFsck` — independent reachability classification

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp`
- Test: `src/Disks/tests/gtest_cas_fsck.cpp` (new, auto-globbed)

`runFsck` walks `listNamespaces → listRefs → resolveRef → readTree → locate` to build the reachable content-key set (blobs + trees, recursing subtrees), diffs against a paginated `backend.list` of `prefix/blobs/`, `prefix/trees/`, `prefix/packs/`, and classifies. Per-ref tree walks reuse `Store`'s internal `tree_cache`, so shared trees are read once.

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_fsck.cpp`:
```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;
using DB::Cas::tests::idOf;
using DB::Cas::tests::u128Of;

namespace
{
StorePtr openPool(std::shared_ptr<Backend> backend)
{
    PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(7);
    return Store::open(backend, cfg);
}

/// Publish a one-blob part `ref` carrying `payload` (shared content => shared blob).
void publishPart(Store & store, const String & payload, const String & ref)
{
    auto build = store.startBuild({});
    build->putBlob(idOf(payload), BlobSource::fromString(payload));
    auto tree = build->putTree({[&]{ TreeEntry e; e.name = "data.bin"; e.placement = Placement::Blob;
        e.file_hash = u128Of(payload); e.file_size = payload.size(); return e; }()});
    build->publish(RootNamespace{"uui/uuid-1"}, ref, tree, {});
}
}

TEST(CasFsck, CleanPoolHasNoDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "shared-A", "all_1_1_0");
    publishPart(*store, "shared-A", "all_2_2_0");   /// identical content => same blob (dedup)

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_TRUE(report.clean());
    EXPECT_EQ(report.unreachable, 0u);
    /// Two refs, one distinct blob => dedup is real.
    EXPECT_EQ(report.distinct_blobs, 1u);
    EXPECT_EQ(report.total_blob_refs, 2u);
}

TEST(CasFsck, MissingReachableBlobIsDangling)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "lonely", "all_1_1_0");

    /// Delete the reachable blob out-of-band (as a backend corruption / lost write would).
    const String bkey = store->layout().blobKey(idOf("lonely"));
    const auto tok = backend->head(bkey).token;
    ASSERT_EQ(backend->deleteExact(bkey, tok).kind, DeleteOutcome::Kind::Deleted);

    auto report = runFsck(*store, /*detail=*/true);
    EXPECT_EQ(report.dangling, 1u);
    EXPECT_FALSE(report.clean());
    auto it = std::find_if(report.objects.begin(), report.objects.end(),
        [&](const FsckObject & o) { return o.cls == FsckClass::Dangling; });
    ASSERT_NE(it, report.objects.end());
    EXPECT_EQ(it->key, bkey);
    EXPECT_FALSE(it->reachable_from.empty());
}

TEST(CasFsck, DroppedButUnreclaimedBlobIsUnreachable)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPool(backend);
    publishPart(*store, "ghost", "all_1_1_0");
    store->dropRef(RootNamespace{"uui/uuid-1"}, "all_1_1_0");   /// ref gone, blob/tree NOT yet GC'd

    auto report = runFsck(*store, /*detail=*/false);
    EXPECT_EQ(report.dangling, 0u);
    EXPECT_GE(report.unreachable, 1u);   /// the orphaned blob (+ tree) await GC
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/build_t3.log 2>&1
```
Expected: compile error — `CasFsck.h` does not exist yet.

- [ ] **Step 3: Implement `CasFsck.h`**

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

#include <vector>

namespace DB::Cas
{

enum class FsckClass : uint8_t
{
    Reachable,     /// reachable from a live ref AND present in the object store
    Dangling,      /// reachable from a live ref but the object is MISSING — INV-NO-LOSS violation
    Unreachable,   /// present but not reachable from any ref (in-grace debris or a leak)
};

struct FsckObject
{
    String key;
    ObjectKind kind = ObjectKind::Blob;
    uint64_t size = 0;                   /// on-disk object size (0 when dangling)
    FsckClass cls = FsckClass::Reachable;
    std::vector<String> reachable_from;  /// "ns/ref" labels (populated for reachable/dangling when detail)
};

struct FsckReport
{
    uint64_t reachable = 0;
    uint64_t dangling = 0;
    uint64_t unreachable = 0;

    uint64_t physical_bytes = 0;          /// Σ on-disk size of present content objects (blobs+trees)
    uint64_t referenced_logical_bytes = 0;/// Σ file_size over every blob reference (with multiplicity)
    uint64_t total_blob_refs = 0;         /// count of blob references across all refs (with multiplicity)
    uint64_t distinct_blobs = 0;          /// distinct reachable blob keys

    std::vector<FsckObject> objects;      /// per-object detail (populated only when detail=true)

    double dedupRatio() const { return distinct_blobs ? double(total_blob_refs) / double(distinct_blobs) : 0.0; }
    bool clean() const { return dangling == 0; }
};

/// Independent reachability check: recompute reachability from the authoritative refs (NEVER from
/// gc/snap) and diff against a raw object listing. Read-only. `detail` populates per-object rows.
FsckReport runFsck(Store & store, bool detail);

}
```

- [ ] **Step 4: Implement `CasFsck.cpp`**

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>

#include <functional>
#include <set>
#include <unordered_map>

namespace DB::Cas
{

namespace
{
/// Enumerate every key under `prefix` into `out[key]=size` (paginate until next_cursor empties).
void listAll(Backend & backend, const String & prefix, std::unordered_map<String, uint64_t> & out)
{
    String cursor;
    while (true)
    {
        ListPage page = backend.list(prefix, cursor, 1000);
        for (const auto & k : page.keys)
            out[k.key] = k.size;
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}
}

FsckReport runFsck(Store & store, bool detail)
{
    const Layout & layout = store.layout();
    Backend & backend = store.backend();

    FsckReport report;

    /// 1. Reachable content keys from the authoritative refs.
    std::unordered_map<String, std::vector<String>> reachable;   /// key -> ref labels (labels only when detail)
    std::set<String> reachable_blobs;

    std::function<void(const TreeId &, const String &, std::set<String> &)> walk =
        [&](const TreeId & tree_id, const String & label, std::set<String> & seen)
    {
        const String tkey = layout.treeKey(tree_id);
        if (detail)
            reachable[tkey].push_back(label);
        else
            reachable.try_emplace(tkey);
        if (!seen.insert(tree_id.string()).second)
            return;   /// already expanded under this ref (DAG / cycle guard)
        for (const TreeEntry & e : store.readTree(tree_id))
        {
            if (e.placement == Placement::Blob)
            {
                const String bkey = layout.blobKey(BlobId(u128ToHex(e.file_hash)));
                if (detail)
                    reachable[bkey].push_back(label);
                else
                    reachable.try_emplace(bkey);
                reachable_blobs.insert(bkey);
                ++report.total_blob_refs;
                report.referenced_logical_bytes += e.file_size;
            }
            else if (e.placement == Placement::Subtree)
            {
                walk(TreeId(u128ToHex(e.file_hash)), label, seen);
            }
            /// Inline: no object. PackSlice: M-F (not produced yet); its pack key would be handled here later.
        }
    };

    for (const String & ns_str : store.listNamespaces(""))
    {
        const RootNamespace ns{ns_str};
        for (const auto & [ref_name, resolved] : store.listRefs(ns))
        {
            std::set<String> seen;
            walk(resolved.tree_id, ns_str + "/" + ref_name, seen);
        }
    }
    report.distinct_blobs = reachable_blobs.size();

    /// 2. Raw listing of the content planes.
    std::unordered_map<String, uint64_t> present;
    listAll(backend, layout.blobsPrefix(), present);
    listAll(backend, layout.treesPrefix(), present);
    listAll(backend, layout.packsPrefix(), present);
    for (const auto & [_, sz] : present)
        report.physical_bytes += sz;

    /// 3. Classify.
    auto kindOf = [&](const String & key)
    {
        if (key.starts_with(layout.blobsPrefix())) return ObjectKind::Blob;
        if (key.starts_with(layout.packsPrefix())) return ObjectKind::Pack;
        return ObjectKind::Tree;
    };

    for (const auto & [key, labels] : reachable)
    {
        auto it = present.find(key);
        const bool exists = it != present.end();
        if (exists)
            ++report.reachable;
        else
            ++report.dangling;
        if (detail || !exists)
        {
            FsckObject o;
            o.key = key;
            o.kind = kindOf(key);
            o.size = exists ? it->second : 0;
            o.cls = exists ? FsckClass::Reachable : FsckClass::Dangling;
            o.reachable_from = labels;
            report.objects.push_back(std::move(o));
        }
    }
    for (const auto & [key, sz] : present)
    {
        if (reachable.contains(key))
            continue;
        ++report.unreachable;
        if (detail)
        {
            FsckObject o;
            o.key = key;
            o.kind = kindOf(key);
            o.size = sz;
            o.cls = FsckClass::Unreachable;
            report.objects.push_back(std::move(o));
        }
    }

    return report;
}

}
```

This adds three accessors to `Layout` (Step 4b): `blobsPrefix()`, `treesPrefix()`, `packsPrefix()`. In `CasLayout.h`, next to `rootsPrefix()`:
```cpp
    String blobsPrefix() const { return prefix + "/blobs/"; }
    String treesPrefix() const { return prefix + "/trees/"; }
    String packsPrefix() const { return prefix + "/packs/"; }
```

- [ ] **Step 5: Build, run, and commit**

```bash
ninja -C build unit_tests_dbms > build/build_t3.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CasFsck.*:Cas*' > tmp/t3.log 2>&1
```
Expected: PASS (3 new tests + battery green).
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/tests/gtest_cas_fsck.cpp
git commit -m "CA fsck T3: Cas::runFsck — independent reachability classification (reachable/dangling/unreachable)"
```

---

## Task 4: `Cas::Gc::previewDeletes` — write-free GC delete preview + cross-check

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

`previewDeletes` reads the durable `gc/state` + snap (`loadSnap`) and reports the zero-in-degree candidates that exist — the set the next round would retire/delete — performing **zero** writes. The cross-check the soak test relies on: `{preview} ⊆ {fsck unreachable}`.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_gc_round.cpp`. The test is self-contained (it does not depend on other helpers in that file): it opens a pool, publishes one part, drops it, runs one real round so the durable snap reflects the drop, then asserts `previewDeletes` is write-free and a subset of fsck's unreachable set. Needs these includes at the top of the file if absent: `CasFsck.h`, `CasBuild.h`, `cas_test_helpers.h`.
```cpp
TEST(CasGcRound, PreviewDeletesIsWriteFreeAndSubsetOfUnreachable)
{
    using namespace DB::Cas;
    using DB::Cas::tests::idOf;
    using DB::Cas::tests::u128Of;

    auto backend = std::make_shared<InMemoryBackend>();
    PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = DB::UInt128(7);
    auto store = Store::open(backend, cfg);

    /// Publish one part, then drop it (ref gone; tree+blob await GC).
    {
        auto build = store->startBuild({});
        build->putBlob(idOf("ghost"), BlobSource::fromString("ghost"));
        TreeEntry e; e.name = "data.bin"; e.placement = Placement::Blob;
        e.file_hash = u128Of("ghost"); e.file_size = 5;
        auto tree = build->putTree({e});
        build->publish(RootNamespace{"uui/uuid-1"}, "all_1_1_0", tree, {});
    }
    store->dropRef(RootNamespace{"uui/uuid-1"}, "all_1_1_0");

    /// One real round folds the drop and persists the snap/state previewDeletes reads.
    Gc gc(store, DB::UInt128(123));
    gc.runRegularRound();

    /// countObjects = number of keys under the pool prefix (paginated).
    auto countObjects = [&]() -> size_t
    {
        size_t n = 0; String cursor;
        while (true)
        {
            auto page = backend->list("pool", cursor, 1000);
            n += page.keys.size();
            if (page.next_cursor.empty()) break;
            cursor = page.next_cursor;
        }
        return n;
    };

    /// previewDeletes must be write-free.
    const size_t before = countObjects();
    const auto preview = gc.previewDeletes();
    EXPECT_EQ(countObjects(), before);

    /// Every previewed delete key must be unreachable per the independent fsck (the safety subset).
    const FsckReport report = runFsck(*store, /*detail=*/true);
    std::set<String> unreachable;
    for (const auto & o : report.objects)
        if (o.cls == FsckClass::Unreachable)
            unreachable.insert(o.key);
    for (const auto & p : preview)
        EXPECT_TRUE(unreachable.contains(p.key)) << "preview delete not unreachable: " << p.key;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/build_t4.log 2>&1
```
Expected: compile error — `previewDeletes` / `PreviewEntry` undefined.

- [ ] **Step 3: Declare the API in `CasGc.h`**

In the public section (after `RoundReport runRegularRound();`):
```cpp
    /// One previewed deletion the next regular round would make, with the reason it is eligible.
    struct PreviewEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        UInt128 hash{};
        String key;
        uint64_t size = 0;
        String reason;   /// "unreachable" (zero in-degree, present); future: "in_grace"/"fenced_pending"
    };

    /// WRITE-FREE preview of the next round's deletes, derived from the DURABLE gc/snap + gc/state.
    /// Reflects the snap as last persisted (run at quiescence for an exact picture). No CAS/delete.
    std::vector<PreviewEntry> previewDeletes();
```

- [ ] **Step 4: Implement `previewDeletes` in `CasGc.cpp`**

Mirror the retire candidate loop (`CasGc.cpp:587-631`) but emit instead of persisting, and `head` only to confirm existence + size. No `fold`/`fence`/`recheck`/`cascade`/`casPut`/`deleteExact`.
```cpp
std::vector<Gc::PreviewEntry> Gc::previewDeletes()
{
    std::vector<PreviewEntry> out;

    const auto state_bytes = store->backend().get(store->layout().gcStateKey());
    if (!state_bytes)
        return out;   /// no GC state yet => nothing retired
    const GcState state = decodeGcState(state_bytes->bytes);

    const Layout & layout = store->layout();
    Backend & backend = store->backend();
    std::map<uint64_t, GcSnap> snap = loadSnap(state);

    for (const auto & [snap_shard, shard_snap] : snap)
    {
        for (const Candidate & candidate : shard_snap.zeroInDegreeKnown())
        {
            const HeadResult observed = backend.head(objectKey(layout, candidate.kind, candidate.hash));
            if (!observed.exists)
                continue;   /// absent => already reclaimed; nothing to preview
            PreviewEntry e;
            e.kind = candidate.kind;
            e.hash = candidate.hash;
            e.key = objectKey(layout, candidate.kind, candidate.hash);
            e.size = observed.size;
            e.reason = "unreachable";
            out.push_back(std::move(e));
        }
    }
    return out;
}
```
(`loadSnap`, `decodeGcState`, `objectKey`, `zeroInDegreeKnown`, `Candidate`, `GcSnap` are all already in scope per the reference. `loadSnap` is a private member; `previewDeletes` is a member, so it can call it.)

- [ ] **Step 5: Build, run, commit**

```bash
ninja -C build unit_tests_dbms > build/build_t4.log 2>&1
./build/src/unit_tests_dbms --gtest_filter='CasGcRound.*:Cas*' > tmp/t4.log 2>&1
```
Expected: PASS, battery green.
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CA fsck T4: Gc::previewDeletes — write-free delete preview; {preview} subset {fsck unreachable}"
```

> **Adversarial-review gate (per spec §11 / M-C3 carry):** `previewDeletes` reads the real delete path's inputs. After Step 5, dispatch an adversarial reviewer (spec + model in hand) to confirm it (a) writes nothing on any path, (b) can only ever report a SUBSET of what a real round would delete (never more — a preview that over-reports could mislead a "safe to delete" decision), and (c) reuses the same candidate predicate (`zeroInDegreeKnown`) as the real round, not a divergent copy. Address findings before Task 5.

---

## Task 5: `clickhouse-disks fsck` command

**Files:**
- Create: `programs/disks/CommandFsck.cpp`
- Modify: `programs/disks/ICommand.h` (declare `makeCommandFsck()`)
- Modify: `programs/disks/DisksApp.cpp` (register `"fsck"`)
- Modify: `programs/disks/CMakeLists.txt` (only if it lists command sources explicitly)

The command resolves the current disk → `DiskObjectStorage` → `getMetadataStorage()` → `ContentAddressedMetadataStorage`, requires it to be content-addressed AND read-only (observe-only — refuses a writable CA disk so `clickhouse-disks` never probes/schedules a live pool), then calls `Cas::runFsck` and prints the report. Exit nonzero on `dangling > 0`.

- [ ] **Step 1: Implement the command**

```cpp
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h>
#include <Common/Exception.h>
#include "ICommand.h"

#include <iostream>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

class CommandFsck final : public ICommand
{
public:
    CommandFsck() : ICommand("CommandFsck")
    {
        command_name = "fsck";
        description = "Independently verify content-addressed pool reachability (read-only). "
                      "Exits nonzero if any reachable object is missing (dangling).";
        options_description.add_options()
            ("detail", "list per-object rows (key, class, size, reachable_from)");
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        const bool detail = options.count("detail") > 0;
        auto disk = client.getCurrentDiskWithPath().getDisk();

        auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
        if (!dos)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "fsck: '{}' is not an object-storage disk", disk->getName());
        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get());
        if (!ca)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "fsck: disk '{}' is not content-addressed", disk->getName());
        if (!ca->isReadOnly())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "fsck: open the CA disk read-only (<readonly>true</readonly>) so inspection never probes/schedules a live pool");

        const Cas::FsckReport report = Cas::runFsck(*ca->store(), detail);

        std::cout << "reachable=" << report.reachable
                  << " dangling=" << report.dangling
                  << " unreachable=" << report.unreachable
                  << " physical_bytes=" << report.physical_bytes
                  << " referenced_logical_bytes=" << report.referenced_logical_bytes
                  << " distinct_blobs=" << report.distinct_blobs
                  << " total_blob_refs=" << report.total_blob_refs
                  << " dedup_ratio=" << report.dedupRatio() << "\n";

        if (detail)
            for (const auto & o : report.objects)
            {
                const char * c = o.cls == Cas::FsckClass::Reachable ? "reachable"
                               : o.cls == Cas::FsckClass::Dangling ? "dangling" : "unreachable";
                std::cout << c << "\t" << o.key << "\t" << o.size;
                for (const auto & r : o.reachable_from)
                    std::cout << "\t" << r;
                std::cout << "\n";
            }

        if (report.dangling > 0)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "fsck: {} reachable object(s) MISSING (INV-NO-LOSS violation)", report.dangling);
    }
};

CommandPtr makeCommandFsck()
{
    return std::make_shared<DB::CommandFsck>();
}
}
```

- [ ] **Step 2: Declare + register**

In `programs/disks/ICommand.h`, alongside the other `makeCommandX()` decls (~`:120-136`):
```cpp
CommandPtr makeCommandFsck();
```
In `programs/disks/DisksApp.cpp`, alongside the other `command_descriptions.emplace(...)` (~`:320-335`):
```cpp
command_descriptions.emplace("fsck", makeCommandFsck());
```
If `programs/disks/CMakeLists.txt` lists command sources explicitly, add `CommandFsck.cpp`; if it globs, no change.

- [ ] **Step 3: Build the binary**

```bash
ninja -C build clickhouse-disks > build/build_t5.log 2>&1
```
Expected: links clean. (If `clickhouse-disks` is part of the main `clickhouse` multi-binary, build `clickhouse` instead — the subagent log summary will indicate the target.)

- [ ] **Step 4: Smoke-test against a real CA disk**

Create `tmp/fsck_smoke/config.xml` declaring a local CA disk over a temp dir (storage_configuration with `metadata_type=content_addressed`, an object-storage `type=local_blob_storage`, and `<readonly>true</readonly>`), pointed at a pool a prior writable run populated. Then:
```bash
./build/programs/clickhouse-disks --config-file tmp/fsck_smoke/config.xml --disk ca_ro --query "fsck --detail" > tmp/t5.log 2>&1; echo "exit=$?"
```
Expected: prints the summary line; `exit=0` on a clean pool. (Exact config authored at execution time from an existing CA disk config in the repo's test configs; a subagent summarizes the run.)

- [ ] **Step 5: Commit**

```bash
git add programs/disks/CommandFsck.cpp programs/disks/ICommand.h programs/disks/DisksApp.cpp programs/disks/CMakeLists.txt
git commit -m "CA fsck T5: clickhouse-disks fsck command (read-only reachability check, nonzero exit on dangling)"
```

---

## Task 6: `clickhouse-disks ca-gc-dryrun` command

**Files:**
- Create: `programs/disks/CommandCaGcDryRun.cpp`
- Modify: `programs/disks/ICommand.h`, `programs/disks/DisksApp.cpp`, `programs/disks/CMakeLists.txt` (as Task 5)

Same disk-resolution + read-only requirement; constructs a `Cas::Gc` over the read-only store and prints `previewDeletes()`.

- [ ] **Step 1: Implement the command**

```cpp
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h>
#include <Common/Exception.h>
#include "ICommand.h"

#include <iostream>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

class CommandCaGcDryRun final : public ICommand
{
public:
    CommandCaGcDryRun() : ICommand("CommandCaGcDryRun")
    {
        command_name = "ca-gc-dryrun";
        description = "Preview the next GC round's deletes for a content-addressed pool (read-only, no deletes).";
    }

    void executeImpl(const CommandLineOptions &, DisksClient & client) override
    {
        auto disk = client.getCurrentDiskWithPath().getDisk();
        auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
        if (!dos)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-gc-dryrun: '{}' is not an object-storage disk", disk->getName());
        auto * ca = dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get());
        if (!ca)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-gc-dryrun: disk '{}' is not content-addressed", disk->getName());
        if (!ca->isReadOnly())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-gc-dryrun: open the CA disk read-only");

        /// A non-leader, read-only Gc handle: previewDeletes never acquires the lease or writes.
        Cas::Gc gc(ca->store(), UInt128(1));
        const auto preview = gc.previewDeletes();
        std::cout << "preview_deletes=" << preview.size() << "\n";
        for (const auto & p : preview)
            std::cout << p.reason << "\t" << p.key << "\t" << p.size << "\n";
    }
};

CommandPtr makeCommandCaGcDryRun()
{
    return std::make_shared<DB::CommandCaGcDryRun>();
}
}
```

- [ ] **Step 2: Declare + register**

`ICommand.h`:
```cpp
CommandPtr makeCommandCaGcDryRun();
```
`DisksApp.cpp`:
```cpp
command_descriptions.emplace("ca-gc-dryrun", makeCommandCaGcDryRun());
```
`CMakeLists.txt`: add `CommandCaGcDryRun.cpp` if sources are explicit.

- [ ] **Step 3: Build**

```bash
ninja -C build clickhouse-disks > build/build_t6.log 2>&1
```
Expected: links clean.

- [ ] **Step 4: Smoke**

```bash
./build/programs/clickhouse-disks --config-file tmp/fsck_smoke/config.xml --disk ca_ro --query "ca-gc-dryrun" > tmp/t6.log 2>&1; echo "exit=$?"
```
Expected: prints `preview_deletes=N` + rows; `exit=0`.

- [ ] **Step 5: Commit**

```bash
git add programs/disks/CommandCaGcDryRun.cpp programs/disks/ICommand.h programs/disks/DisksApp.cpp programs/disks/CMakeLists.txt
git commit -m "CA fsck T6: clickhouse-disks ca-gc-dryrun command (write-free GC delete preview)"
```

---

## Task 7: Harden `clickhouse-disks list`/`read` on CA disks

**Files:**
- Possibly modify: `programs/disks/CommandList.cpp` / `CommandRead.cpp` and/or `ContentAddressedMetadataStorage.cpp` read paths, depending on what the smoke test surfaces.
- Test: a stateless smoke (`tmp/fsck_smoke/`) or a `.sh` under `tests/queries/0_stateless/` (decide at execution time).

This is a **verify-and-fix** task: `list`/`read` ride the existing `IDisk` read API, which the gtests already cover; the goal is to confirm they behave through the CLI against a populated CA disk and fix any path-resolution / virtual-directory / blob-read bug surfaced.

- [ ] **Step 1: Exercise `list` (recursive) and `read`**

Against the read-only CA disk from Task 5's config (populated with a part that has a content blob, a projection subdir, and mutable per-part files):
```bash
./build/programs/clickhouse-disks --config-file tmp/fsck_smoke/config.xml --disk ca_ro --query "list --recursive uui/uuid-1/all_1_1_0" > tmp/t7_list.log 2>&1; echo "list exit=$?"
./build/programs/clickhouse-disks --config-file tmp/fsck_smoke/config.xml --disk ca_ro --query "read uui/uuid-1/all_1_1_0/data.bin" > tmp/t7_read.log 2>&1; echo "read exit=$?"
```
Expected: `list` shows `data.bin`, the projection dir, and mutable files (reserved `.ca_mtime` filtered out); `read` emits the blob bytes. A subagent summarizes the logs.

- [ ] **Step 2: Triage & fix**

If `list`/`read` misbehave (wrong listing, read error on a blob-backed file, path-resolution failure), fix the smallest root cause — first check whether the bug reproduces at the gtest layer (`gtest_ca_wiring.cpp` `listDirectory`/`readFile`); if so, add a failing gtest there and fix under TDD; if it is CLI-only (path normalization in the command), fix in the command. **If `list`/`read` already work correctly, record that and skip to Step 4** (no change is a valid outcome — do not invent a fix).

- [ ] **Step 3: Re-run to confirm**

Re-run the Step 1 commands; expected clean output and `exit=0`.

- [ ] **Step 4: Commit**

```bash
# only the files actually changed; if nothing changed, commit the smoke test/notes only
git add -A
git commit -m "CA fsck T7: verify clickhouse-disks list/read on CA disks (+ fixes if any)"
```

---

## Final review

After Task 7, dispatch a final code reviewer over the whole `cas-fsck` change set (Tasks 1-7) — focus: the read-only fail-closed surface is complete (no mutating path reachable in read-only mode), `runFsck` reachability matches the protocol's reachability definition, `previewDeletes` is write-free and subset-safe, and the commands reject non-CA / writable disks cleanly. Then update the backlog (`docs/superpowers/deferred_backlog/cas-mergetree-integration.md`): mark sub-project A done, and record any deferred follow-ups (force-read-only at clickhouse-disks disk-build time so fsck works without reconfig; precise dedup-byte accounting; the `ca-gc-dryrun` in-grace/leaked `reason` split).
