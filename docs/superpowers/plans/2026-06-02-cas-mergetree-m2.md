---
description: M1 Phase 2 — ContentAddressedMetadataStorage (read/resolve) + registration, mirroring plain_rewritable.
sidebar_label: 'CAS MergeTree M2 plan'
sidebar_position: 2
slug: /superpowers/plans/cas-mergetree-m2
title: 'Content-Addressed MergeTree M1 — Phase 2 Plan (metadata storage, read/resolve)'
doc_type: 'guide'
---

# Content-Addressed MergeTree — Phase 2 Plan (metadata storage, read/resolve) {#cas-mergetree-m2-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Add a `metadata_type = content_addressed` object-storage metadata layer (`ContentAddressedMetadataStorage`) that resolves a part's logical files through the Phase-1 `Footer` — the **read/resolve** side only.

**Architecture:** A new `IMetadataStorage` subclass (mirroring `MetadataStorageFromPlainRewritableObjectStorage`) translates ClickHouse part paths (`<table_path>/<part_name>/<file>`) into the content-addressed pool: a per-part **ref** → `part_id` → `parts/<part_id>` **footer** (Phase-1 `Footer`) → `blobs/<file_checksum>`. In M1 *every* part file is its own blob (the footer's `blobs` map covers all files; `inlined` is reserved for B10), so `getStorageObjects` always returns a whole object and `FileCache`/readers are untouched. The write path (build-local-then-upload) is Phase 3; the transaction's write methods `throwNotImplemented()` here and tests seed the object store directly.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/`), `IMetadataStorage`/`IMetadataTransaction`, `MetadataStorageFactory`, `LocalObjectStorage` (gtest harness in `src/Disks/tests/`). Reuses Phase-1 `Footer` (`…/ContentAddressed/Footer.h`).

**Source spec:** `docs/superpowers/specs/2026-06-02-cas-mergetree-integration-design.md`. **Deferred (do NOT implement):** `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (B1–B19). Honor **B18** (fail-close on a missing footer for a live ref). Phase-1 is committed on branch `cas-mergetree-poc`.

---

## Build & test (same as Phase 1) {#build}

`build/` is configured; `unit_tests_dbms` is current. Build in the **background** (heavy link, exceeds foreground timeout): `cmake --build build --target unit_tests_dbms > build/cas_build.log 2>&1`; then `tail -n 60 build/cas_build.log` + `grep -nE "error:|FAILED|ninja: build stopped" build/cas_build.log`. Run: `build/src/unit_tests_dbms --gtest_filter='ContentAddressed*'`. New `.cpp` under `…/ContentAddressed/` are auto-globbed (dir registered at `src/CMakeLists.txt:129`); new `gtest_*.cpp` are auto-globbed (`grep_gtest_sources`).

## File structure {#file-structure}

```
src/Disks/DiskType.h                                                        # MODIFY: add MetadataStorageType::ContentAddressed
src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
  PoolPaths.h / PoolPaths.cpp        # NEW: pure path helpers (blobKey/partKey/refKey, parse part path) — TDD-able in isolation
  ContentAddressedMetadataStorage.h / .cpp   # NEW: the IMetadataStorage subclass (read/resolve)
  ContentAddressedTransaction.h / .cpp        # NEW: IMetadataTransaction (write methods throwNotImplemented in Phase 2)
src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp     # MODIFY: register "content_addressed"
src/Disks/tests/gtest_content_addressed_metadata.cpp                        # NEW: gtests (seed object store, assert resolve/list/read)
```

Decomposition rationale: the **path math** (`PoolPaths`) is pure and gets its own TDD'd file (the riskiest, fiddliest part isolated); the **metadata storage** is the `IMetadataStorage` glue; the **transaction** is a thin Phase-2 stub. Mirror throughout: `src/Disks/DiskObjectStorage/MetadataStorages/PlainRewritable/MetadataStorageFromPlainRewritableObjectStorage.{h,cpp}` and `Plain/MetadataStorageFromPlainObjectStorage.cpp` (esp. `getStorageObjects`, `listDirectory` using `object_storage->listObjects`).

---

## Task 0 (DISCOVERY, no code commit): confirm the path convention {#task-0}

Before writing path math, the implementer MUST confirm what relative paths `DiskObjectStorage` passes to the metadata storage, so `PoolPaths` parses the real format (not a guess).

- [ ] **Step 1:** Read how reads/listing reach the metadata storage: `src/Disks/DiskObjectStorage/DiskObjectStorage.cpp` (`prepareRead`/`exists`/`listDirectory` → `metadata_storage->getStorageObjects/iterateDirectory`), and how MergeTree discovers parts: `MergeTreeData::loadDataParts` (`disk->iterateDirectory(relative_data_path)`). Note the EXACT shape of the `path` argument (e.g. is it `store/<uuid-prefix>/<uuid>/<part_name>/<file>`? relative to disk root? trailing slashes?). Read `MergeTreeData::relative_data_path` construction.
- [ ] **Step 2:** Read the mirror's path handling: `Plain/MetadataStorageFromPlainObjectStorage.cpp::listDirectory`/`getStorageObjects` (how it derives a key prefix from `path` via `getKeyForPath(object_storage->getCommonKeyPrefix(), path)`) and `getCommonKeyPrefix()`.
- [ ] **Step 3:** Write findings as a short comment block at the top of `PoolPaths.h` documenting: the incoming path format, where the `<table_uuid>` and `<part_name>` and `<file>` sit, and the server-id source (`ServerUUID::get()`). No commit yet — this feeds Task 1.

### Task 0 findings (CONFIRMED — supersede the `store/<uuid>/…` examples below) {#task-0-findings}

- **Incoming path is disk-relative, no leading slash:** a part file is `<uuid[:3]>/<uuid>/<part_name>/<file>`; the table-parts directory passed to `iterateDirectory` is `<uuid[:3]>/<uuid>/` (from `DatabaseCatalog::getPathForUUID` = `toString(uuid).substr(0,3) + '/' + toString(uuid) + '/'`, `DatabaseCatalog.cpp:1662`). There is **no `store/` prefix** in the relative path. So `parsePartFilePath` splits on `/`: `[0]`=uuid-prefix (ignore), `[1]`=`table_uuid`, `[2]`=`part_name`, `[3..]`=`file` (≥3 comps = part dir, ≥4 = file).
- **Internal pool keys are OUR layout** (the incoming path only yields `table_uuid`+`part_name`): `blobs/…` and `parts/…` GLOBAL; refs at `store/<server_id>/<table_uuid>/refs/<part_name>` with `server_id = ServerUUID::get()`. Use **bare internal keys** for `StoredObject.remote_path`, and seed the gtest at the same bare keys (the `LocalObjectStorage` harness writes/reads `remote_path` directly under its `key_prefix` dir). If a live disk reports a non-empty `object_storage->getCommonKeyPrefix()`, prepend it uniformly the way `Plain/MetadataStorageFromPlainObjectStorage.cpp:26 getKeyForPath` does.
- **`iterateDirectory` returns `StaticDirectoryIterator`** (`…/MetadataStorages/StaticDirectoryIterator.h`), constructed from `std::vector<std::filesystem::path>` (mirror Plain: list names → `fs::path(path)/child` → `make_unique<StaticDirectoryIterator>`).
- **`ServerUUID::get()`** (`src/Core/ServerUUID.h`) → `UUID`; `ServerUUID::setRandomForUnitTests()` for the gtest SetUp; also `getIOThreadPool().initialize(1,1,0)`.
- **Harness:** `LocalObjectStorageSettings("test", "./" + key_prefix, false)` → `LocalObjectStorage`; the `writeObject(os, remote_path, data)` / `readObject(os, remote_path)` free helpers from `gtest_metadata_plain_rewritable_disk.cpp`.

Apply these to the function names/tests below (replace `store/<uuid>/…` with `<uuid[:3]>/<uuid>/…`; keep the helper names).

## Task 1: `PoolPaths` — pure path math (TDD) {#task-1}

**Files:** Create `…/ContentAddressed/PoolPaths.h` + `.cpp`; Create `src/Disks/tests/gtest_content_addressed_metadata.cpp`.

- [ ] **Step 1: Write the failing test** (`gtest_content_addressed_metadata.cpp`)

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PoolPaths.h>

using namespace DB::ContentAddressed;

TEST(ContentAddressedPoolPaths, ContentKeysFanOut)
{
    EXPECT_EQ(blobKey("abcdef0123"), "blobs/ab/cd/abcdef0123");
    EXPECT_EQ(partKey("0011223344"), "parts/00/11/0011223344");
}

TEST(ContentAddressedPoolPaths, RefKeyUsesServerAndTable)
{
    // refKey(server_id, table_uuid, part_name)
    EXPECT_EQ(refKey("srvA", "uuid-1", "all_1_1_0"), "store/srvA/uuid-1/refs/all_1_1_0");
}

TEST(ContentAddressedPoolPaths, ParsePartPath)
{
    // parsePartFilePath("store/uuid-1/all_1_1_0/columns.txt") -> {table_uuid, part_name, file}
    auto p = parsePartFilePath("store/uuid-1/all_1_1_0/columns.txt");
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->table_uuid, "uuid-1");
    EXPECT_EQ(p->part_name, "all_1_1_0");
    EXPECT_EQ(p->file, "columns.txt");
}
```

(Adjust the expected `store/<uuid>/…` shape in these expectations to the format confirmed in Task 0 before implementing — keep the function names.)

- [ ] **Step 2:** Build (background) → red (PoolPaths.h missing).
- [ ] **Step 3: Implement `PoolPaths.h`**

```cpp
#pragma once
#include <optional>
#include <string>

