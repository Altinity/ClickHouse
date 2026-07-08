---
description: "Implementation plan, Phase 1 of the CAS part-folder cache: vocabulary types, shared decoded manifests, decoder ordering, and pure index-free PartFolderView queries."
sidebar_label: "CAS Part Folder Cache Plan P1"
sidebar_position: 14
slug: "/superpowers/plans/2026-07-08-cas-part-folder-cache-phase1"
title: "CAS Part Folder Cache — Phase 1 Plan"
doc_type: "reference"
---

# CAS Part Folder Cache — Phase 1: Vocabulary, Shared Decodes, Pure Views {#phase-1-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** introduce `PartRefKey`, `Freshness`, and an immutable index-free `PartFolderView`; add `Store::readManifestShared` and strict decoder ordering; migrate all post-resolve manifest logic onto pure view queries — behavior-preserving, no facade, no retention.

**Architecture:** the decoder starts enforcing strict canonical path order, which makes binary search over the shared decode sound; `readManifestShared` exposes the `shared_ptr<const PartManifest>` the manifest cache already holds (killing the per-operation copy); `PartFolderView` becomes the only place that interprets a decoded manifest as a file tree. Spec: `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md` (normative).

**Tech Stack:** C++ (ClickHouse `src/`), gtest (`unit_tests_dbms` target auto-globs `src/Disks/tests/gtest_*.cpp`).

## Global Constraints {#global-constraints}

- Branch: create `cas-part-folder-cache` off the current CAS dev branch before Task 1; never commit to `master`. Add new commits only — no rebase/amend.
- C++ style: Allman braces (opening brace on its own line). Enforced by CI style check.
- Never `sleep` in C++ to order concurrent events; tests use deterministic seams only.
- Build command (always redirect to a log in the build dir; on failure, dispatch a subagent to summarize the log): `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1`
- Run gtests as: `./build_debug/src/unit_tests_dbms --gtest_filter='<Filter>' 2>&1 | tail -20`
- The functional CA suite (run at the end of the phase) via praktika, binary symlinked at `ci/tmp/clickhouse`, ONE `--test` flag with space-separated names.
- Comments/messages: wrap literal names in backticks; write function names as `f` not `f()`; say "exception", not "crash".
- Doc-file headers (if any docs touched) need `{#anchors}` and frontmatter.

---

### Task 1: Decoder strict canonical ordering {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp` (the `decodePartManifest` entry loop)
- Test: `src/Disks/tests/gtest_cas_manifest_codec.cpp`

**Interfaces:**
- Consumes: existing `encodePartManifest` / `decodePartManifest` / `computePayloadDigest`.
- Produces: the decoder invariant "decoded `PartManifest::entries` are strictly ascending by `path`" — Task 2's binary search and everything after relies on it.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_manifest_codec.cpp` (use the file's existing includes; `expectThrowsCode` comes from `cas_test_helpers.h`, already included there — add the include if absent). The forge trick: encode a valid manifest whose paths are unique equal-length strings, then byte-swap the two path strings inside the encoded body — same length keeps the `RunFile` framing intact, and `payload_digest` is integrity/debug only (never verified on decode), so only the ordering check can reject the result.

```cpp
namespace
{

DB::Cas::PartManifest makeTwoEntryManifestForOrderTest()
{
    DB::Cas::PartManifest m;
    m.ref = DB::Cas::ManifestRef{.writer_epoch = 1, .build_sequence = 2, .manifest_ordinal = 3};
    m.root_namespace_id = DB::Cas::RootNamespace{"srv/tbl"};
    DB::Cas::ManifestEntry a;
    a.path = "path_alpha_0001";
    a.placement = DB::Cas::EntryPlacement::Inline;
    a.blob_hash = DB::UInt128(1);
    a.blob_size = 1;
    a.inline_bytes = "x";
    DB::Cas::ManifestEntry b = a;
    b.path = "path_bravo_0002";
    b.blob_hash = DB::UInt128(2);
    b.inline_bytes = "y";
    m.entries = {a, b};
    m.payload_digest = DB::Cas::computePayloadDigest(m);
    return m;
}

/// Swap two equal-length unique substrings inside an encoded body.
DB::String swapPaths(DB::String encoded, const DB::String & p1, const DB::String & p2)
{
    const auto i1 = encoded.find(p1);
    const auto i2 = encoded.find(p2);
    EXPECT_NE(i1, DB::String::npos);
    EXPECT_NE(i2, DB::String::npos);
    EXPECT_EQ(p1.size(), p2.size());
    encoded.replace(i1, p1.size(), p2);
    encoded.replace(i2, p2.size(), p1);
    return encoded;
}

}

TEST(CasManifestCodec, DecodeRejectsOutOfOrderEntries)
{
    const auto m = makeTwoEntryManifestForOrderTest();
    const DB::String forged = swapPaths(
        DB::Cas::encodePartManifest(m), "path_alpha_0001", "path_bravo_0002");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { DB::Cas::decodePartManifest(forged); });
}

TEST(CasManifestCodec, DecodeRejectsNonAdjacentDuplicatePath)
{
    /// Three entries a < b < c; forging c := a yields (a, b, a) — the OLD adjacent-only duplicate
    /// check missed this shape; the ordering check must reject it.
    auto m = makeTwoEntryManifestForOrderTest();
    DB::Cas::ManifestEntry c = m.entries[0];
    c.path = "path_delta_0003";
    c.blob_hash = DB::UInt128(3);
    c.inline_bytes = "z";
    m.entries.push_back(c);
    m.payload_digest = DB::Cas::computePayloadDigest(m);
    DB::String encoded = DB::Cas::encodePartManifest(m);
    const auto pos = encoded.find("path_delta_0003");
    ASSERT_NE(pos, DB::String::npos);
    encoded.replace(pos, 15, "path_alpha_0001");
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { DB::Cas::decodePartManifest(encoded); });
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasManifestCodec.DecodeRejects*' 2>&1 | tail -20`
Expected: both new tests FAIL (`expected DB::Exception` from `expectThrowsCode` — the decoder currently accepts out-of-order bodies).

