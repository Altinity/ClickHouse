# CAS part-file directory probes without an S3 LIST — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A path inside a resolved part on a CAS disk (`<table>/<part>/<file>`) is answered by `existsDirectory`, `listDirectory` and `isDirectoryEmpty` from the part-folder view, so a restart issues no S3 LIST per part file (issue https://github.com/Altinity/ClickHouse/issues/2439, backlog CAS-95.1).

**Architecture:** One new directory shape `PartFile` in `ContentAddressedMetadataStorage::classifyDirectory`, placed after `ProjectionDir` and before the table-subdirectory fall-through; `existsDirectory` and `listDirectory` answer it from `partAccess()->getView(...)` when the ref resolves and otherwise take today's branch through two helpers extracted verbatim from the existing `TableSubdir` case bodies. No pool, transaction, format or upstream `MergeTree` change.

**Tech Stack:** C++ (ClickHouse, Allman braces), gtest (`unit_tests_dbms`, filter `CAS*`), a stateless `.sh` test through `clickhouse-test`, ninja with ccache.

**Spec:** `docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md` (rev.13, on `cas-gc-rebuild`). The spec's §3.1 is the design; §4 lists the tests this plan implements; §2 lists what must not change.

## Global Constraints

- Target branch: `altinity/antalya-26.6` (spec line numbers refer to `8d62c314ec1`). The implementation lives in a NEW worktree on a NEW branch; the `master` worktree stays on `cas-gc-rebuild` and only receives the plan/spec/backlog commits.
- No fallback paths: a failed ref resolution or manifest read propagates; the old table-subdirectory branch is entered only on a resolved absence (`getView` returned null), never on a failed request.
- The on-S3 layout, the request protocol (HEAD-before-PUT and friends) and `CasPlainObjects` are untouched.
- Every existing answer stays the same except the one in spec §2: a non-projection nested directory inside a resolved part answers present, lists its children and `isDirectoryEmpty` answers false. `ProjectionDir` keeps its empty answer (`ContentAddressedMetadataStorage.cpp:1939-1945`).
- Allman braces; comments keep the reason, never a plan or backlog reference; no `LOGICAL_ERROR` introduced; no `no-parallel` tag on the stateless test; `add-test` assigns the test number.
- Commit by explicit paths only (`git add <file>` and `git commit -- <file>`), never a directory, never `-A`. New commits only, no rebase, no amend. Never push; the user pushes.
- Build output goes to a log file in the build directory; a subagent summarizes the log. Test output goes to a unique log file per run in the build directory.
- Commit messages end with the attribution lines the session reminder gives (`Co-Authored-By` and `Claude-Session`).

## Review Focus

Inputs the spec implies but no §4 test names; each has its test added to the owning task below.

1. A probe path with a trailing slash (`<table>/<part>/sub/`): the route's `file` would become `sub/` and the prefix `sub//`, answering absent for a present directory. Expected: same answer as without the slash. Test in Task 2.
2. A probe on a part that is being written in an open transaction and not yet published (`tmp_insert_*` staged locally): the ref does not resolve, the old branch runs, the answer and the one LIST are today's. Expected: no exception, no change. Test in Task 3.
3. A ref that resolves but whose manifest cannot be read (both caches disabled, the object storage fails the GET): the exception must propagate; the old LIST branch must not run. Test in Task 3 (spec test 4b).
4. A path inside a part after `moveDirectory` to `detached/`: the detached ref resolves and the file answers false with zero LIST. Test in Task 2.
5. A table-level subdirectory whose first component collides with no ref but is part-shaped (`<table>/all_9_9_0/x` with no such part): the old branch, one LIST, today's answer. Test in Task 3 (spec test 3).

---

### Task 0: Worktree, branch and build

**Files:**
- Create: worktree `/home/mfilimonov/workspace/ClickHouse/cas-95-1` on branch `fix/antalya-26.6/cas-part-file-probes-no-list` from `altinity/antalya-26.6`
- Create: `/home/mfilimonov/workspace/ClickHouse/cas-95-1/build/` (cmake, ccache, same options as the `lane-g` build)

**Interfaces:**
- Produces: `W=/home/mfilimonov/workspace/ClickHouse/cas-95-1` (every later task runs inside it), `$W/build/src/unit_tests_dbms`, `$W/build/programs/clickhouse`.

- [ ] **Step 1: Create the worktree from the fork branch**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git fetch altinity antalya-26.6
git worktree add /home/mfilimonov/workspace/ClickHouse/cas-95-1 -b fix/antalya-26.6/cas-part-file-probes-no-list altinity/antalya-26.6
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git submodule update --init --recursive 2>&1 | tail -3
git log -1 --format='%h %s'
```

Expected: HEAD is `8d62c314ec1` or a later `antalya-26.6` commit (if later, re-check the spec's line numbers before editing; the functions are the same).

- [ ] **Step 2: Configure the build with the lane-g options**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
grep -E "^(CMAKE_C_COMPILER|CMAKE_CXX_COMPILER|CMAKE_CXX_COMPILER_LAUNCHER|CMAKE_C_COMPILER_LAUNCHER|CMAKE_BUILD_TYPE|ENABLE_TESTS|ENABLE_CLICKHOUSE_ALL|COMPILER_CACHE):" ../lane-g/build/CMakeCache.txt
```

Take the printed values and configure with them (example, adjust to what was printed):

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER="$(grep '^CMAKE_C_COMPILER:' ../lane-g/build/CMakeCache.txt | cut -d= -f2)" \
  -DCMAKE_CXX_COMPILER="$(grep '^CMAKE_CXX_COMPILER:' ../lane-g/build/CMakeCache.txt | cut -d= -f2)" \
  -DCOMPILER_CACHE=ccache -DENABLE_TESTS=1 > build/cmake.log 2>&1; tail -3 build/cmake.log
```

Expected: `-- Build files have been written to: .../cas-95-1/build`.

- [ ] **Step 3: Build the unit-test binary and the server in the background**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
nohup sh -c 'ninja -C build unit_tests_dbms clickhouse > build/build_task0.log 2>&1; echo NINJA_EXIT=$? >> build/build_task0.log' > /dev/null 2>&1 &
```

Wait for `NINJA_EXIT=` in `build/build_task0.log` (no `-j`, no `nproc`). Have a subagent summarize the log; the summary must say `NINJA_EXIT=0`.

