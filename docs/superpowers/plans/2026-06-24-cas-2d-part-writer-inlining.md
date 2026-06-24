# CAS Part-Writer Eager-File Inlining — Implementation Plan (Plan 2d)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (or executing-plans). Checkbox steps.

**Goal:** Stop staging every part file as a standalone `Blob`. Small eager part-load files (`checksums.txt`, `columns.txt`, `count.txt`, `serialization.json`, `metadata_version.txt`, `partition.dat`, `minmax_*.idx`, codec/ttl, …) are staged **`Inline`** (their bytes ride the single tree object), so opening a part takes ≈1 GET instead of N. Per-column lazy data (`.bin`, `.mrk*`) and the potentially-large `primary.idx` stay `Blob` to preserve per-column fetch selectivity and bound memory. This delivers B10/B97.

**Architecture:** A transaction-layer change in `ContentAddressedTransaction::writeFile` (the disk seam that creates a write buffer per part file). Files that must stay blobs route to the existing `CaContentWriteBuffer` (streaming spill+hash → `Blob` staging). All other part files route to the in-memory `CaInlineWriteBuffer`; its finalize callback stages **`Inline`** when the file is ≤ `INLINE_CAP`, else falls back to blob staging from memory (a safety net for an unexpectedly large file). The existing blob-staging lambda body is refactored into a reusable member so both paths share it. `Inline` entries carry `file_hash` = the content hash, so the Merkle `treeId` (Plan 2a) treats an inline file and a standalone blob of the same content identically — placement never moves an id. Reads already handle `Inline` (`ContentAddressedTransaction.cpp:400`, `CasStore`), so the read path is unchanged.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`, incl. `gtest_ca_transaction.cpp`), `CityHash_v1_0_2::CityHash128`, ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only, no amend/rebase).

**Scope guards:** Changes ONLY `ContentAddressedTransaction::writeFile` + a small placement helper + tests. Does NOT change the tree codec (2c, done), the envelope (2b), `merkleTreeId` (2a), or `CasBuild` closure (2e). This is a **behavior change** (which objects a part-write creates), not a format-freeze — the `Inline` placement format is already frozen by 2c. Out of scope: B92, Part IV, B164b/B147, and threshold-based `primary.idx` inlining (a noted follow-up — `primary.idx` stays `Blob` here).

**Build & test:**
- Build: `cd build && ninja unit_tests_dbms > cas_2d_build.log 2>&1` — no `-j`/`nproc`.
- Tests: `build/src/unit_tests_dbms --gtest_filter='CaTransaction*:CaInlinePlacement*' > build/cas_2d_test.log 2>&1`.
- Full sweep (final): `--gtest_filter='Cas*:Ca*' > build/cas_2d_sweep.log 2>&1`. Only tolerated red: baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

---

### Task 1: The placement predicate + constant

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Test: `src/Disks/tests/gtest_cas_inline_placement.cpp` (new)

The predicate is pure and the easiest thing to pin first.

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_inline_placement.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h>

using DB::ContentAddressed::partFileMustStayBlob;

TEST(CaInlinePlacement, ColumnAndMarkFilesStayBlob)
{
    EXPECT_TRUE(partFileMustStayBlob("data.bin"));
    EXPECT_TRUE(partFileMustStayBlob("data.mrk"));
    EXPECT_TRUE(partFileMustStayBlob("data.mrk2"));
    EXPECT_TRUE(partFileMustStayBlob("data.mrk3"));
    EXPECT_TRUE(partFileMustStayBlob("data.cmrk3"));
    EXPECT_TRUE(partFileMustStayBlob("primary.idx"));   // potentially large; stays blob (follow-up tuning)
}

TEST(CaInlinePlacement, EagerMetadataFilesAreInlineCandidates)
{
    EXPECT_FALSE(partFileMustStayBlob("checksums.txt"));
    EXPECT_FALSE(partFileMustStayBlob("columns.txt"));
    EXPECT_FALSE(partFileMustStayBlob("count.txt"));
    EXPECT_FALSE(partFileMustStayBlob("serialization.json"));
    EXPECT_FALSE(partFileMustStayBlob("metadata_version.txt"));
    EXPECT_FALSE(partFileMustStayBlob("partition.dat"));
    EXPECT_FALSE(partFileMustStayBlob("minmax_date.idx"));
    EXPECT_FALSE(partFileMustStayBlob("default_compression_codec.txt"));
}
```

- [ ] **Step 2: Run — verify it fails**