namespace DB::ContentAddressed
{

/// Content-addressed object keys with 2x2 hex prefix fan-out (S3 per-prefix limits).
std::string blobKey(const std::string & file_checksum);
std::string partKey(const std::string & part_id);

/// Per-server/per-table ref object key: store/<server_id>/<table_uuid>/refs/<part_name>.
std::string refKey(const std::string & server_id, const std::string & table_uuid, const std::string & part_name);
std::string refsPrefix(const std::string & server_id, const std::string & table_uuid);

struct PartFilePath
{
    std::string table_uuid;
    std::string part_name;
    std::string file; /// empty if the path is the part directory itself
};

/// Parse a ClickHouse relative part path (format confirmed in Task 0) into its components.
std::optional<PartFilePath> parsePartFilePath(const std::string & path);

}
```

- [ ] **Step 4: Implement `PoolPaths.cpp`** — `blobKey`/`partKey` prepend `blobs/`/`parts/` + `hash.substr(0,2) + "/" + hash.substr(2,2) + "/" + hash` (guard hashes shorter than 4 chars → no fan-out, used only in tests). `refKey`/`refsPrefix` concatenate as shown. `parsePartFilePath` splits on `/` per the Task-0 format. Use Allman braces; throw `DB::Exception(ErrorCodes::LOGICAL_ERROR, …)` on malformed input where a value is required (not for the optional-parse, which returns `std::nullopt`).
- [ ] **Step 5:** Build → green; run `--gtest_filter='ContentAddressedPoolPaths*'`; expect 3 PASS.
- [ ] **Step 6: Commit** — `git commit -m "CAS M1 P2: PoolPaths content-addressed key + part-path helpers"`

## Task 2: metadata-storage skeleton + registration + construct-it test {#task-2}

**Files:** Modify `src/Disks/DiskType.h`; Create `ContentAddressedMetadataStorage.{h,cpp}` + `ContentAddressedTransaction.{h,cpp}`; Modify `MetadataStorageFactory.cpp`; append to the gtest.

- [ ] **Step 1:** Add `ContentAddressed` to `enum class MetadataStorageType` in `src/Disks/DiskType.h` (after `PlainRewritable`). Find `magic_enum`/`toString` switches over `MetadataStorageType` (grep `MetadataStorageType::PlainRewritable`) and add the matching arm in each to keep `-Werror`/switch-exhaustiveness happy.
- [ ] **Step 2: Failing test** (append) — construct over `LocalObjectStorage`, mirroring `gtest_metadata_plain_rewritable_disk.cpp`'s harness (LocalObjectStorage over a temp dir; `ServerUUID::setRandomForUnitTests()` in SetUp):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
// ... harness copied from gtest_metadata_plain_rewritable_disk.cpp (LocalObjectStorage factory) ...

TEST_F(ContentAddressedMetaTest, ConstructAndType)
{
    auto ms = getMetadataStorage("cas1"); // builds ContentAddressedMetadataStorage over a LocalObjectStorage
    EXPECT_EQ(ms->getType(), DB::MetadataStorageType::ContentAddressed);
    EXPECT_FALSE(ms->isReadOnly());
    EXPECT_FALSE(ms->areBlobPathsRandom());
}
```