- [ ] **Step 4: Verify the CAS gate runs on the untouched tree**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
build/src/unit_tests_dbms --gtest_filter='CASWiring*' > build/test_task0_wiring.log 2>&1; tail -3 build/test_task0_wiring.log
```

Expected: `[  PASSED  ]` with no failures. Nothing to commit.

---

### Task 1: The `PartFile` shape with today's answers (routing only)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:508-521` (enum), private helpers near line 472
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1529-1622` (`classifyDirectory`), `:1702-1712` and `:1895-1904` (the two `TableSubdir` case bodies become helpers), the two switches gain a `PartFile` case
- Test: `src/Disks/tests/gtest_ca_wiring.cpp:862-873` (the dispatch-order test)

**Interfaces:**
- Produces: `DirShape::PartFile`; `DirRoute::tf` set for a `PartFile` on an Atomic path; private `bool tableSubdirExists(const Cas::TableFilePath & tf) const` and `std::vector<std::string> tableSubdirChildren(const Cas::TableFilePath & tf) const`.
- Consumes: `Route`, `Cas::parsePartFilePath`, `Cas::parseTableFilePath`, `Cas::PartFolderView::projectionDirPrefix`, `liveTreeDirHasChildren`, `listLiveTreeChildren`, `addFirstComponent`, `toVector` (all existing).

- [ ] **Step 1: Write the failing routing assertions**

In `src/Disks/tests/gtest_ca_wiring.cpp`, inside the existing dispatch test that ends at line 873 (the one with `using DS = DB::ContentAddressedMetadataStorage::DirShape;`), add before the closing brace:

```cpp
    /// A path INSIDE a part (file or nested directory): its own shape, decided by the ref at answer
    /// time. Atomic, detached, moving, non-Atomic and a temporary restore part all route here; a
    /// projection dir, a table-level subdir and a shadow part file keep their shapes.
    const std::string tbl = "a11/a11a11a1-1111-4111-8111-111111111111";
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/all_1_1_0/columns.txt").shape,            DS::PartFile);
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/all_1_1_0/sub").shape,                    DS::PartFile);
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/detached/all_1_1_0/columns.txt").shape,   DS::PartFile);
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/moving/all_1_1_0/columns.txt").shape,     DS::PartFile);
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/tmp_restore_all_1_1_0-abcdefgh/columns.txt").shape, DS::PartFile);
    EXPECT_EQ(storage->classifyDirectoryForTest("data/db/tbl/all_1_1_0/columns.txt").shape,        DS::PartFile);
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/all_1_1_0/p.proj").shape,                 DS::ProjectionDir);
    EXPECT_EQ(storage->classifyDirectoryForTest(tbl + "/deduplication_logs").shape,               DS::TableSubdir);
    EXPECT_EQ(storage->classifyDirectoryForTest("shadow/bk1/store/" + tbl + "/all_1_1_0/columns.txt").shape, DS::ShadowIntermediate);
    /// The old branch's parse travels with the shape: present on an Atomic path, absent on non-Atomic.
    EXPECT_TRUE(storage->classifyDirectoryForTest(tbl + "/all_1_1_0/columns.txt").tf.has_value());
    EXPECT_FALSE(storage->classifyDirectoryForTest("data/db/tbl/all_1_1_0/columns.txt").tf.has_value());
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
ninja -C build unit_tests_dbms > build/build_task1a.log 2>&1; grep -m3 "error:" build/build_task1a.log
```

Expected: a compile error `no member named 'PartFile' in 'DB::ContentAddressedMetadataStorage::DirShape'`.

- [ ] **Step 3: Add the enum value and the helper declarations**

In `ContentAddressedMetadataStorage.h`, in `enum class DirShape` (line 508), add after `ProjectionDir,`:

```cpp
        PartFile,
```

Near the declarations of `listLiveTreeChildren` / `liveTreeDirHasChildren` (line 472-475), add:

```cpp
    /// The table-level-subdirectory answers (a LIST of the life's `_files/` prefix filtered by
    /// `tf.tail + "/"`), shared by the `TableSubdir` shape and by a `PartFile` whose ref does not
    /// resolve, which must answer exactly as it did before that shape existed.
    bool tableSubdirExists(const Cas::TableFilePath & tf) const;
    std::vector<std::string> tableSubdirChildren(const Cas::TableFilePath & tf) const;
```

- [ ] **Step 4: Classify the shape and route it to today's answers**

In `ContentAddressedMetadataStorage.cpp`, `classifyDirectory`, after the `ProjectionDir` block (the `if (r && !r->ref.empty())` that checks `projectionDirPrefix`) and before the `/// No sub-shape matched` comment, add:

```cpp
        /// A path with a part-shaped component followed by more components: a file or nested
        /// directory of a live, detached or moving part IF that ref resolves (shadow is routed
        /// above). The parser calls every first component after the table root except
        /// `deduplication_logs` the part component, so whether this really is a part is decided by
        /// the ref at answer time, not by the path: `existsDirectory`/`listDirectory` take the old
        /// table-subdirectory branch when it does not resolve. Classification stays pure path
        /// computation, so the parse that branch needs travels with the shape.
        if (r && !r->ref.empty() && !r->file.empty())
        {
            dr.shape = DirShape::PartFile;
            dr.p = std::move(p);
            dr.r = std::move(r);
            dr.tf = Cas::parseTableFilePath(path);
            return dr;
        }
```

Replace the `TableSubdir` case body of `existsDirectory` (`:1702-1712`) with:

```cpp
        case DirShape::TableSubdir:
            return tableSubdirExists(*dr.tf);
        case DirShape::PartFile:
            /// Answered as the table subdirectory or generic directory the same path was classified
            /// as before this shape existed (the view-based answer replaces this in the next commit).
            return dr.tf ? tableSubdirExists(*dr.tf) : liveTreeDirHasChildren(path);
```

Replace the `TableSubdir` case body of `listDirectory` (`:1895-1904`) with:

```cpp
        case DirShape::TableSubdir:
            return tableSubdirChildren(*dr.tf);
        case DirShape::PartFile:
            return dr.tf ? tableSubdirChildren(*dr.tf) : listLiveTreeChildren(path);
```

Add the two helpers next to `liveTreeDirHasChildren`'s definition, verbatim from the old case bodies:

```cpp
bool ContentAddressedMetadataStorage::tableSubdirExists(const Cas::TableFilePath & tf) const
{
    /// At least one verbatim file under it.
    const auto life = readableNamespaceFilesLife(liveNamespace(tf.table_uuid));
    if (!life)
        return false;
    const std::string prefix = tf.tail + "/";
    for (const auto & name : store()->listNamespaceFiles(*life))
        if (name.starts_with(prefix))
            return true;
    return false;
}

std::vector<std::string> ContentAddressedMetadataStorage::tableSubdirChildren(const Cas::TableFilePath & tf) const
{
    /// Verbatim files under <subdir>/, first-component collapsed.
    std::unordered_set<std::string> result;
    if (const auto life = readableNamespaceFilesLife(liveNamespace(tf.table_uuid)))
        for (const auto & name : store()->listNamespaceFiles(*life))
            if (name.starts_with(tf.tail + "/"))
                addFirstComponent(result, name.substr(tf.tail.size() + 1));
    return toVector(std::move(result));
}
```