Run: `cd build && ninja unit_tests_dbms > cas_2d_build.log 2>&1; tail -15 cas_2d_build.log`
Expected: build FAILS — `partFileMustStayBlob` not declared.

- [ ] **Step 3: Declare + implement the predicate**

In `ContentAddressedTransaction.h`, in namespace `DB::ContentAddressed`, declare:

```cpp
/// Part files that must NOT be inlined into the tree: per-column data (`.bin`) and marks (`.mrk*`/
/// `.cmrk*`) — inlining them would force a full-part fetch and destroy column-read selectivity — plus
/// `primary.idx`, which can be large (a size-threshold inlining of small primary.idx is a follow-up).
/// Everything else (the small eager metadata files) is an inline candidate, subject to INLINE_CAP.
bool partFileMustStayBlob(std::string_view file_name);
```

In `ContentAddressedTransaction.cpp` (near the top, after includes), implement it:

```cpp
namespace DB::ContentAddressed
{

namespace
{

bool hasSuffix(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

}

bool partFileMustStayBlob(std::string_view file_name)
{
    if (file_name == "primary.idx")
        return true;
    for (std::string_view suffix : {".bin", ".mrk", ".mrk2", ".mrk3", ".cmrk", ".cmrk2", ".cmrk3"})
        if (hasSuffix(file_name, suffix))
            return true;
    return false;
}
```

(If the file already opens `namespace DB::ContentAddressed { ... }`, place `partFileMustStayBlob` inside it rather than re-opening; adjust accordingly. `<string_view>` is already transitively included; add it if the build complains.)

- [ ] **Step 4: Run — verify pass**

Run: `cd build && ninja unit_tests_dbms > cas_2d_build.log 2>&1; tail -3 cas_2d_build.log && ./src/unit_tests_dbms --gtest_filter='CaInlinePlacement.*' > cas_2d_test.log 2>&1; grep -E 'PASSED|FAILED' cas_2d_test.log | tail -3`
Expected: build clean; `[  PASSED  ] 2 tests`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_cas_inline_placement.cpp
git commit -m "CA: add partFileMustStayBlob predicate (.bin/.mrk*/primary.idx)

The set of part files that must NOT be inlined into the tree (per-column data +
marks, to preserve column-read selectivity; primary.idx, potentially large).
Everything else becomes an inline candidate. Pure predicate, unit-tested; wired
into the part-writer in the next commit.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Wire inlining into `writeFile` (refactor blob staging + inline path)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (`writeFile` part-file branch, ~542-567)

Current code (the part-file branch) returns a `CaContentWriteBuffer` whose finalize callback stages a `Blob` (records `pending_blobs`, `recordPendingBlobDep`, pushes a `Blob` `TreeEntry`). Refactor that body into a reusable member, then route inline candidates to `CaInlineWriteBuffer`.

- [ ] **Step 1: Refactor the blob-staging body into a member helper**

Add a private member to `ContentAddressedTransaction` (declare in `.h`):

```cpp
    /// Stage a CONTENT part file as a blob: record the pending upload + a tokenless dep (so stageTree's
    /// W-TREE-BUILD passes) and add/replace its Blob TreeEntry. Shared by the streaming-blob path and
    /// the inline-cap fallback.
    void stageBlobPartFile(const ContentAddressedMetadataStorage::Route & route,
                           const UInt128 & hash, size_t size, const std::string & temp_path);
```

Define it in `.cpp` with the body currently inside the `CaContentWriteBuffer` callback:

```cpp
void ContentAddressedTransaction::stageBlobPartFile(
    const ContentAddressedMetadataStorage::Route & route,
    const UInt128 & hash, size_t size, const std::string & temp_path)
{
    auto & st = stagingFor(route);
    st.pending_blobs.push_back({hash, temp_path, size});
    buildFor(route, st).recordPendingBlobDep(hash, size);

    Cas::TreeEntry entry;
    entry.name = route.file;
    entry.placement = Cas::Placement::Blob;
    entry.file_hash = hash;
    entry.file_size = size;
    std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
    st.entries.push_back(std::move(entry));
}
```

- [ ] **Step 2: Route inline candidates; keep blob files on the existing path**

In `writeFile`, replace the final `return std::make_unique<ContentAddressed::CaContentWriteBuffer>(...)` block (the part-file content path) with a branch. Blob files keep the streaming buffer (now delegating to the helper); inline candidates use `CaInlineWriteBuffer` with a cap decision:

```cpp
    /// Files that must stay blobs (per-column data/marks, primary.idx): stream+spill+hash as before.
    if (ContentAddressed::partFileMustStayBlob(r->file))
    {
        return std::make_unique<ContentAddressed::CaContentWriteBuffer>(
            metadata_storage.scratchPath(), buf_size, settings.use_adaptive_write_buffer,
            settings.adaptive_write_buffer_initial_size,
            [this, route = *r](const std::string & hash_hex, size_t size, const std::string & temp_path)
            {
                stageBlobPartFile(route, Cas::hexToU128(hash_hex), size, temp_path);
            });
    }

    /// Inline candidate (small eager metadata): buffer in memory, decide at finalize.
    return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
        [this, route = *r](std::string bytes)
        {
            const UInt128 hash = Cas::hexToU128(
                getHexUIntLowercase(CityHash_v1_0_2::CityHash128(bytes.data(), bytes.size())));
            if (bytes.size() <= INLINE_CAP)
            {
                auto & st = stagingFor(route);
                Cas::TreeEntry entry;
                entry.name = route.file;
                entry.placement = Cas::Placement::Inline;
                entry.file_hash = hash;            /// content hash — inline == blob for the Merkle id
                entry.file_size = bytes.size();
                entry.inline_bytes = std::move(bytes);
                std::erase_if(st.entries, [&](const Cas::TreeEntry & e) { return e.name == entry.name; });
                st.entries.push_back(std::move(entry));
            }
            else
            {
                /// Safety fallback: an unexpectedly large candidate spills to a blob (preserves the
                /// invariant that big files are not held inline). Write the buffered bytes to a temp
                /// file, then stage exactly like a streaming blob.
                const std::string temp_path = metadata_storage.scratchPath() + "/inline_overflow_" + hash_hex_unused(hash);
                {
                    WriteBufferFromFile tmp(temp_path);
                    tmp.write(bytes.data(), bytes.size());
                    tmp.finalize();
                }
                stageBlobPartFile(route, hash, bytes.size(), temp_path);
            }
        });
```

Notes for the implementer:
- Add `INLINE_CAP` as a file-scope `constexpr size_t INLINE_CAP = 1024 * 1024;` (1 MiB) near the top, with a comment that it is a tuning knob (could become a disk setting later).
- Replace the `hash_hex_unused(hash)` placeholder above with a real unique temp name — reuse whatever unique-temp-name scheme `CaContentWriteBuffer`/`scratchPath` already use (e.g. `u128ToHex(hash)` + a counter, or the existing temp-naming helper). The exact scheme is discovered from the surrounding code; the requirement is a unique path under `scratchPath()`.
- Includes: `CityHash_v1_0_2::CityHash128` needs `<city.h>`; `getHexUIntLowercase` needs `<base/hex.h>`; `WriteBufferFromFile` needs `<IO/WriteBufferFromFile.h>` — add any not already present.
- The blob-files branch must reproduce the EXACT current behavior (same `CaContentWriteBuffer` construction args, same staging) — only the callback body moved into `stageBlobPartFile`. Verify byte-for-byte behavior equivalence for `.bin`/`.mrk` parts (the full sweep + the existing transaction tests cover this).

- [ ] **Step 3: Build**

Run: `cd build && ninja unit_tests_dbms > cas_2d_build.log 2>&1; tail -8 cas_2d_build.log`
Expected: clean. Fix any include/temp-name issues minimally.

- [ ] **Step 4: Integration test — eager file inlines, .bin stays blob**

Add to `src/Disks/tests/gtest_ca_transaction.cpp` (reusing its `openTxStorage`/`writeFileTx` helpers; consult the file for the exact store/resolve/readTree accessor — `storage->store()` exposes `resolveRef`/`readTree`, and the namespace/ref helpers in `cas_test_helpers.h`):

```cpp
TEST(CaTransactionInlining, EagerFileInlinedDataBinBlobbed)
{
    auto storage = openTxStorage();
    auto tx = storage->createTransaction();
    writeFileTx(*tx, "uui/uuid-9/tmp_insert_all_1_1_0/checksums.txt", "the-checksums");
    writeFileTx(*tx, "uui/uuid-9/tmp_insert_all_1_1_0/data.bin", std::string(50000, 'D'));
    tx->moveDirectory("uui/uuid-9/tmp_insert_all_1_1_0", "uui/uuid-9/all_1_1_0");
    tx->commit(DB::NoCommitOptions{});

    /// Resolve the published part to its tree and inspect placements.
    const auto entries = /* resolve "uui/uuid-9/all_1_1_0" -> tree_id -> store()->readTree(tree_id) */;
    const auto * checksums = findByName(entries, "checksums.txt");
    const auto * databin   = findByName(entries, "data.bin");
    ASSERT_TRUE(checksums && databin);
    EXPECT_EQ(checksums->placement, DB::Cas::Placement::Inline);
    EXPECT_EQ(checksums->inline_bytes, "the-checksums");
    EXPECT_EQ(databin->placement, DB::Cas::Placement::Blob);

    /// And the inlined file is still readable through the normal read path.
    EXPECT_EQ(storage->getFileSize("uui/uuid-9/all_1_1_0/checksums.txt"), 13u);
}
```