- [ ] **Step 3: Implement the ordering check**

In `Core/CasManifestCodec.cpp`, `decodePartManifest`, replace the entry loop:

```cpp
        while (r.next(path, payload))
        {
            if (have_prev && path == prev_path)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: duplicate path '{}'", path);
            /// Strict ascending canonical order (spec 2026-07-08-cas-part-folder-cache): the encoder
            /// has always written sorted entries; enforcing it here makes duplicate detection sound
            /// for non-adjacent duplicates AND establishes the ordering invariant `findEntry` /
            /// `PartFolderView` binary search rely on.
            if (have_prev && path < prev_path)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "PartManifest: entries out of canonical order ('{}' after '{}')", path, prev_path);
            m.entries.push_back(decodeEntryPayload(path, payload));
            prev_path = path;
            have_prev = true;
        }
```

- [ ] **Step 4: Run tests to verify they pass (plus the whole codec suite)**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasManifestCodec.*' 2>&1 | tail -20`
Expected: PASS, zero failures in the codec suite.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp src/Disks/tests/gtest_cas_manifest_codec.cpp
git commit -m "CAS codec: decoder enforces strict canonical entry order (part-folder cache spec, Phase 1)"
```

---

### Task 2: `findEntry` / `entryRange` ordered-lookup primitives {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h` (declarations), `Core/CasManifestCodec.cpp` (definitions)
- Test: `src/Disks/tests/gtest_cas_manifest_codec.cpp`

**Interfaces:**
- Consumes: Task 1's ordering invariant.
- Produces (used by `PartFolderView` in Task 5 and by the Core gtest migration in Phase 2):

```cpp
const ManifestEntry * findEntry(const std::vector<ManifestEntry> & entries, std::string_view path);
std::pair<const ManifestEntry *, const ManifestEntry *>
entryRange(const std::vector<ManifestEntry> & entries, std::string_view dir_prefix);
```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasManifestCodec, FindEntryBinarySearch)
{
    std::vector<DB::Cas::ManifestEntry> entries;
    for (const char * p : {"a.txt", "b/inner.txt", "b/z.txt", "c.txt"})
    {
        DB::Cas::ManifestEntry e;
        e.path = p;
        e.placement = DB::Cas::EntryPlacement::Inline;
        e.inline_bytes = "v";
        entries.push_back(e);
    }
    EXPECT_NE(DB::Cas::findEntry(entries, "a.txt"), nullptr);
    EXPECT_EQ(DB::Cas::findEntry(entries, "a.txt")->path, "a.txt");
    EXPECT_NE(DB::Cas::findEntry(entries, "c.txt"), nullptr);          /// last element
    EXPECT_EQ(DB::Cas::findEntry(entries, "b"), nullptr);              /// prefix of a path, not a path
    EXPECT_EQ(DB::Cas::findEntry(entries, "zzz"), nullptr);            /// past the end
    EXPECT_EQ(DB::Cas::findEntry({}, "a"), nullptr);                   /// empty
}

TEST(CasManifestCodec, EntryRangeContiguousPrefix)
{
    std::vector<DB::Cas::ManifestEntry> entries;
    for (const char * p : {"a.txt", "p.proj/data.bin", "p.proj/x.txt", "q.txt"})
    {
        DB::Cas::ManifestEntry e;
        e.path = p;
        e.placement = DB::Cas::EntryPlacement::Inline;
        e.inline_bytes = "v";
        entries.push_back(e);
    }
    auto [first, last] = DB::Cas::entryRange(entries, "p.proj/");
    ASSERT_EQ(last - first, 2);
    EXPECT_EQ(first->path, "p.proj/data.bin");
    EXPECT_EQ((last - 1)->path, "p.proj/x.txt");

    auto [w1, w2] = DB::Cas::entryRange(entries, "");                  /// empty prefix = whole span
    EXPECT_EQ(w2 - w1, 4);

    auto [n1, n2] = DB::Cas::entryRange(entries, "zzz/");              /// no match
    EXPECT_EQ(n1, n2);
}
```

- [ ] **Step 2: Run tests to verify they fail to compile**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `findEntry` is not a member of `DB::Cas`.

- [ ] **Step 3: Implement**

`Core/CasManifestCodec.h`, after `decodePartManifest`:

```cpp
/// Ordered-entry lookup primitives (spec 2026-07-08-cas-part-folder-cache §Shared Decodes): pure
/// functions of a DECODED manifest, whose entries the decoder guarantees strictly ascending by
/// `path`. `PartFolderView` composes these with wiring policy; Core tests use them directly.

/// Binary search. Returns nullptr when absent. The pointer aliases `entries` — do not outlive it.
const ManifestEntry * findEntry(const std::vector<ManifestEntry> & entries, std::string_view path);