(If the old `TableSubdir` bodies on the checked-out commit differ from the spec's citation, copy the checked-out bodies; the helper must be a verbatim extraction.) Every other `switch (dr.shape)` in the file (grep for `case DirShape::TableSubdir`) that has no `PartFile` case gets one that behaves as `TableSubdir` did, so `-Wswitch` stays clean.

- [ ] **Step 5: Build and run the wiring tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
ninja -C build unit_tests_dbms > build/build_task1b.log 2>&1; echo NINJA_EXIT=$? >> build/build_task1b.log; tail -1 build/build_task1b.log
build/src/unit_tests_dbms --gtest_filter='CASWiring*:CASNamespaceFile*' > build/test_task1.log 2>&1; tail -3 build/test_task1.log
```

Expected: `NINJA_EXIT=0`, `[  PASSED  ]`, including the request-profile gate (`CASNamespaceFileRequestProfile.*`), whose literal counts must not move.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_ca_wiring.cpp
git commit -m "CAS: route a path inside a part to its own directory shape

`classifyDirectory` gains \`DirShape::PartFile\` for \`<table>/<part>/<file>\`
(live, detached, moving, non-Atomic); the two \`TableSubdir\` case bodies
become \`tableSubdirExists\`/\`tableSubdirChildren\` and the new shape answers
through them for now, so no answer changes in this commit.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Disks/tests/gtest_ca_wiring.cpp
git show --stat HEAD | tail -4
```

Expected: exactly three files in the commit.

---

### Task 2: Answer a resolved `PartFile` from the part-folder view

**Files:**
- Create: `src/Disks/tests/gtest_cas_directory_probes.cpp` (the `CountingObjectStorage` seam and spec test 2)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (the two `PartFile` cases from Task 1)
- Modify: `src/Disks/tests/CMakeLists.txt` only if test sources are listed explicitly there (check with `grep -n gtest_ca_wiring src/Disks/tests/CMakeLists.txt`; the tree globs `gtest_*.cpp` by default and needs no change)

**Interfaces:**
- Produces: `CountingObjectStorage` (a `DB::LocalObjectStorage` recording `(kind, key)` with `listCount(prefix)`, `getCount(prefix)`, `headCount(prefix)`, `reset()`, and a `fail_reads_containing` needle for Task 3), `openCountingStorage(out_object_storage, disable_caches)` returning a started `ContentAddressedMetadataStorage`, `publishPart(storage, part_path, files)` publishing a part through the transaction, and the constants `kTbl`, `kNonAtomicTbl`.
- Consumes: `DirShape::PartFile`, `tableSubdirExists`, `tableSubdirChildren` from Task 1; `DB::Cas::tests::makeSettingsForTest` from `cas_test_helpers.h`; `partAccess()->getView`, `PartFolderView::hasDirectory`, `PartFolderView::listChildren`.

- [ ] **Step 1: Write the counting seam and the failing test**

Create `src/Disks/tests/gtest_cas_directory_probes.cpp`:

```cpp
#include <gtest/gtest.h>
#include "cas_test_helpers.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>
#include <Disks/ObjectStorages/Local/LocalObjectStorage.h>
#include <IO/WriteHelpers.h>
#include <Core/Defines.h>

#include <fmt/ranges.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace DB::ErrorCodes
{
    extern const int CANNOT_READ_ALL_DATA;
}

/// Per-TU declarations of the settings this file overrides, the pattern `cas_test_helpers.h`
/// documents: defined once in `ContentAddressedSettings.cpp`, declared by each consumer.
namespace DB::ContentAddressedSetting
{
    extern const ContentAddressedSettingsBool gc_enabled;
    extern const ContentAddressedSettingsUInt64 part_folder_cache_bytes;
    extern const ContentAddressedSettingsUInt64 manifest_decode_cache_bytes;
}

/// Directory probes on a path INSIDE a part (`<table>/<part>/<file>`) must be answered from the
/// part-folder view, never by a LIST of the table's `_files/` prefix. The metadata storage builds its
/// own backend from an `ObjectStoragePtr`, so the instrument sits at the `IObjectStorage` layer: a
/// `LocalObjectStorage` that counts every operation it is asked, by kind and key. Every method
/// `CasObjectStorageBackend` reaches is overridden, so "zero LISTs" is satisfied by the behaviour, not
/// by an unrecorded path.

using namespace DB::Cas::tests;

namespace
{

class CountingObjectStorage : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    enum class Kind { List, Get, Head, Put, Delete };

    bool exists(const DB::StoredObject & object) const override
    {
        record(Kind::Head, object.remote_path);
        return DB::LocalObjectStorage::exists(object);
    }

    std::unique_ptr<DB::ReadBufferFromFileBase> readObject(
        const DB::StoredObject & object, const DB::ReadSettings & read_settings,
        std::optional<size_t> read_hint, bool use_external_buffer,
        bool restrict_seek) const override
    {
        record(Kind::Get, object.remote_path);
        failIfArmed(object.remote_path);
        return DB::LocalObjectStorage::readObject(object, read_settings, read_hint, use_external_buffer, restrict_seek);
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject & object, DB::WriteMode mode,
        std::optional<DB::ObjectAttributes> attributes,
        size_t buf_size,
        const DB::WriteSettings & write_settings) override
    {
        record(Kind::Put, object.remote_path);
        return DB::LocalObjectStorage::writeObject(object, mode, attributes, buf_size, write_settings);
    }

    void removeObjectIfExists(const DB::StoredObject & object) override
    {
        record(Kind::Delete, object.remote_path);
        DB::LocalObjectStorage::removeObjectIfExists(object);
    }

    void removeObjectsIfExist(const DB::StoredObjects & objects) override
    {
        for (const DB::StoredObject & object : objects)
            record(Kind::Delete, object.remote_path);
        DB::LocalObjectStorage::removeObjectsIfExist(objects);
    }

    DB::ObjectMetadata getObjectMetadata(const std::string & path, bool with_tags) const override
    {
        record(Kind::Head, path);
        return DB::LocalObjectStorage::getObjectMetadata(path, with_tags);
    }

    std::optional<DB::ObjectMetadata> tryGetObjectMetadata(const std::string & path, bool with_tags) const override
    {
        record(Kind::Head, path);
        return DB::LocalObjectStorage::tryGetObjectMetadata(path, with_tags);
    }

    void listObjects(const std::string & path, DB::RelativePathsWithMetadata & children, size_t max_keys) const override
    {
        record(Kind::List, path);
        DB::LocalObjectStorage::listObjects(path, children, max_keys);
    }

    bool existsOrHasAnyChild(const std::string & path) const override
    {
        record(Kind::List, path);
        return DB::LocalObjectStorage::existsOrHasAnyChild(path);
    }

    void copyObject(
        const DB::StoredObject & object_from, const DB::StoredObject & object_to,
        const DB::ReadSettings & read_settings, const DB::WriteSettings & write_settings,
        std::optional<DB::ObjectAttributes> object_to_attributes) override
    {
        record(Kind::Get, object_from.remote_path);
        record(Kind::Put, object_to.remote_path);
        DB::LocalObjectStorage::copyObject(object_from, object_to, read_settings, write_settings, object_to_attributes);
    }

    size_t listCount(std::string_view prefix) const { return count(Kind::List, prefix); }
    size_t getCount(std::string_view prefix) const { return count(Kind::Get, prefix); }
    size_t headCount(std::string_view prefix) const { return count(Kind::Head, prefix); }

    /// Every recorded key of `kind` containing `needle`, so a failure names the offender.
    std::vector<String> keys(Kind kind, std::string_view needle) const
    {
        std::lock_guard lock(mutex);
        std::vector<String> out;
        for (const auto & [k, key] : records)
            if (k == kind && key.find(needle) != String::npos)
                out.push_back(key);
        return out;
    }

    void reset()
    {
        std::lock_guard lock(mutex);
        records.clear();
    }

    /// Task 3: every GET of a key containing `needle` throws, so a resolved ref whose manifest
    /// cannot be read is an error, never a fall-through.
    void failReadsContaining(String needle)
    {
        std::lock_guard lock(mutex);
        fail_reads_containing = std::move(needle);
    }

private:
    void record(Kind kind, const std::string & key) const
    {
        std::lock_guard lock(mutex);
        records.emplace_back(kind, key);
    }

    void failIfArmed(const std::string & key) const
    {
        std::lock_guard lock(mutex);
        if (!fail_reads_containing.empty() && key.find(fail_reads_containing) != String::npos)
            throw DB::Exception(DB::ErrorCodes::CANNOT_READ_ALL_DATA, "CountingObjectStorage: injected read failure on '{}'", key);
    }

    size_t count(Kind kind, std::string_view prefix) const
    {
        std::lock_guard lock(mutex);
        size_t n = 0;
        for (const auto & [k, key] : records)
            if (k == kind && key.starts_with(prefix))
                ++n;
        return n;
    }

    mutable std::mutex mutex;
    mutable std::vector<std::pair<Kind, String>> records;
    String fail_reads_containing;
};

const std::string kTbl = "a11/a11a11a1-1111-4111-8111-111111111111";
const std::string kNonAtomicTbl = "data/db/tbl";

std::shared_ptr<DB::ContentAddressedMetadataStorage> openCountingStorage(
    std::shared_ptr<CountingObjectStorage> & out_object_storage, bool disable_caches)
{
    static std::atomic<uint64_t> counter{0};
    const String unique = std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    const auto root = (std::filesystem::temp_directory_path() / ("cas_dir_probes_" + unique)).string();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    out_object_storage = std::make_shared<CountingObjectStorage>(
        DB::LocalObjectStorageSettings("test", root, /*read_only_=*/false));

    auto settings = makeSettingsForTest(
        "test", std::filesystem::temp_directory_path() / ("cas_dir_probes_scratch_" + unique));
    /// A GC round LISTs on its own schedule; a timer is not a fence, so keep it off.
    settings[DB::ContentAddressedSetting::gc_enabled] = false;
    if (disable_caches)
    {
        settings[DB::ContentAddressedSetting::part_folder_cache_bytes] = 0;
        settings[DB::ContentAddressedSetting::manifest_decode_cache_bytes] = 0;
    }
    auto storage = std::make_shared<DB::ContentAddressedMetadataStorage>(
        out_object_storage, "pool", "srv1", "", nullptr, settings);
    storage->startup();
    return storage;
}

/// Publishes one part through the real transaction path: every (relative file, bytes) pair is written
/// and the transaction is committed, exactly as a MergeTree part write does.
void publishPart(DB::ContentAddressedMetadataStorage & storage, const std::string & part_path,
                 const std::vector<std::pair<std::string, std::string>> & files)
{
    auto tx = storage.createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    for (const auto & [file, bytes] : files)
    {
        auto buf = ca_tx.writeFile(part_path + "/" + file, 65536, DB::WriteMode::Rewrite, {});
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    tx->commit(DB::NoCommitOptions{});
}

std::string filesPrefixOf(DB::ContentAddressedMetadataStorage & storage, const std::string & table_path)
{
    /// The LIST that must not happen: the life's `_files/` prefix. Resolved through the storage so a
    /// layout change moves the assertion instead of silently matching nothing.
    const auto uuid = table_path.substr(table_path.find_last_of('/') + 1);
    const auto life = storage.readableNamespaceFilesLife(storage.liveNamespace(uuid));
    return life ? storage.store()->layout().namespaceFilesPrefix(*life) : "cas/ns/state/";
}

}

/// A file of a published part is not a directory; a non-projection nested directory is, lists its
/// children and is not empty; a projection directory keeps its deliberate empty answer. None of it
/// LISTs the table's `_files/` prefix, with or without the view caches.
class CASDirectoryProbes : public ::testing::TestWithParam<bool> {};

TEST_P(CASDirectoryProbes, PartFileAnswersFromTheViewWithoutAList)
{
    const bool disable_caches = GetParam();
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, disable_caches);

    const std::string part = kTbl + "/all_1_1_0";
    publishPart(*storage, part, {
        {"columns.txt", "cols"}, {"data.bin", "data-bytes"},
        {"sub/inner.bin", "inner"}, {"p.proj/data.bin", "proj-bytes"}});
    /// The load has happened; from here on every probe must be answered from what it retained.
    ASSERT_TRUE(storage->existsDirectory(part));
    const std::string files_prefix = filesPrefixOf(*storage, kTbl);
    os->reset();

    EXPECT_FALSE(storage->existsDirectory(part + "/columns.txt"));
    EXPECT_FALSE(storage->existsDirectory(part + "/data.bin"));
    EXPECT_TRUE(storage->existsDirectory(part + "/sub"));
    EXPECT_TRUE(storage->existsDirectory(part + "/sub/"));            /// Review Focus 1: trailing slash
    EXPECT_EQ(storage->listDirectory(part + "/sub"), (std::vector<std::string>{"inner.bin"}));
    EXPECT_FALSE(storage->isDirectoryEmpty(part + "/sub"));
    EXPECT_TRUE(storage->isDirectoryEmpty(part + "/p.proj"));          /// ProjectionDir keeps its answer
    EXPECT_TRUE(storage->existsDirectory(part + "/p.proj"));
    EXPECT_FALSE(storage->existsDirectory(part + "/absent"));

    EXPECT_EQ(os->listCount(files_prefix), 0u) << "keys: " << fmt::join(os->keys(CountingObjectStorage::Kind::List, "_files"), ", ");
    EXPECT_EQ(os->listCount(""), 0u) << "no LIST of any prefix for probes inside a resolved part";
    if (!disable_caches)
        EXPECT_EQ(os->getCount(""), 0u) << "warm view: no GET at all";
    else
        EXPECT_GT(os->getCount(storage->store()->layout().casManifestsPrefix()), 0u) << "cold caches: the manifest is read, never listed";
}

INSTANTIATE_TEST_SUITE_P(Caches, CASDirectoryProbes, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool> & info) { return info.param ? "Disabled" : "Default"; });

/// Detached (Review Focus 4) and non-Atomic parts route through the same shape and the same view.
TEST(CASDirectoryProbes, DetachedAndNonAtomicPartFilesAnswerWithoutAList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);

    const std::string part = kTbl + "/all_2_2_0";
    publishPart(*storage, part, {{"columns.txt", "cols"}, {"sub/x.bin", "x"}});
    {
        auto tx = storage->createTransaction();
        tx->moveDirectory(part, kTbl + "/detached/all_2_2_0");
        tx->commit(DB::NoCommitOptions{});
    }
    const std::string detached = kTbl + "/detached/all_2_2_0";
    ASSERT_TRUE(storage->existsDirectory(detached));

    const std::string na_part = kNonAtomicTbl + "/all_1_1_0";
    publishPart(*storage, na_part, {{"columns.txt", "cols"}, {"sub/y.bin", "y"}});
    ASSERT_TRUE(storage->existsDirectory(na_part));

    os->reset();
    EXPECT_FALSE(storage->existsDirectory(detached + "/columns.txt"));
    EXPECT_TRUE(storage->existsDirectory(detached + "/sub"));
    EXPECT_FALSE(storage->existsDirectory(na_part + "/columns.txt"));
    EXPECT_TRUE(storage->existsDirectory(na_part + "/sub"));
    EXPECT_EQ(os->listCount(""), 0u) << "keys: " << fmt::join(os->keys(CountingObjectStorage::Kind::List, ""), ", ");
}
```

If `readableNamespaceFilesLife`, `liveNamespace` or `store()` are not public on the checked-out header, add a one-line public test accessor next to `classifyDirectoryForTest`:

```cpp
    /// Test-only: the `_files/` prefix of a live table's current life, for "this LIST must not happen".
    std::string namespaceFilesPrefixForTest(const std::string & table_uuid) const
    {
        const auto life = readableNamespaceFilesLife(liveNamespace(table_uuid));
        return life ? store()->layout().namespaceFilesPrefix(*life) : std::string{};
    }
```

and use it in `filesPrefixOf` instead.

- [ ] **Step 2: Build and run to verify the test fails**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
ninja -C build unit_tests_dbms > build/build_task2a.log 2>&1; echo NINJA_EXIT=$? >> build/build_task2a.log; tail -1 build/build_task2a.log
build/src/unit_tests_dbms --gtest_filter='*CASDirectoryProbes*' > build/test_task2a.log 2>&1; grep -E "FAILED|listCount|existsDirectory" build/test_task2a.log | head
```

Expected: `NINJA_EXIT=0`; the `Default` and `Disabled` cases FAIL on `existsDirectory(part + "/sub")` (false today) and on `listCount(files_prefix) == 0` (today every probe LISTs). The trailing-slash and detached assertions fail the same way.

- [ ] **Step 3: Answer from the view**

In `ContentAddressedMetadataStorage.cpp`, replace the Task 1 `PartFile` case of `existsDirectory` with:

```cpp
        case DirShape::PartFile:
        {
            /// A resolved part answers from its folder view: a plain file has no entries under
            /// `<file>/`, a nested directory has. An unresolved ref is not a part we know
            /// (`<table>/custom/sub`, a non-Atomic part-shaped table component) and answers exactly
            /// as before this shape existed. A failed resolution or manifest read propagates.
            auto view = partAccess()->getView(dr.r->refKey(), Cas::Freshness::CachedForLoad);
            if (view)
                return view->hasDirectory(dirPrefixOf(dr.r->file));
            return dr.tf ? tableSubdirExists(*dr.tf) : liveTreeDirHasChildren(path);
        }
```

and the `PartFile` case of `listDirectory` with:

```cpp
        case DirShape::PartFile:
        {
            auto view = partAccess()->getView(dr.r->refKey(), Cas::Freshness::CachedForLoad);
            if (view)
                return view->listChildren(dirPrefixOf(dr.r->file));
            return dr.tf ? tableSubdirChildren(*dr.tf) : listLiveTreeChildren(path);
        }
```

Add a file-local helper above `classifyDirectory` (Review Focus 1: a probe path may carry a trailing slash, and `entryRange` needs exactly one):

```cpp
namespace
{

/// The manifest prefix of a directory inside a part: the route's file with exactly one trailing
/// slash, whether or not the probe path carried one.
std::string dirPrefixOf(const std::string & file)
{
    if (file.ends_with('/'))
        return file;
    return file + "/";
}

}
```

- [ ] **Step 4: Build and run the new tests, the wiring tests and the request-profile gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
ninja -C build unit_tests_dbms > build/build_task2b.log 2>&1; echo NINJA_EXIT=$? >> build/build_task2b.log; tail -1 build/build_task2b.log
build/src/unit_tests_dbms --gtest_filter='*CASDirectoryProbes*:CASWiring*:CASNamespaceFile*' > build/test_task2b.log 2>&1; tail -3 build/test_task2b.log
```

Expected: `NINJA_EXIT=0`, `[  PASSED  ]`. If `existsDirectory(part + "/sub/")` still fails, the route's `file` already carries the slash and `dirPrefixOf` must strip a double slash instead: `file.ends_with("//") ? file.substr(0, file.size() - 1) : ...`; fix and re-run.

- [ ] **Step 5: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_cas_directory_probes.cpp
git commit -m "CAS: answer directory probes inside a resolved part from the part-folder view

\`existsDirectory\`/\`listDirectory\` on \`<table>/<part>/<file>\` used to fall
through to the table-subdirectory branch and LIST the life's \`_files/\`
prefix per probe; \`MergeTreeDataPartChecksum::checkSize\` asks it for every
checksum entry of every part at load (77k LISTs on a 1,672-part restart,
issue #2439). A resolved ref now answers from its retained view; an
unresolved one keeps the old branch. A non-projection nested directory
inside a part now reports present, its children and non-empty.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC" -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Disks/tests/gtest_cas_directory_probes.cpp
git show --stat HEAD | tail -3
```

---

### Task 3: Unresolved refs, failures and the load profile

**Files:**
- Modify: `src/Disks/tests/gtest_cas_directory_probes.cpp` (spec tests 3, 4, 4b; Review Focus 2, 3, 5)

**Interfaces:**
- Consumes: `CountingObjectStorage::failReadsContaining`, `openCountingStorage`, `publishPart`, `filesPrefixOf` from Task 2; `writeVerbatimThroughDisk`-style namespace-file writes through `tryCreateWriteBuffer` (copy the helper from `gtest_cas_namespace_file_request_profile.cpp:423-440` into this file as `writeTableFile`).

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_directory_probes.cpp`, inside the anonymous namespace a copy of the request-profile helper:

```cpp
/// One table-level (verbatim) file written through the real disk write path.
void writeTableFile(DB::ContentAddressedMetadataStorage & storage, const std::string & path, const String & bytes)
{
    auto tx = storage.createTransaction();
    auto buf = tx->tryCreateWriteBuffer(
        /*owner*/ nullptr, path, DB::DBMS_DEFAULT_BUFFER_SIZE, DB::WriteMode::Rewrite, {}, /*autocommit*/ true);
    ASSERT_TRUE(buf != nullptr);
    DB::writeString(bytes, *buf);
    buf->finalize();
}
```

and after the existing tests:

```cpp
/// An unresolved ref is not a part: the old branch answers, with today's one LIST (spec test 3,
/// Review Focus 5). Exact oracle: answer and LIST count per probe.
TEST(CASDirectoryProbes, UnresolvedRefKeepsTheTableSubdirBranchAndItsOneList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);
    /// A real part so the table has a live life and a resident ref table.
    publishPart(*storage, kTbl + "/all_1_1_0", {{"columns.txt", "cols"}});
    writeTableFile(*storage, kTbl + "/custom/sub/x", "x");
    const std::string files_prefix = filesPrefixOf(*storage, kTbl);

    os->reset();
    EXPECT_TRUE(storage->existsDirectory(kTbl + "/custom/sub"));
    EXPECT_EQ(os->listCount(files_prefix), 1u);

    os->reset();
    EXPECT_FALSE(storage->existsDirectory(kTbl + "/custom/nothere"));
    EXPECT_EQ(os->listCount(files_prefix), 1u);

    os->reset();   /// a part-shaped name with no such part (Review Focus 5)
    EXPECT_FALSE(storage->existsDirectory(kTbl + "/all_9_9_0/columns.txt"));
    EXPECT_EQ(os->listCount(files_prefix), 1u);

    os->reset();   /// non-Atomic, unresolved: the generic live-tree probe, not the `_files/` prefix
    EXPECT_FALSE(storage->existsDirectory(kNonAtomicTbl + "/all_9_9_0/f"));
    EXPECT_EQ(os->listCount(files_prefix), 0u);
    EXPECT_EQ(os->listCount(""), 1u) << "keys: " << fmt::join(os->keys(CountingObjectStorage::Kind::List, ""), ", ");
}