- [ ] **Step 3:** Build → red.
- [ ] **Step 4: Implement the class declarations + trivial bodies.** `ContentAddressedMetadataStorage.h` declares all `IMetadataStorage` pure-virtuals (see spec / `IMetadataStorage.h`); trivial ones inline: `getType()→MetadataStorageType::ContentAddressed`, `getPath()→storage_path_full`, `supportsChmod()→false`, `supportsStat()→false`, `isReadOnly()→false`, `areBlobPathsRandom()→false`, `getHardlinkCount()→0`. Constructor takes `(ObjectStoragePtr, String storage_path_prefix, String server_id)` (mirror the plain_rewritable ctor + add `server_id` from `ServerUUID::get()` at the registration site). The resolve methods (`existsFile`/`getFileSize`/`listDirectory`/`iterateDirectory`/`getStorageObjects`/`getLastModified`) are implemented in Task 3 — for now have them `throwNotImplemented()` (so the class is concrete and links). `createTransaction()` returns a `ContentAddressedTransaction` (Task 4 stub). Mirror file: `PlainRewritable/MetadataStorageFromPlainRewritableObjectStorage.h`.
- [ ] **Step 5: Register.** In `MetadataStorageFactory.cpp` add `void registerContentAddressedMetadataStorage(MetadataStorageFactory & factory)` mirroring `registerPlainRewritableMetadataStorage` (resolve the local object storage, build `ContentAddressedMetadataStorage` passing `ServerUUID::get()` as `server_id`), and call it in `registerMetadataStorages()`.
- [ ] **Step 6:** Build → green; `--gtest_filter='ContentAddressedMetaTest.ConstructAndType'` PASS.
- [ ] **Step 7: Commit** — `git commit -m "CAS M1 P2: ContentAddressedMetadataStorage skeleton + registration"`