/// The contiguous [first, last) sub-span of entries whose path starts with `dir_prefix` (canonical
/// order makes matches contiguous). Empty prefix = the whole span.
std::pair<const ManifestEntry *, const ManifestEntry *>
entryRange(const std::vector<ManifestEntry> & entries, std::string_view dir_prefix);
```

`Core/CasManifestCodec.cpp` (add `#include <algorithm>` if absent):

```cpp
const ManifestEntry * findEntry(const std::vector<ManifestEntry> & entries, std::string_view path)
{
    const auto it = std::lower_bound(entries.begin(), entries.end(), path,
        [](const ManifestEntry & e, std::string_view p) { return std::string_view(e.path) < p; });
    if (it == entries.end() || std::string_view(it->path) != path)
        return nullptr;
    return &*it;
}

std::pair<const ManifestEntry *, const ManifestEntry *>
entryRange(const std::vector<ManifestEntry> & entries, std::string_view dir_prefix)
{
    if (dir_prefix.empty())
        return {entries.data(), entries.data() + entries.size()};
    /// Every path starting with `dir_prefix` compares >= `dir_prefix`, and prefixed paths form a
    /// contiguous run from the first such position.
    const auto first = std::lower_bound(entries.begin(), entries.end(), dir_prefix,
        [](const ManifestEntry & e, std::string_view p) { return std::string_view(e.path) < p; });
    auto last = first;
    while (last != entries.end() && std::string_view(last->path).starts_with(dir_prefix))
        ++last;
    return {entries.data() + (first - entries.begin()), entries.data() + (last - entries.begin())};
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasManifestCodec.*' 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp src/Disks/tests/gtest_cas_manifest_codec.cpp
git commit -m "CAS codec: findEntry/entryRange ordered-lookup primitives over decoded manifests"
```

---

### Task 3: `Store::readManifestShared` {#task-3}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` (declaration next to `readManifest`), `Core/CasStore.cpp` (refactor `readManifest`)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Produces: `std::shared_ptr<const PartManifest> Store::readManifestShared(const ManifestId & id)` — identical semantics to `readManifest` (mandatory `HEAD`, fail-closed `FILE_DOESNT_EXIST` / `CORRUPTED_DATA`, `manifest_cache` insert) but returns the cached shared decode instead of copying. `readManifest` becomes a by-value shim over it.

- [ ] **Step 1: Write the failing test**

Append to `gtest_cas_store.cpp` (it already includes `cas_test_helpers.h`; helpers used: `CountingBackend`, `writeManifestRaw`, `publishCommittedTransition`, `blobEntryFor`, `openStoreForTest`-style open):

```cpp
TEST(CasStore, ReadManifestSharedReturnsSharedDecodeWithoutCopy)
{
    auto backend = std::make_shared<DB::Cas::tests::CountingBackend>();
    const DB::Cas::Layout layout("p");
    const DB::Cas::RootNamespace ns{"srv/t1"};
    const DB::Cas::ManifestRef ref{.writer_epoch = 1, .build_sequence = 1, .manifest_ordinal = 1};
    const auto id = DB::Cas::tests::writeManifestRaw(*backend, layout, ns, ref,
        {DB::Cas::tests::blobEntryFor("data.bin", DB::UInt128(7))});
    DB::Cas::tests::publishCommittedTransition(*backend, layout, ns, "part_1", std::nullopt, ref);

    auto store = DB::Cas::Store::open(backend,
        DB::Cas::PoolConfig{.pool_prefix = "p", .server_root_id = "test", .root_shards = 1});
    const auto resolved = store->resolveRef(ns, "part_1");
    ASSERT_TRUE(resolved.has_value());

    const String manifest_key = layout.manifestKey(id);
    backend->resetCounts();

    auto m1 = store->readManifestShared(resolved->manifest_id);
    auto m2 = store->readManifestShared(resolved->manifest_id);
    EXPECT_EQ(m1.get(), m2.get());                          /// the SAME shared decode, no copy
    EXPECT_EQ(backend->getCount(manifest_key), 1u);         /// one body GET
    EXPECT_EQ(backend->headCount(manifest_key), 2u);        /// mandatory HEAD per call (unchanged)
    ASSERT_EQ(m1->entries.size(), 1u);
    EXPECT_EQ(m1->entries[0].path, "data.bin");
}
```

- [ ] **Step 2: Run test to verify it fails to compile**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `readManifestShared` is not a member of `Store`.

- [ ] **Step 3: Implement**

`Core/CasStore.h`, right after the `readManifest` declaration:

```cpp
    /// Identical to `readManifest` (same mandatory HEAD, same fail-closed validation, same decode
    /// cache) but returns the SHARED immutable decode the manifest cache holds — no per-call copy.
    /// The wiring read path uses this variant (spec 2026-07-08-cas-part-folder-cache).
    std::shared_ptr<const PartManifest> readManifestShared(const ManifestId & id);
```

`Core/CasStore.cpp`: rename the existing `readManifest` body to `readManifestShared` with two changes — the cache-hit branch returns the shared pointer, and the tail returns `decoded`:

```cpp
std::shared_ptr<const PartManifest> Store::readManifestShared(const ManifestId & id)
{
    /// ... the ENTIRE existing readManifest body, unchanged, except:
    ///   cache hit:   `return *it->second;`  ->  `return it->second;`
    ///   final line:  `return *decoded;`     ->  `return decoded;`
}

PartManifest Store::readManifest(const ManifestId & id)
{
    return *readManifestShared(id);
}
```