/// A part being written in an open transaction is not published: its ref does not resolve through
/// the storage, so the probe takes today's branch and today's answer (Review Focus 2).
TEST(CASDirectoryProbes, UnpublishedPartFallsThroughLikeToday)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);
    publishPart(*storage, kTbl + "/all_1_1_0", {{"columns.txt", "cols"}});

    auto tx = storage->createTransaction();
    auto & ca_tx = dynamic_cast<DB::ContentAddressedTransaction &>(*tx);
    const std::string staged = kTbl + "/tmp_insert_all_2_2_0";
    auto buf = ca_tx.writeFile(staged + "/sub/data.bin", 65536, DB::WriteMode::Rewrite, {});
    buf->write("d", 1);
    buf->finalize();

    const std::string files_prefix = filesPrefixOf(*storage, kTbl);
    os->reset();
    EXPECT_NO_THROW(EXPECT_FALSE(storage->existsDirectory(staged + "/sub")));
    EXPECT_EQ(os->listCount(files_prefix), 1u) << "unpublished: the old branch and its LIST, unchanged";
    tx->commit(DB::NoCommitOptions{});
}

/// Failure is not absence (spec test 4b, Review Focus 3): a resolved ref whose manifest cannot be
/// read throws; the old LIST branch is never entered on a failed request.
TEST(CASDirectoryProbes, FailedManifestReadPropagatesAndDoesNotList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/true);
    const std::string part = kTbl + "/all_1_1_0";
    publishPart(*storage, part, {{"columns.txt", "cols"}});
    const std::string files_prefix = filesPrefixOf(*storage, kTbl);

    os->failReadsContaining(storage->store()->layout().casManifestsPrefix());
    os->reset();
    EXPECT_THROW(storage->existsDirectory(part + "/columns.txt"), DB::Exception);
    EXPECT_EQ(os->listCount(files_prefix), 0u);
    EXPECT_EQ(os->listCount(""), 0u);
}