## Task 3: ref→footer→blob resolution (the core read path) {#task-3}

**Files:** Modify `ContentAddressedMetadataStorage.{h,cpp}`; append to the gtest.

- [ ] **Step 1: Failing test** — seed the object store directly, then resolve/read. Helper `writeObject(object_storage, key, data)` is the one from `gtest_metadata_plain_rewritable_disk.cpp`. Build a `Footer` (Phase-1), serialize it to `parts/<part_id>`, put the blob at `blobs/<key>`, put the ref at `store/<server_id>/<uuid>/refs/<part>` containing `part_id`:

```cpp
TEST_F(ContentAddressedMetaTest, ResolvesAndReadsSeededPart)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas2");
    auto os = getObjectStorage("cas2");
    const std::string server_id = ms->serverIdForTest(); // test-only accessor, or pass the known id
    const std::string uuid = "uuid-2", part = "all_1_1_0", file = "data.bin";
    const std::string blob_data = "COLUMN-BYTES";
    const std::string blob_key = /* sha of blob_data; in test just */ "deadbeef01";

    writeObject(os, blobKey(blob_key), blob_data);
    Footer f; f.blobs[file] = BlobEntry{blob_key, blob_data.size(), blob_key};
    const std::string part_id = "cafe112233";
    writeObject(os, partKey(part_id), f.serialize());
    writeObject(os, refKey(server_id, uuid, part), part_id);

    const std::string logical = "store/" + uuid + "/" + part + "/" + file; // Task-0 format
    EXPECT_TRUE(ms->existsFile(logical));
    EXPECT_EQ(ms->getFileSize(logical), blob_data.size());
    auto objs = ms->getStorageObjects(logical);
    ASSERT_EQ(objs.size(), 1u);
    EXPECT_EQ(objs[0].remote_path, blobKey(blob_key));
    EXPECT_EQ(readObject(os, objs[0].remote_path), blob_data);
}

TEST_F(ContentAddressedMetaTest, FailsClosedOnMissingFooter)
{
    auto ms = getMetadataStorage("cas3");
    auto os = getObjectStorage("cas3");
    // ref present but its parts/<part_id> footer is absent → must THROW, not return empty (B18)
    writeObject(os, refKey(ms->serverIdForTest(), "uuid-3", "all_1_1_0"), "missingpartid");
    EXPECT_THROW(ms->getStorageObjects("store/uuid-3/all_1_1_0/data.bin"), DB::Exception);
}
```