- [ ] **Step 4: Run tests to verify they pass (whole Store suite — `readManifest` callers must be unaffected)**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasStore.*:CasBuild.*:CasProtocolScenarios.*' 2>&1 | tail -20`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "CAS store: readManifestShared returns the shared immutable decode (no per-call manifest copy)"
```

---

### Task 4: `PartRefKey` and `Freshness` {#task-4}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` (include + `Route::refKey`)
- Test: `src/Disks/tests/gtest_cas_part_folder_view.cpp` (created here; extended in Task 5)

**Interfaces:**
- Produces:

```cpp
namespace DB::ContentAddressed
{
struct PartRefKey { Cas::RootNamespace ns; String ref; bool operator==(...); String cacheKey() const; };
enum class Freshness { CachedForLoad, ForceFresh, StrictValidate };
}
/// and on the existing Route:
ContentAddressed::PartRefKey ContentAddressedMetadataStorage::Route::refKey() const;
```

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_part_folder_view.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <gtest/gtest.h>

using namespace DB;

TEST(CasPartRefKey, CacheKeyIsUnambiguous)
{
    /// Refs may contain '/' (the `detached/<part>` fold, B181); the '\0' join keeps
    /// (ns="a", ref="b/c") distinct from (ns="a/b", ref="c").
    const ContentAddressed::PartRefKey k1{Cas::RootNamespace{"a"}, "b/c"};
    const ContentAddressed::PartRefKey k2{Cas::RootNamespace{"a/b"}, "c"};
    EXPECT_NE(k1.cacheKey(), k2.cacheKey());
    EXPECT_FALSE(k1 == k2);
    EXPECT_TRUE((k1 == ContentAddressed::PartRefKey{Cas::RootNamespace{"a"}, "b/c"}));
}
```

- [ ] **Step 2: Run to verify it fails to compile**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `PartRefKey.h` does not exist.

- [ ] **Step 3: Implement**

Create `PartRefKey.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>

namespace DB::ContentAddressed
{

/// The stable identity of a committed part or projection folder (spec
/// 2026-07-08-cas-part-folder-cache §PartRefKey): owning root namespace + ref name
/// ("<part>" or "detached/<part>", B181 fold).
struct PartRefKey
{
    Cas::RootNamespace ns{""};
    String ref;

    bool operator==(const PartRefKey & o) const { return ns.string() == o.ns.string() && ref == o.ref; }

    /// Canonical map key. '\0' cannot occur in namespace strings or ref names (both derive from
    /// disk paths), so the join is unambiguous even though refs may contain '/'.
    String cacheKey() const { return ns.string() + '\0' + ref; }
};

/// Read-freshness policy at the part-folder access boundary (spec §Freshness). The
/// mutable-read-vs-write-evidence distinction is carried by the METHOD, not a fourth value:
/// mutable per-part reads call `resolve` (no manifest involved); write-path source reads call
/// `getView`, which under ForceFresh always re-proves the manifest body (mandatory HEAD in
/// `readManifestShared` — a fresh ref resolve alone proves ref currency, NOT body existence).
enum class Freshness
{
    CachedForLoad,   /// repeated load-window reads; stale-tolerant resolve (allow_stale=true)
    ForceFresh,      /// mutable per-part reads and write-path source reads; resolve fresh
    StrictValidate,  /// fsck/debug: bypass retained views entirely; fresh resolve + validated read
};

}
```

In `ContentAddressedMetadataStorage.h`: add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>` to the include block, and inside `struct Route` (after the `file` member):

```cpp
        /// The (ns, ref) identity subset — what the part-folder access layer keys on.
        ContentAddressed::PartRefKey refKey() const { return {ns, ref}; }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasPartRefKey.*' 2>&1 | tail -10`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/tests/gtest_cas_part_folder_view.cpp
git commit -m "CAS wiring: PartRefKey + Freshness vocabulary (part-folder cache spec, Phase 1)"
```

---

### Task 5: `PartFolderView` {#task-5}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h`, `.../PartFolderView.cpp`
- Test: `src/Disks/tests/gtest_cas_part_folder_view.cpp`

**Interfaces:**
- Consumes: `Cas::findEntry` / `Cas::entryRange` (Task 2), `PartRefKey` (Task 4), `Cas::PartManifest` / `Cas::ManifestId` / `Cas::Resolved`.
- Produces (Task 6 and all later phases build on these exact signatures):

```cpp
class PartFolderView
{
public:
    PartFolderView(PartRefKey key, Cas::ManifestId manifest_id, uint64_t manifest_size,
                   uint64_t published_at_ms, std::map<String, String> mutable_files,
                   std::shared_ptr<const Cas::PartManifest> manifest);
    static std::shared_ptr<const PartFolderView> make(
        PartRefKey key, const Cas::Resolved & resolved,
        std::shared_ptr<const Cas::PartManifest> manifest);

    static bool isReservedMutableName(std::string_view name);
    static std::optional<std::string> projectionDirPrefix(const std::string & file);

    const PartRefKey & refKey() const;
    const Cas::ManifestId & manifestId() const;
    uint64_t manifestSize() const;
    uint64_t publishedAtMs() const;
    const std::map<String, String> & mutableFiles() const;
    const std::shared_ptr<const Cas::PartManifest> & manifest() const;

    const Cas::ManifestEntry * findFile(const String & path) const;
    bool hasFile(const String & path) const;
    std::optional<uint64_t> fileSize(const String & path) const;
    std::optional<String> inlineBytes(const String & path) const;
    std::optional<String> mutableBytes(const String & path) const;
    std::vector<String> listChildren(const String & dir_prefix) const;
    bool hasDirectory(const String & dir_prefix) const;
    size_t estimatedBytes() const;
};
```