/// The load profile (spec test 4): after the one legitimate table-directory enumeration, probing
/// every file of every part adds no LIST.
TEST(CASDirectoryProbes, CheckSizeProbesOfFiftyPartsAddNoList)
{
    std::shared_ptr<CountingObjectStorage> os;
    auto storage = openCountingStorage(os, /*disable_caches=*/false);
    const std::vector<std::string> files = {"columns.txt", "checksums.txt", "count.txt", "data.bin", "data.cmrk3", "primary.cidx"};
    for (int i = 1; i <= 50; ++i)
    {
        std::vector<std::pair<std::string, std::string>> contents;
        for (const auto & f : files)
            contents.emplace_back(f, "bytes-" + std::to_string(i));
        publishPart(*storage, kTbl + "/all_" + std::to_string(i) + "_" + std::to_string(i) + "_0", contents);
    }
    const std::string files_prefix = filesPrefixOf(*storage, kTbl);

    /// The table directory enumeration a load does once (its one `_files/` LIST is legitimate).
    auto names = storage->listDirectory(kTbl);
    ASSERT_EQ(std::count_if(names.begin(), names.end(), [](const auto & n) { return n.starts_with("all_"); }), 50);
    os->reset();

    for (const auto & name : names)
        if (name.starts_with("all_"))
            for (const auto & f : files)
                EXPECT_FALSE(storage->existsDirectory(kTbl + "/" + name + "/" + f));

    EXPECT_EQ(os->listCount(files_prefix), 0u) << "keys: " << fmt::join(os->keys(CountingObjectStorage::Kind::List, "_files"), ", ");
    EXPECT_EQ(os->listCount(""), 0u);
}
```

- [ ] **Step 2: Build and run to see which assertions fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
ninja -C build unit_tests_dbms > build/build_task3a.log 2>&1; echo NINJA_EXIT=$? >> build/build_task3a.log; tail -1 build/build_task3a.log
build/src/unit_tests_dbms --gtest_filter='*CASDirectoryProbes*' > build/test_task3a.log 2>&1; grep -E "^\[  (FAILED|PASSED)|Failure" build/test_task3a.log | head -20
```