- [ ] **Step 2:** Build → red.
- [ ] **Step 3: Implement resolution.** Add private helpers: `readRefPartId(table_uuid, part_name)` → read `refKey(server_id, …)` via `object_storage->readObject` into a string (the part_id); throw `DB::Exception(ErrorCodes::FILE_DOESNT_EXIST, …)` if the ref object is absent. `loadFooter(part_id)` → read `partKey(part_id)`; if absent **throw `DB::Exception(ErrorCodes::CORRUPTED_DATA, "CAS: live ref points at missing footer …")`** (B18 fail-close); else `Footer::deserialize(...)`. Cache footers in an LRU keyed by `part_id` (`std::list` + `std::unordered_map`, mutex-guarded; small cap, e.g. 1024 — mirror the spirit of plain_rewritable's caching but simpler). Then:
  - `existsFile(path)`: `parsePartFilePath` → resolve ref+footer → `footer.blobs.count(file) > 0`. Return false if the ref/footer/file is absent for `existsFile` (existence query), BUT `getStorageObjects`/`getFileSize` of an existing ref with a missing footer must throw (fail-close).
  - `getFileSize(path)`: resolve → `footer.blobs.at(file).size` (throw `FILE_DOESNT_EXIST` if file not in footer).
  - `getStorageObjects(path)`: resolve → `return {StoredObject(blobKey(footer.blobs.at(file).key), path, entry.size)}`.
  - `getLastModified(path)`: delegate to `object_storage->getLastModified` of the resolved blob (or the footer object) — confirm the available object-storage API; acceptable to return the blob's mtime.
  Add a test-only `serverIdForTest()` accessor (or have the harness construct with a fixed server id).
- [ ] **Step 4:** Build → green; run the two tests; PASS. Also run full `--gtest_filter='ContentAddressed*'` (Phase-1 tests still pass).
- [ ] **Step 5: Commit** — `git commit -m "CAS M1 P2: ref→footer→blob resolution (fail-close on missing footer, B18)"`

## Task 4: directory listing + transaction stub {#task-4}

**Files:** Modify `ContentAddressedMetadataStorage.{h,cpp}`, `ContentAddressedTransaction.{h,cpp}`; append to the gtest.

- [ ] **Step 1: Failing test** — seed two parts' refs; assert `iterateDirectory`/`listDirectory` over the table path returns the part names; assert a part-dir listing returns the footer's files; assert `createTransaction()` returns non-null and a write op throws not-implemented:

```cpp
TEST_F(ContentAddressedMetaTest, ListsPartsAndPartFiles)
{
    using namespace DB::ContentAddressed;
    auto ms = getMetadataStorage("cas4"); auto os = getObjectStorage("cas4");
    const std::string sid = ms->serverIdForTest(), uuid = "uuid-4";
    auto seed = [&](const std::string & part, const std::string & pid, const Footer & f)
    {
        writeObject(os, partKey(pid), f.serialize());
        writeObject(os, refKey(sid, uuid, part), pid);
    };
    Footer fa; fa.blobs["data.bin"] = {"k1", 3, "k1"}; fa.blobs["columns.txt"] = {"k2", 2, "k2"};
    seed("all_1_1_0", "pidA", fa);
    Footer fb; fb.blobs["data.bin"] = {"k3", 4, "k3"};
    seed("all_2_2_0", "pidB", fb);

    auto parts = ms->listDirectory("store/" + uuid); // the table dir
    std::set<std::string> got(parts.begin(), parts.end());
    EXPECT_EQ(got, (std::set<std::string>{"all_1_1_0", "all_2_2_0"}));

    auto files = ms->listDirectory("store/" + uuid + "/all_1_1_0");
    std::set<std::string> gotf(files.begin(), files.end());
    EXPECT_EQ(gotf, (std::set<std::string>{"data.bin", "columns.txt"}));
}

TEST_F(ContentAddressedMetaTest, TransactionWriteNotImplementedYet)
{
    auto ms = getMetadataStorage("cas5");
    auto tx = ms->createTransaction();
    ASSERT_NE(tx, nullptr);
    EXPECT_THROW(tx->writeStringToFile("store/uuid-5/all_1_1_0/x", "y"), DB::Exception); // Phase 3
}
```

- [ ] **Step 2:** Build → red.
- [ ] **Step 3: Implement listing.** `listDirectory(path)`/`iterateDirectory(path)`:
  - If `path` is a table dir (`store/<uuid>`): list `object_storage->listObjects(refsPrefix(server_id, uuid))`, strip the prefix → part names (mirror `Plain/MetadataStorageFromPlainObjectStorage.cpp::listDirectory`, which derives child names from `listObjects` results). Return them; `iterateDirectory` wraps the vector in a `DirectoryIterator` (reuse the same iterator type the plain storage returns — find it via the mirror).
  - If `path` is a part dir (`store/<uuid>/<part>`): resolve ref+footer → return `footer.blobs` keys.
  Implement `ContentAddressedTransaction`: declare it implementing `IMetadataTransaction`; `commit`/`tryCommit` are no-ops returning success for now (or throwNotImplemented — but `createTransaction` must return a usable object); `supportsChmod()→false`; `generateObjectKeyForPath` returns a placeholder (Phase 3 defines real keying); `getSubmittedForRemovalBlobs()→{}`; `createMetadataFile` → `throwNotImplemented()`. All other write methods inherit the `throwNotImplemented()` defaults. Add a comment: "Phase 3 implements build-local-then-upload here."
- [ ] **Step 4:** Build → green; run the two tests + full `ContentAddressed*`; PASS.
- [ ] **Step 5: Commit** — `git commit -m "CAS M1 P2: directory listing + Phase-2 transaction stub"`

---

## Self-review {#self-review}

- **Spec coverage:** §3 (the `ContentAddressedMetadataStorage` component + `Manifest`/Footer reuse), §4 (layout: `blobs/`/`parts/`/`store/<serverid>/<table_uuid>/refs/` keys — `PoolPaths`), §5 READ flow (ref→footer→blob via `getStorageObjects`; `iterateDirectory` lists refs), §8 (per-server refs namespace via `server_id`). The read path is covered; write (§5 INSERT), GC (§6), self-check (§8/§9), feature-gate (§9) are explicitly Phase 3–5.
- **Placeholder scan:** Task 0 is a discovery step (no commit) feeding the path format — necessary because the exact relative-path shape must be confirmed against live code, not guessed; every code task has concrete code or an exact mirror reference + test. The one parameter the implementer must finalize from Task 0 is the `store/<uuid>/…` path shape in the test expectations — flagged inline.
- **Type consistency:** `Footer`/`BlobEntry` (Phase 1) used as-is; `blobKey`/`partKey`/`refKey`/`refsPrefix`/`parsePartFilePath`/`PartFilePath` consistent across Tasks 1, 3, 4; `ContentAddressedMetadataStorage`/`ContentAddressedTransaction` signatures match `IMetadataStorage`/`IMetadataTransaction`.

## New deferral surfaced {#deferral}

- `getLastModified` for content-addressed parts has no natural per-part timestamp (objects are immutable/shared). Phase 2 returns the resolved blob's mtime; a cleaner per-part modified-time (e.g. from the ref object) is a **Phase 3+** refinement — appended to the backlog if it proves to matter for `system.parts`.

## Execution {#execution}

Implement Task 0 → Task 4 in order via `superpowers:subagent-driven-development`. Append any newly-surfaced deferrals to the backlog with plug-in points. After Phase 2, run `writing-plans` for Phase 3 (the write path).