- [ ] **Step 1: Write the failing tests**

Append to `gtest_cas_part_folder_view.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <algorithm>

namespace
{

using namespace DB;

std::shared_ptr<const ContentAddressed::PartFolderView> makeView()
{
    auto manifest = std::make_shared<Cas::PartManifest>();
    auto add = [&](const char * path, Cas::EntryPlacement placement, const char * bytes, uint64_t blob_size)
    {
        Cas::ManifestEntry e;
        e.path = path;
        e.placement = placement;
        e.blob_hash = UInt128(manifest->entries.size() + 1);
        e.blob_size = blob_size;
        e.inline_bytes = bytes;
        manifest->entries.push_back(e);
    };
    /// Canonical (sorted) order — the ctor chasserts it.
    add("checksums.txt", Cas::EntryPlacement::Inline, "cs", 2);
    add("data.bin", Cas::EntryPlacement::Blob, "", 100);
    add("p.proj/checksums.txt", Cas::EntryPlacement::Inline, "pc", 2);
    add("p.proj/data.bin", Cas::EntryPlacement::Blob, "", 50);

    std::map<String, String> mutables{{"txn_version.txt", "ver"}, {".ca_hidden", "x"}};
    return std::make_shared<const ContentAddressed::PartFolderView>(
        ContentAddressed::PartRefKey{Cas::RootNamespace{"srv/t"}, "part_1"},
        Cas::ManifestId{Cas::RootNamespace{"srv/t"}, Cas::ManifestRef{1, 2, 3}},
        /*manifest_size=*/1000, /*published_at_ms=*/42, std::move(mutables), manifest);
}

std::vector<String> sorted(std::vector<String> v) { std::sort(v.begin(), v.end()); return v; }

}

TEST(CasPartFolderView, FindFileAndHasFile)
{
    auto v = makeView();
    ASSERT_NE(v->findFile("data.bin"), nullptr);
    EXPECT_EQ(v->findFile("data.bin")->blob_size, 100u);
    EXPECT_EQ(v->findFile("absent.bin"), nullptr);
    EXPECT_TRUE(v->hasFile("p.proj/data.bin"));
    EXPECT_TRUE(v->hasFile("txn_version.txt"));      /// non-reserved mutable counts
    EXPECT_FALSE(v->hasFile(".ca_hidden"));          /// reserved mutable is invisible
    EXPECT_FALSE(v->hasFile("p.proj"));              /// a directory, not a file
}

TEST(CasPartFolderView, ListChildrenCollapsesFirstComponent)
{
    auto v = makeView();
    EXPECT_EQ(sorted(v->listChildren("")),
              sorted({"checksums.txt", "data.bin", "p.proj", "txn_version.txt"}));
    EXPECT_EQ(sorted(v->listChildren("p.proj/")), sorted({"checksums.txt", "data.bin"}));
    EXPECT_TRUE(v->listChildren("q.proj/").empty());
}

TEST(CasPartFolderView, HasDirectory)
{
    auto v = makeView();
    EXPECT_TRUE(v->hasDirectory("p.proj/"));
    EXPECT_FALSE(v->hasDirectory("q.proj/"));
}

TEST(CasPartFolderView, SizesAndBytes)
{
    auto v = makeView();
    EXPECT_EQ(v->fileSize("checksums.txt"), std::optional<uint64_t>(2));   /// inline: bytes size
    EXPECT_EQ(v->fileSize("data.bin"), std::optional<uint64_t>(100));      /// blob: blob_size
    EXPECT_EQ(v->fileSize("txn_version.txt"), std::optional<uint64_t>(3)); /// mutable: value size
    EXPECT_EQ(v->fileSize("absent"), std::nullopt);
    EXPECT_EQ(v->inlineBytes("checksums.txt"), std::optional<String>("cs"));
    EXPECT_EQ(v->inlineBytes("data.bin"), std::nullopt);                   /// blob has no inline bytes
    EXPECT_EQ(v->mutableBytes("txn_version.txt"), std::optional<String>("ver"));
    EXPECT_EQ(v->mutableBytes(".ca_hidden"), std::nullopt);                /// reserved filtered
    EXPECT_GE(v->estimatedBytes(), 1000u);                                 /// >= manifest_size
}

TEST(CasPartFolderView, ProjectionDirPrefixRecognizer)
{
    using V = ContentAddressed::PartFolderView;
    EXPECT_EQ(V::projectionDirPrefix("p.proj"), std::optional<std::string>("p.proj/"));
    EXPECT_EQ(V::projectionDirPrefix("a/b.tmp_proj"), std::optional<std::string>("a/b.tmp_proj/"));
    EXPECT_EQ(V::projectionDirPrefix("data.bin"), std::nullopt);
    EXPECT_EQ(V::projectionDirPrefix(""), std::nullopt);
}
```