Expected: with Task 2 in place these tests PASS except where a helper assumption is wrong (a `moveDirectory` refusal, a different staged path rule, a different error class). They are regression pins, not red-first tests; if one fails, the failure is either a plan assumption (fix the test to the code's real contract and say so in the commit) or a real gap in Task 2 (fix the code). `UnpublishedPartFallsThroughLikeToday` may see `listCount(files_prefix) == 0` if the transaction overlay answers the staged path first (`DataPartStorageOnDiskFull.cpp:89-97` answers through the transaction, the storage does not); then assert `0` and keep the `EXPECT_NO_THROW`.

- [ ] **Step 3: Run the whole CAS gate**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_task3_gate.log 2>&1; tail -3 build/test_task3_gate.log
```

Expected: `[  PASSED  ]`, zero failures (the gate filter is exactly `CAS*`). Have a subagent summarize the log if it is long.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git add src/Disks/tests/gtest_cas_directory_probes.cpp
git commit -m "CAS: pin the unresolved-ref, failure and load-profile contracts of part-file probes

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC" -- src/Disks/tests/gtest_cas_directory_probes.cpp
```

---

### Task 4: Stateless test: `ATTACH` LIST count independent of the part count

**Files:**
- Create: `tests/queries/0_stateless/<n>_cas_part_file_probes_no_list.sh` and `.reference` through `add-test`

**Interfaces:**
- Consumes: the ad-hoc CAS disk declaration of `tests/queries/0_stateless/04278_cas_disk.sh:15-27`; `ProfileEvents['CASRootList']` of `system.query_log`.

- [ ] **Step 1: Create the test files**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
./tests/queries/0_stateless/add-test cas_part_file_probes_no_list.sh
ls tests/queries/0_stateless/ | grep cas_part_file_probes_no_list
```

Expected: two new files, `<n>_cas_part_file_probes_no_list.sh` and `.reference`, `<n>` being the next free number.

- [ ] **Step 2: Write the test**

Content of `tests/queries/0_stateless/<n>_cas_part_file_probes_no_list.sh`:

```bash
#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# Loading a table on a cas disk must not issue one S3 LIST per part file: checkSize asks
# existsDirectory for every checksum entry of every part, and that probe used to LIST the table's
# _files/ prefix each time. ATTACH reloads every part synchronously inside the query, so the ATTACH
# query's own ProfileEvents (unaffected by parallel tests and by background work) are the oracle:
# the LIST count of a 200-part table equals that of a 20-part table and stays below the part count.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK="disk(type = object_storage, object_storage_type = local, metadata_type = cas,
    cas_server_root_id = '${CLICKHOUSE_DATABASE}_cas95',
    name = '${CLICKHOUSE_DATABASE}_cas95_cas',
    path = '${CLICKHOUSE_DATABASE}_cas95_cas_pool/')"