The implementer fills the `/* resolve ... */` line and `findByName` using the actual `ContentAddressedMetadataStorage`/`Store` API (the same calls `readTree`/`resolveRef` that `gtest_cas_store.cpp` uses); the assertions (placements + inline_bytes + readability) are the spec of the test and must not be weakened.

- [ ] **Step 5: Run the inlining + transaction tests**

Run: `cd build && ninja unit_tests_dbms > cas_2d_build.log 2>&1; tail -3 cas_2d_build.log && ./src/unit_tests_dbms --gtest_filter='CaTransaction*:CaInlinePlacement*' > cas_2d_test.log 2>&1; grep -E 'PASSED|FAILED' cas_2d_test.log | tail -5`
Expected: build clean; the new inlining test + the existing `CaTransactionLockScope` tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp \
        src/Disks/tests/gtest_ca_transaction.cpp
git commit -m "CA: inline small eager part files into the tree (one-GET part open)

writeFile now stages the small eager metadata files (checksums/columns/count/
serialization/metadata_version/partition.dat/minmax_*/codec/ttl) as Inline tree
entries instead of standalone blobs, so opening a part reads them in the single
tree GET (B10/B97). Per-column data (.bin) and marks (.mrk*) and primary.idx
stay Blob to preserve column-read selectivity. Inline entries carry the content
hash, so the Merkle treeId treats inline == blob (placement never moves an id).
The streaming-blob staging is refactored into a shared stageBlobPartFile; an
INLINE_CAP (1 MiB) falls an oversized candidate back to a blob.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Full regression sweep

**Files:** none — verification only.

- [ ] **Step 1: Build + full sweep**

Run: `cd build && ninja unit_tests_dbms > cas_2d_build.log 2>&1; tail -3 cas_2d_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_2d_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_2d_sweep.log | tail -8`
Expected: all pass except the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`. The existing transaction/store/build tests still pass — `.bin`/`.mrk` behavior is unchanged (same blob staging via the helper), and the read path already supported `Inline`.

- [ ] **Step 2: (no commit)** Plan 2d complete. Note: full end-to-end validation (a real INSERT/SELECT round-trip producing inlined parts) happens in the group's praktika/soak validation, not this gtest loop.

---

## Self-Review (inline)

**Spec coverage (2d slice):** eager files → Inline, lazy `.bin`/`.mrk` → Blob (selectivity), one-GET open → Tasks 1-2; size-threshold spirit via `INLINE_CAP` + the always-blob set; `primary.idx` stays Blob (explicit follow-up, per spec "a large primary.idx may stay a blob"); Inline `file_hash` = content hash so placement doesn't move the Merkle id → Task 2 + a property already proven in 2a. ✓
**Placeholder scan:** the predicate + its test + the staging helper + the routing are complete code; two execution-time fill-ins are explicitly flagged (the unique temp-name scheme — reuse existing; the resolve→readTree accessor in the integration test — use the real Store API), each with the criteria and the assertions fixed. These are genuine "adapt to existing API" steps, not vague TODOs. ✓
**Type consistency:** `partFileMustStayBlob(std::string_view)->bool`; `stageBlobPartFile(Route, UInt128, size_t, string)`; `Cas::Placement::{Inline,Blob}`; `TreeEntry.{name,placement,file_hash,file_size,inline_bytes}` — all match the current structs; `INLINE_CAP` size_t. ✓
**Note for 2e / Plan 3:** 2e collapses `CasBuild`'s dependency-closure into the Merkle-fold walk (gtest_cas_build). Plan 3 converts the mutable objects (manifest/gc-snap/gc-state/retired-set/watermark/pool-meta) to protobuf under one `cas_format.proto`, deletes the JSON codec family + `tolerateUnknownKeys`, and removes the monotone `checkVersion`.