- [ ] **Step 2: Run to verify compile failure**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1; tail -5 build_debug/build_unit_tests.log`
Expected: compile error — `PartFolderView.h` does not exist.

- [ ] **Step 3: Implement**

Create `PartFolderView.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DB::ContentAddressed
{

/// Immutable snapshot of one resolved committed part/projection folder (spec
/// 2026-07-08-cas-part-folder-cache §PartFolderView). Index-free: the decoder guarantees strictly
/// ascending canonical path order, so file lookup is a binary search and directory listing is a
/// contiguous range scan over the SHARED decode (`manifest` is the same object the Store's
/// manifest cache holds). No I/O; never mutated after construction — a mutable-files refresh
/// builds a NEW view sharing the same manifest pointer. All answers are pure functions of the
/// members; wiring-reserved `.ca_*` mutable names are filtered here (folder-view semantics).
class PartFolderView
{
public:
    PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                   uint64_t published_at_ms_, std::map<String, String> mutable_files_,
                   std::shared_ptr<const Cas::PartManifest> manifest_);

    /// Convenience join of a fresh `Resolved` + its validated shared decode.
    static std::shared_ptr<const PartFolderView> make(
        PartRefKey key, const Cas::Resolved & resolved,
        std::shared_ptr<const Cas::PartManifest> manifest);

    /// Wiring-reserved RefPayload.mutable_files keys (dot-prefixed `.ca_*`) — never user-visible.
    static bool isReservedMutableName(std::string_view name) { return name.starts_with(".ca_"); }

    /// A projection DIRECTORY is recognized by its LAST path component (.proj / .tmp_proj) — the
    /// PoC recognizer (B64, also matches the nested detached-staging shape). `file` is the ROUTED
    /// in-tree file path. Returns the "<file>/" prefix, or nullopt when not a projection dir.
    static std::optional<std::string> projectionDirPrefix(const std::string & file);

    const PartRefKey & refKey() const { return key; }
    const Cas::ManifestId & manifestId() const { return manifest_id; }
    uint64_t manifestSize() const { return manifest_size; }
    uint64_t publishedAtMs() const { return published_at_ms; }
    const std::map<String, String> & mutableFiles() const { return mutable_files; }
    const std::shared_ptr<const Cas::PartManifest> & manifest() const { return manifest_body; }

    const Cas::ManifestEntry * findFile(const String & path) const;
    bool hasFile(const String & path) const;                      /// entry OR non-reserved mutable
    std::optional<uint64_t> fileSize(const String & path) const;  /// mutable / inline / blob
    std::optional<String> inlineBytes(const String & path) const; /// Inline entries only
    std::optional<String> mutableBytes(const String & path) const;
    std::vector<String> listChildren(const String & dir_prefix) const;
    bool hasDirectory(const String & dir_prefix) const;
    size_t estimatedBytes() const;

private:
    PartRefKey key;
    Cas::ManifestId manifest_id;
    uint64_t manifest_size = 0;
    uint64_t published_at_ms = 0;
    std::map<String, String> mutable_files;
    std::shared_ptr<const Cas::PartManifest> manifest_body;
};

}
```

Create `PartFolderView.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h>
#include <base/defines.h>
#include <algorithm>
#include <unordered_set>

namespace DB::ContentAddressed
{

PartFolderView::PartFolderView(PartRefKey key_, Cas::ManifestId manifest_id_, uint64_t manifest_size_,
                               uint64_t published_at_ms_, std::map<String, String> mutable_files_,
                               std::shared_ptr<const Cas::PartManifest> manifest_)
    : key(std::move(key_))
    , manifest_id(std::move(manifest_id_))
    , manifest_size(manifest_size_)
    , published_at_ms(published_at_ms_)
    , mutable_files(std::move(mutable_files_))
    , manifest_body(std::move(manifest_))
{
    chassert(manifest_body);
    /// The binary-search contract: decodePartManifest enforces this for every decoded body; a
    /// hand-constructed manifest (tests) must honor it too.
    chassert(std::is_sorted(manifest_body->entries.begin(), manifest_body->entries.end(),
        [](const Cas::ManifestEntry & a, const Cas::ManifestEntry & b) { return a.path < b.path; }));
}

std::shared_ptr<const PartFolderView> PartFolderView::make(
    PartRefKey key, const Cas::Resolved & resolved, std::shared_ptr<const Cas::PartManifest> manifest)
{
    return std::make_shared<const PartFolderView>(
        std::move(key), resolved.manifest_id, resolved.manifest_size,
        resolved.published_at_ms, resolved.mutable_files, std::move(manifest));
}

std::optional<std::string> PartFolderView::projectionDirPrefix(const std::string & file)
{
    if (file.empty())
        return std::nullopt;
    const auto last_slash = file.find_last_of('/');
    const std::string_view last_component
        = last_slash == std::string::npos ? std::string_view(file) : std::string_view(file).substr(last_slash + 1);
    if (last_component.ends_with(".proj") || last_component.ends_with(".tmp_proj"))
        return file + "/";
    return std::nullopt;
}

const Cas::ManifestEntry * PartFolderView::findFile(const String & path) const
{
    return Cas::findEntry(manifest_body->entries, path);
}

bool PartFolderView::hasFile(const String & path) const
{
    if (findFile(path))
        return true;
    return !isReservedMutableName(path) && mutable_files.contains(path);
}

std::optional<uint64_t> PartFolderView::fileSize(const String & path) const
{
    if (auto mb = mutableBytes(path))
        return mb->size();
    if (const auto * e = findFile(path))
        return e->placement == Cas::EntryPlacement::Inline ? e->inline_bytes.size() : e->blob_size;
    return std::nullopt;
}

std::optional<String> PartFolderView::inlineBytes(const String & path) const
{
    const auto * e = findFile(path);
    if (e && e->placement == Cas::EntryPlacement::Inline)
        return e->inline_bytes;
    return std::nullopt;
}

std::optional<String> PartFolderView::mutableBytes(const String & path) const
{
    if (isReservedMutableName(path))
        return std::nullopt;
    const auto it = mutable_files.find(path);
    if (it == mutable_files.end())
        return std::nullopt;
    return it->second;
}

std::vector<String> PartFolderView::listChildren(const String & dir_prefix) const
{
    /// First-component collapse over entries + non-reserved mutables. NOTE (deliberate, safe
    /// normalization vs the pre-view code): projection-dir listings are collapsed too; projection
    /// contents are flat in every real layout, and collapsing is the correct directory semantic
    /// for a hypothetical nested one.
    std::unordered_set<String> names;
    auto add = [&](const String & full)
    {
        if (!full.starts_with(dir_prefix) || full.size() <= dir_prefix.size())
            return;
        const std::string_view rest = std::string_view(full).substr(dir_prefix.size());
        const auto slash = rest.find('/');
        names.emplace(slash == std::string_view::npos ? rest : rest.substr(0, slash));
    };
    const auto [first, last] = Cas::entryRange(manifest_body->entries, dir_prefix);
    for (const auto * e = first; e != last; ++e)
        add(e->path);
    for (const auto & [file, _] : mutable_files)
        if (!isReservedMutableName(file))
            add(file);
    return {std::make_move_iterator(names.begin()), std::make_move_iterator(names.end())};
}

bool PartFolderView::hasDirectory(const String & dir_prefix) const
{
    const auto [first, last] = Cas::entryRange(manifest_body->entries, dir_prefix);
    if (first != last)
        return true;
    for (const auto & [file, _] : mutable_files)
        if (file.starts_with(dir_prefix))
            return true;
    return false;
}

size_t PartFolderView::estimatedBytes() const
{
    /// Conservative cache weight (spec §Memory Bound): fixed overhead + mutable payload +
    /// manifest_size (deliberately over-counts the shared decode — safe direction; Phase 5 notes).
    size_t bytes = 256;
    for (const auto & [k, v] : mutable_files)
        bytes += k.size() + v.size() + 64;
    return bytes + manifest_size;
}

}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='CasPartFolderView.*:CasPartRefKey.*' 2>&1 | tail -15`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.cpp src/Disks/tests/gtest_cas_part_folder_view.cpp
git commit -m "CAS wiring: immutable index-free PartFolderView over the shared manifest decode"
```