for n in 20 200; do
    ${CLICKHOUSE_CLIENT} -q "DROP TABLE IF EXISTS t_$n"
    ${CLICKHOUSE_CLIENT} -q "CREATE TABLE t_$n (a UInt64) ENGINE = MergeTree ORDER BY a
        SETTINGS disk = $DISK, max_bytes_to_merge_at_max_space_in_pool = 1"
    # One row per block, one block per part: n parts from one INSERT.
    ${CLICKHOUSE_CLIENT} -q "INSERT INTO t_$n SELECT number FROM numbers($n)
        SETTINGS max_block_size = 1, min_insert_block_size_rows = 1, min_insert_block_size_bytes = 1"
    ${CLICKHOUSE_CLIENT} -q "SELECT 'parts_$n', count() FROM system.parts
        WHERE database = currentDatabase() AND table = 't_$n' AND active"
    ${CLICKHOUSE_CLIENT} -q "DETACH TABLE t_$n"
    ${CLICKHOUSE_CLIENT} --query_id "${CLICKHOUSE_DATABASE}_attach_$n" -q "ATTACH TABLE t_$n"
    ${CLICKHOUSE_CLIENT} -q "SELECT 'rows_after_attach_$n', count() FROM t_$n"
done

${CLICKHOUSE_CLIENT} -q "SYSTEM FLUSH LOGS query_log"

${CLICKHOUSE_CLIENT} -q "
WITH
    (SELECT ProfileEvents['CASRootList'] FROM system.query_log
      WHERE current_database = currentDatabase() AND type = 'QueryFinish'
        AND query_id = '${CLICKHOUSE_DATABASE}_attach_20' ORDER BY event_time DESC LIMIT 1) AS l20,
    (SELECT ProfileEvents['CASRootList'] FROM system.query_log
      WHERE current_database = currentDatabase() AND type = 'QueryFinish'
        AND query_id = '${CLICKHOUSE_DATABASE}_attach_200' ORDER BY event_time DESC LIMIT 1) AS l200
SELECT 'lists_equal', l200 = l20, 'lists_below_parts', l200 < 20"

${CLICKHOUSE_CLIENT} -q "DROP TABLE t_20"
${CLICKHOUSE_CLIENT} -q "DROP TABLE t_200"
${CLICKHOUSE_CLIENT} -q "SELECT 'dropped_ok'"
```

Content of `.reference`:

```
parts_20	20
rows_after_attach_20	20
parts_200	200
rows_after_attach_200	200
lists_equal	1	lists_below_parts	1
dropped_ok
```

- [ ] **Step 3: Run the test against a standalone server from this build, first on the pre-change binary to see it fail**

The pre-change binary is `../lane-g/build/programs/clickhouse` if it was built from `antalya-26.6` without this change (check with `git -C ../lane-g log -1 --format=%h` and `git merge-base --is-ancestor`); otherwise build `git stash`-free by checking out `altinity/antalya-26.6` in a throwaway worktree only if cheap. If no pre-change binary is at hand, skip the red run and note it in the commit message.

Start a standalone server (recipe from the memory note "Standalone server for stateless tests"):

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
D=$PWD/build/stateless_task4; mkdir -p $D/data $D/tmp $D/uf $D/caches
nohup build/programs/clickhouse server --config-file=programs/server/config.xml -- --path=$D/data/ --tmp_path=$D/tmp/ \
  --user_files_path=$D/uf/ --logger.log=$D/server.log --tcp_port=19381 --http_port=19382 --interserver_http_port=19383 \
  --mysql_port=0 --postgresql_port=0 --keeper_server.tcp_port=0 --prometheus.port=0 \
  --keeper_server.raft_configuration.server.port=19384 --filesystem_caches_path=$D/caches/ \
  --custom_cached_disks_base_directory=$D/caches/ > $D/stdout.log 2>&1 &
until build/programs/clickhouse client --port 19381 -q "SELECT 1" > /dev/null 2>&1; do sleep 1; done
build/programs/clickhouse client --port 19381 -q "CREATE DATABASE IF NOT EXISTS test"
CLICKHOUSE_PORT_TCP=19381 CLICKHOUSE_PORT_HTTP=19382 python3 tests/clickhouse-test -b $PWD/build/programs/clickhouse --no-stateful cas_part_file_probes_no_list > build/test_task4.log 2>&1; tail -5 build/test_task4.log
```