---

### Task 6: Rewire the read paths onto `PartFolderView` {#task-6}

**Files:**
- Modify: `.../ContentAddressed/ContentAddressedMetadataStorage.h` (`resolveRouted` signature), `.../ContentAddressedMetadataStorage.cpp` (all `resolveRouted` consumers; delete the anonymous-namespace `projectionDirPrefix` and `isReservedMutableName`), `.../ContentAddressedTransaction.cpp` (`createHardLink` committed-source branch, `republishRef` manifest reads)

**Interfaces:**
- Consumes: `PartFolderView` (Task 5), `readManifestShared` (Task 3).
- Produces: `resolveRouted` now returns `std::shared_ptr<const ContentAddressed::PartFolderView>` (nullptr = absent ref). Behavior-preserving; deleted in Phase 2 when the facade takes over.

No new test file — the existing gtest + functional suites are the (regression) tests; they must pass unchanged. Steps:

- [ ] **Step 1: Change `resolveRouted`**

Header declaration becomes:

```cpp
    /// resolveRef + readManifestShared for a routed path, joined into an immutable view; nullptr
    /// when the ref is absent. Throws on a present-but-corrupt manifest (fail closed, INV-NO-DANGLE
    /// surfaced). Phase-1 shape: built per call; the Phase-2 facade replaces this method.
    std::shared_ptr<const ContentAddressed::PartFolderView> resolveRouted(const Route & r) const;
```

Implementation (add `#include <...ContentAddressed/PartFolderView.h>` to the cpp):

```cpp
std::shared_ptr<const ContentAddressed::PartFolderView>
ContentAddressedMetadataStorage::resolveRouted(const Route & r) const
{
    auto resolved = store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true);
    if (!resolved)
        return nullptr;
    /// A live ref to a missing/corrupt manifest throws (INV-NO-DANGLE surfaced, never substituted).
    return ContentAddressed::PartFolderView::make(
        r.refKey(), *resolved, store()->readManifestShared(resolved->manifest_id));
}
```

- [ ] **Step 2: Rewrite every consumer over view queries**

Delete the anonymous-namespace `isReservedMutableName` and `projectionDirPrefix` in `ContentAddressedMetadataStorage.cpp`; qualify all their uses as `ContentAddressed::PartFolderView::isReservedMutableName` / `::projectionDirPrefix`. Keep `addFirstComponent` / `toVector` / `canonicalDiskPath` / `splitFirstComponent` (still used by ref/namespace-listing branches). New bodies, one per consumer:

`existsFile` — only the tail after the mutable branch changes:

```cpp
    auto view = resolveRouted(*r);
    return view && view->findFile(r->file);
```

`existsDirectory` — only the projection branch changes:

```cpp
        if (r && !r->ref.empty())
        {
            if (auto prefix = ContentAddressed::PartFolderView::projectionDirPrefix(r->file))
            {
                auto view = resolveRouted(*r);
                return view && view->hasDirectory(*prefix);
            }
        }
```

`getFileSize` — only the tail after `resolveRouted` changes (the double resolve via `tryGetInManifestBytes` stays until Phase 2):

```cpp
    auto view = resolveRouted(*r);
    if (!view)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    if (auto size = view->fileSize(r->file))
        return *size;
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", r->file, path);
```

`listDirectory` — three branches. Shadow part dir:

```cpp
            auto view = resolveRouted(Route{shadowNamespace(p->shadow_table_dir), p->part_name, ""});
            return view ? view->listChildren("") : std::vector<std::string>{};
```

Live/detached part dir (`r->file.empty()` branch):

```cpp
            auto view = resolveRouted(*r);
            return view ? view->listChildren("") : std::vector<std::string>{};
```

Projection dir:

```cpp
            if (auto prefix = ContentAddressed::PartFolderView::projectionDirPrefix(r->file))
            {
                auto view = resolveRouted(*r);
                return view ? view->listChildren(*prefix) : std::vector<std::string>{};
            }
```

`isDirectoryEmpty` — replace its `projectionDirPrefix(r->file)` call with the view-static variant (no other change).

`getStorageObjects` — the tail:

```cpp
    auto view = resolveRouted(*r);
    if (!view)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no ref for {}", path);
    if (const auto * entry = view->findFile(r->file))
    {
        const auto location = store()->locate(*entry);
        /// StoredObject carries no range (the recorded upstream delta) — the PAYLOAD length is the
        /// size (what every size consumer wants); the header offset is applied by
        /// getBlobViewPlan's view window, the only byte-reading path.
        return {StoredObject(location.key, path, location.length)};
    }
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: file {} not in manifest of {}", r->file, path);
```

`tryGetInManifestBytes` — the inline tail:

```cpp
    auto view = resolveRouted(*r);
    if (!view)
        return std::nullopt;
    return view->inlineBytes(r->file);
```

(The mutable branch above it keeps its force-fresh `resolveRef` and swaps `isReservedMutableName` for the view-static.)

`getBlobViewPlan` — the tail:

```cpp
    auto view = resolveRouted(*r);
    if (!view)
        return std::nullopt;
    if (const auto * entry = view->findFile(r->file))
    {
        const auto location = store()->locate(*entry);
        BlobViewPlan plan;
        /// bytes_size is the readable extent of THIS file's window, NOT the whole blob (see the
        /// original comment — shared-blob cache keying on the physical key is unchanged).
        plan.object = StoredObject(physicalKey(location.key), path, location.offset + location.length);
        plan.payload_offset = location.offset;
        plan.payload_end = location.offset + location.length;
        return plan;
    }
    return std::nullopt;
```

`getPartManifestBytes` — replace the two last lines with the shared decode (no copy):

```cpp
    return Cas::encodePartManifest(*store()->readManifestShared(resolved->manifest_id));
```

`ContentAddressedTransaction.cpp` — `createHardLink` committed-source tail:

```cpp
    const auto src_manifest = metadata_storage.store()->readManifestShared(resolved->manifest_id);
    const auto * src_entry = Cas::findEntry(src_manifest->entries, src->file);
    if (!src_entry)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "ContentAddressed: createHardLink source file missing in manifest: {}", path_from);
    buildFor(*dst, dst_st).adoptEvidence(*src_entry);
    entry = *src_entry;
```

`ContentAddressedTransaction.cpp` — `republishRef`: change both by-value manifest reads to shared:

```cpp
    const auto src_manifest = metadata_storage.store()->readManifestShared(resolved->manifest_id);
    /// ... and in the idempotent branch:
    const auto dst_manifest = metadata_storage.store()->readManifestShared(dst_resolved->manifest_id);
    if (dst_manifest->entries != src_manifest->entries)
```

(and the later uses become `src_manifest->entries`).

- [ ] **Step 3: Build and run the full CAS gtest batch**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_unit_tests.log 2>&1 && ./build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -10`
Expected: PASS, zero failures.

- [ ] **Step 4: Run the CA functional suite (behavior preservation gate)**

From the repository root, with the freshly built server binary symlinked at `ci/tmp/clickhouse` (see `docs`/memory conventions), redirect to a log and summarize via a subagent:

```bash
python3 -m ci.praktika run "stateless" --test "04278_content_addressed_disk 04279_content_addressed_gc 04280_content_addressed_clone_partition_works 04282_content_addressed_mutable_state 04283_content_addressed_replicated_rejected 04284_content_addressed_backup_pointer_holding 04285_content_addressed_dedup_window_inline_disk 04286_content_addressed_remote_data_paths 04287_content_addressed_detach_partition_listing 04288_content_addressed_detached_part_modification_time 04289_content_addressed_multi_detach_drop 04290_content_addressed_no_leftovers 04292_content_addressed_mutations 04293_content_addressed_lightweight_delete 04294_content_addressed_patch_parts 04295_content_addressed_mutation_no_leftovers 04299_content_addressed_projection_inline_disk 04300_content_addressed_projection_multiblock 05000_content_addressed_projection_carry_forward 05001_content_addressed_attach_partition_projection 05002_content_addressed_fetch_partition 05003_content_addressed_freeze 05004_content_addressed_transactions 05005_content_addressed_backup_restore 05006_content_addressed_dedup_blob_insert 05007_content_addressed_gc_introspection 05009_content_addressed_event_log" > build_debug/test_phase1_stateless.log 2>&1
```

Expected: all listed tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "CAS wiring: read paths answer from PartFolderView over the shared decode (Phase 1 complete)"
```

---

## Phase acceptance {#phase-acceptance}

Same behavior (functional suite green); no per-operation manifest copies (`readManifestShared` everywhere on the wiring read path); O(log n) lookups; the codec rejects out-of-order bodies; `Cas*` gtests green.