Expected on the post-change binary: `1 tests passed`. Expected on a pre-change binary: the reference line differs, `lists_equal 0` (the 200-part `ATTACH` LISTs about `200 × files per part` times). If `l20 == l200` also on the pre-change binary, the loading pool threads are not attributed to the query's thread group; then switch the oracle to `system.events` deltas taken around each `ATTACH` with the test tagged nothing new (the ad-hoc disk is private to the test, and other tests' CAS disks would still count, so prefer fixing attribution: check that `loadDataParts` runs its pool through `ThreadPoolCallbackRunner` with the current thread group) and record what was found in the commit message.

Stop the server afterwards: `kill $(pgrep -f "tcp_port=19381")`.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
N=$(ls tests/queries/0_stateless/ | grep -o '^[0-9]*_cas_part_file_probes_no_list.sh' | head -1)
git add tests/queries/0_stateless/${N} tests/queries/0_stateless/${N%.sh}.reference
git commit -m "CAS: stateless test, ATTACH LIST count independent of the part count

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC" -- tests/queries/0_stateless/${N} tests/queries/0_stateless/${N%.sh}.reference
```

---

### Task 5: Documentation, style check, backlog and PR body

**Files:**
- Modify: `docs/en/antalya/cas/architecture/read-path.md` (one sentence in the paragraph at line 12 that begins "A `CAS` read never touches a classical local-metadata path")
- Modify (on the `master` worktree, branch `cas-gc-rebuild`): backlog task CAS-95.1 through the `backlog` CLI
- Create: `/home/mfilimonov/workspace/ClickHouse/cas-95-1/build/pr_body.md` (not committed; the PR body for the user to open the PR with)

- [ ] **Step 1: Add the documentation sentence**

In `docs/en/antalya/cas/architecture/read-path.md`, at the end of the paragraph that starts at line 12, append:

```markdown
A directory probe on a path inside a part (`<table>/<part>/<file>`, which `MergeTree` issues for every checksum entry at load) is answered from the part's retained folder manifest: a plain file is not a directory, a nested directory is and lists its children. No object-store LIST is involved; only a probe whose part does not resolve falls back to the table-level file listing.
```

Run the docs checks that exist in the tree (`grep -rn "read-path.md" docs/ utils/check-style 2>/dev/null | head -3` shows none beyond the anchor rule; every header already carries its `{#anchor}`).

- [ ] **Step 2: Style check on the touched files**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
utils/check-style/check-style 2>&1 | grep -E "ContentAddressedMetadataStorage|gtest_cas_directory_probes|cas_part_file_probes" | head; echo "style-exit=$?"
git diff --name-only altinity/antalya-26.6 | grep -v "^docs/" | xargs -I{} sh -c 'grep -n "^\s*{$" {} > /dev/null || true'
```

Expected: no style findings for the touched files (Allman braces, no trailing whitespace, no `-Wswitch` gaps; the compiler already enforced the last one).

- [ ] **Step 3: Commit the docs**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git add docs/en/antalya/cas/architecture/read-path.md
git commit -m "docs(cas): part-level directory probes are answered from the part manifest

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC" -- docs/en/antalya/cas/architecture/read-path.md
```

- [ ] **Step 4: Write the PR body from the template (not committed)**

Fill `.github/PULL_REQUEST_TEMPLATE.md` into `build/pr_body.md`:

```markdown
A path inside a part on a `cas` disk (`<table>/<part>/<file>`) is now answered by `existsDirectory`, `listDirectory` and `isDirectoryEmpty` from the part's folder manifest instead of an S3 LIST of the table's `_files/` prefix. `MergeTreeDataPartChecksum::checkSize` issues that probe for every checksum entry of every part at load, so a restart of a 1,672-part stand issued 77k LISTs in three minutes, got 537 `503 Slow Down` and failed 139 uploads. The LIST count of a load is now bounded by the number of tables. A non-projection nested directory inside a resolved part now reports present, its children and non-empty (it reported absent and empty before); an unresolved ref keeps the old branch and answer.

Closes: https://github.com/Altinity/ClickHouse/issues/2439

Related: docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md (on `cas-gc-rebuild`)

### Changelog category (leave one):
- Performance Improvement

### Changelog entry (a user-readable short description of the changes that goes into CHANGELOG.md):

`cas` disk: loading a table no longer issues one object-store LIST per part file; directory probes inside a part are answered from the part manifest.

### Documentation entry for user-facing changes

- [x] Documentation is written (mandatory for new features)

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC
```

Do not push and do not open the PR: report the branch name and the body's path to the user.

- [ ] **Step 5: Update the backlog on the `master` worktree**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
backlog task edit 95.1 -s "In Progress" --append-notes "2026-09-27: implemented on branch fix/antalya-26.6/cas-part-file-probes-no-list (worktree cas-95-1), plan docs/superpowers/plans/2026-09-27-cas-part-file-probes-no-list.md; PR body in cas-95-1/build/pr_body.md; awaiting push and CI."
T=$(ls docs/superpowers/cas/backlog/tasks/ | grep "^cas-95.1 " | head -1)
git add -- "docs/superpowers/cas/backlog/tasks/$T"
git commit -m "docs(cas): CAS-95.1 in progress, branch and plan recorded

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01V8mZSGiD8iJumpJMiQnrmC" -- "docs/superpowers/cas/backlog/tasks/$T"
```

---

### Task 6: Gates on the exact tree to be pushed

**Files:** none new. Runs on the final commit of `fix/antalya-26.6/cas-part-file-probes-no-list`.

- [ ] **Step 1: Full CAS gate and the touched suites once more on HEAD**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-95-1
git status --short | grep -v "^??" ; echo "clean-tree-check-done"
ninja -C build unit_tests_dbms clickhouse > build/build_task6.log 2>&1; echo NINJA_EXIT=$? >> build/build_task6.log; tail -1 build/build_task6.log
build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_task6_gate.log 2>&1; tail -3 build/test_task6_gate.log
```

Expected: no modified tracked files, `NINJA_EXIT=0`, `[  PASSED  ]`.

- [ ] **Step 2: ASan on the touched suites**

If an ASan build directory exists for this branch line (`ls -d ../lane-g/build_asan ../cas-95-1/build_asan 2>/dev/null`), build `unit_tests_dbms` there from this worktree's sources and run `--gtest_filter='*CASDirectoryProbes*:CASWiring*:CASNamespaceFile*'` with the output in `build_asan/test_task6_asan.log`. If none exists, say so in the report: the ASan lane runs in CI after the push, and the user decides whether to wait for it before merging.

- [ ] **Step 3: Report**

The final message names: the branch and worktree, the six commits (hash and first line), which gates ran green with their log paths, the pre-change red run of the stateless test (or that no pre-change binary was available), the PR body path, and that nothing was pushed. The before/after `S3ListObjects` numbers from an otel.demo restart (spec §5) are the user's operational step after the PR is deployed, not part of this plan.
