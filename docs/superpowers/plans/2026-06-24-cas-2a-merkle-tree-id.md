# CAS Merkle `treeId` — Implementation Plan (Plan 2a of the hashed-objects group)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a tree's content-address (`treeId`) a **Merkle hash over its logical children** `(name, node_kind, child_hash)` instead of `CityHash128` of the serialized catalog — decoupling identity from serialization and from placement (inline vs blob).

**Architecture:** Add `merkleTreeId(entries)` to `CasTreeCodec`, computing the id over a frozen canonical Merkle input (domain-tagged; name length-prefixed; a `node_kind` byte for file-vs-subtree domain separation; the child content hash; **no** `file_size`, **no** `placement`, **no** inline bytes). Switch `CasBuild::stageTree` to derive the id via `merkleTreeId(entries)` before it serializes the payload, and delete the old `treeIdFor(encoded)`. The on-disk tree payload and the read path are **unchanged** in this plan — `readTree` already trusts the envelope's stored `logical_hash` against the key (`CasStore.cpp:485`) and never recomputes, so only the build-time derivation changes.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), `CityHash_v1_0_2::CityHash128` (existing), ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only, no amend/rebase).

**Scope guards:** Pre-release, no on-disk/wire compatibility needed. This plan changes ONLY how `treeId` is derived at build time + tests. It does NOT touch the envelope, the tree on-disk layout, the part-writer, or the read path (those are Plans 2b/2c/2d). Out of scope: B92 `tree_size`, Part IV machinery, B164b/B147.

**Build & test conventions (every task):**
- Build dir: `/home/mfilimonov/workspace/ClickHouse/master/build`. Build: `cd build && ninja unit_tests_dbms > cas_2a_build.log 2>&1` — **no `-j`, no `nproc`**; check `tail -5` for a `[N/N]` link line and no `error:`/`FAILED:`.
- New tests: `build/src/unit_tests_dbms --gtest_filter='CasTreeId.*' > build/cas_2a_test.log 2>&1; grep -E 'PASSED|FAILED' build/cas_2a_test.log`.
- Full regression (final task): `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/cas_2a_sweep.log 2>&1`. The ONLY tolerated red is the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

---

## File Structure

- **Modify** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h` — declare `merkleTreeId`; remove `treeIdFor`.
- **Modify** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp` — implement `merkleTreeId`; remove `treeIdFor`.
- **Modify** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` — `stageTree` derives the id via `merkleTreeId`.
- **Create** `src/Disks/tests/gtest_cas_tree_id.cpp` — Merkle identity property tests.
- **Modify** (if needed) `src/Disks/tests/gtest_cas_codecs.cpp` — drop any test asserting the old `treeIdFor`/`CityHash(encoded)` identity.

---

### Task 1: `merkleTreeId` — the Merkle identity rule

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp`
- Test: `src/Disks/tests/gtest_cas_tree_id.cpp`

**The frozen Merkle input (canonical bytes that get hashed):**
```
"CAMT"                         4-byte domain tag (CA Merkle Tree)
u8   = 1                       Merkle rule version (lives ONLY inside the hash input; never stored on disk)
u32  = entry_count             little-endian
  per entry, sorted by name byte-wise, duplicate names rejected:
    u16  name_len  (LE) + name bytes
    u8   node_kind            0 = file (Inline OR Blob), 1 = subtree
    u128 child_hash (LE)      entry.file_hash (content hash for a file; child tree id for a subtree)
treeId = lowercase-hex of CityHash128(those bytes)
```
Note what is **excluded**: `file_size`, `placement` (Inline vs Blob), and `inline_bytes`. Two trees that differ only in those produce the **same** id.

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_tree_id.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

using namespace DB;
using namespace DB::Cas;

namespace
{

TreeEntry blobEntry(const String & name, UInt128 hash, uint64_t size)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Blob;
    e.file_hash = hash;
    e.file_size = size;
    return e;
}

TreeEntry inlineEntry(const String & name, UInt128 hash, const String & bytes)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Inline;
    e.file_hash = hash;          // the content hash, same as the blob form would carry
    e.file_size = bytes.size();
    e.inline_bytes = bytes;
    return e;
}

TreeEntry subtreeEntry(const String & name, UInt128 child_id, uint64_t size)
{
    TreeEntry e;
    e.name = name;
    e.placement = Placement::Subtree;
    e.file_hash = child_id;
    e.file_size = size;
    return e;
}

}

TEST(CasTreeId, PlacementAndSizeDoNotAffectId)
{
    const UInt128 h = 0x1234567890abcdefULL;
    /// Same logical file (name + content hash), once Blob, once Inline, with a different file_size:
    /// identity must be identical.
    std::vector<TreeEntry> a = {blobEntry("data.bin", h, 100)};
    std::vector<TreeEntry> b = {inlineEntry("data.bin", h, "anything-here")};
    EXPECT_EQ(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, OrderDoesNotAffectId)
{
    const UInt128 h1 = 0xaaaa, h2 = 0xbbbb;
    std::vector<TreeEntry> a = {blobEntry("a", h1, 1), blobEntry("b", h2, 2)};
    std::vector<TreeEntry> b = {blobEntry("b", h2, 2), blobEntry("a", h1, 1)};
    EXPECT_EQ(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, NameBindsTheMapping)
{
    const UInt128 h1 = 0xaaaa, h2 = 0xbbbb;
    /// Same hashes, swapped names => a different directory => a different id.
    std::vector<TreeEntry> a = {blobEntry("a", h1, 1), blobEntry("b", h2, 1)};
    std::vector<TreeEntry> b = {blobEntry("a", h2, 1), blobEntry("b", h1, 1)};
    EXPECT_NE(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, ChildHashAffectsId)
{
    std::vector<TreeEntry> a = {blobEntry("x", 0x1111, 1)};
    std::vector<TreeEntry> b = {blobEntry("x", 0x2222, 1)};
    EXPECT_NE(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, FileVsSubtreeAreDistinct)
{
    const UInt128 h = 0x9999;
    /// A file and a subtree under the same name with the same hash must NOT collide (domain separation).
    std::vector<TreeEntry> a = {blobEntry("p", h, 1)};
    std::vector<TreeEntry> b = {subtreeEntry("p", h, 1)};
    EXPECT_NE(merkleTreeId(a).string(), merkleTreeId(b).string());
}

TEST(CasTreeId, DuplicateNameThrows)
{
    std::vector<TreeEntry> a = {blobEntry("dup", 0x1, 1), blobEntry("dup", 0x2, 1)};
    try
    {
        merkleTreeId(a);
        FAIL() << "expected BAD_ARGUMENTS";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::BAD_ARGUMENTS);
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && ninja unit_tests_dbms > cas_2a_build.log 2>&1; tail -20 cas_2a_build.log`
Expected: build FAILS — `merkleTreeId` not declared.

- [ ] **Step 3: Declare `merkleTreeId` and remove `treeIdFor` in `CasTreeCodec.h`**

In `src/Disks/.../Core/CasTreeCodec.h`, replace the `treeIdFor` declaration:

```cpp
/// The tree's logical id = cityHash128 of the encoded payload (same hashing the PoC uses for content).
TreeId treeIdFor(const String & encoded);
```

with:

```cpp
/// The tree's logical id — a MERKLE hash over the logical children only: for each entry, sorted by
/// name, the canonical input is (name, node_kind{file|subtree}, child_hash). It DELIBERATELY excludes
/// file_size, placement (Inline vs Blob) and inline bytes, so identity is independent of serialization
/// and storage layout (an inline file and a standalone blob with the same content yield the same id).
/// The rule is frozen by convention (changing it only loses dedup across the boundary; readers never
/// recompute it — `treeId` is an address). Rejects duplicate names (BAD_ARGUMENTS).
TreeId merkleTreeId(std::vector<TreeEntry> entries);
```

- [ ] **Step 4: Implement `merkleTreeId` and remove `treeIdFor` in `CasTreeCodec.cpp`**

In `src/Disks/.../Core/CasTreeCodec.cpp`, delete the `treeIdFor` function:

```cpp
TreeId treeIdFor(const String & encoded)
{
    const auto hash = CityHash_v1_0_2::CityHash128(encoded.data(), encoded.size());
    return TreeId(getHexUIntLowercase(hash));
}
```

and add `merkleTreeId` (place it after `decodeTree`). It reuses the sort + duplicate-name discipline already used by `encodeTree`:

```cpp
TreeId merkleTreeId(std::vector<TreeEntry> entries)
{
    std::sort(entries.begin(), entries.end(),
        [](const TreeEntry & a, const TreeEntry & b) { return a.name < b.name; });

    for (size_t i = 1; i < entries.size(); ++i)
    {
        if (entries[i].name == entries[i - 1].name)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CAS tree: duplicate entry name '{}'", entries[i].name);
    }

    /// Canonical, frozen Merkle input. node_kind domain-separates a file leaf from a subtree node so a
    /// blob hash and a child tree id under the same name can never collide (RFC 6962-style separation).
    WriteBufferFromOwnString buf;
    writeString("CAMT", buf);                                       /// domain tag
    writeBinaryLittleEndian(static_cast<uint8_t>(1), buf);          /// Merkle rule version (in-hash only)
    writeBinaryLittleEndian(static_cast<uint32_t>(entries.size()), buf);
    for (const auto & e : entries)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(e.name.size()), buf);
        writeString(e.name, buf);
        const uint8_t node_kind = (e.placement == Placement::Subtree) ? 1 : 0;   /// 0 = file, 1 = subtree
        writeBinaryLittleEndian(node_kind, buf);
        writeU128LE(buf, e.file_hash);
    }
    buf.finalize();

    const auto hash = CityHash_v1_0_2::CityHash128(buf.str().data(), buf.str().size());
    return TreeId(getHexUIntLowercase(hash));
}
```

(`WriteBufferFromOwnString`, `writeString`, `writeBinaryLittleEndian`, `writeU128LE`, `CityHash_v1_0_2::CityHash128`, `getHexUIntLowercase` are all already included/used in this file — no new includes needed.)

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms > cas_2a_build.log 2>&1; tail -3 cas_2a_build.log && ./src/unit_tests_dbms --gtest_filter='CasTreeId.*' > cas_2a_test.log 2>&1; grep -E 'PASSED|FAILED' cas_2a_test.log | tail -3`
Expected: build may FAIL to LINK because `CasBuild.cpp` still calls the now-removed `treeIdFor` — that is fixed in Task 2. If the build fails ONLY on `treeIdFor` in `CasBuild.cpp`, proceed to Task 2 (do not commit yet). If the build somehow links, `CasTreeId.*` shows `[  PASSED  ] 6 tests`.

> Note: Tasks 1 and 2 form one compilable unit (removing `treeIdFor` breaks its caller). Implement Task 2 before the first commit; the commit at the end of Task 2 covers both.

---

### Task 2: Switch `stageTree` to the Merkle id

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp:553-557`
- Modify (if present): `src/Disks/tests/gtest_cas_codecs.cpp`

- [ ] **Step 1: Update `stageTree` to derive the id via `merkleTreeId`**

In `CasBuild.cpp`, find (around line 553-557, after the W-TREE-BUILD loop):

```cpp
    const String encoded = encodeTree(std::move(entries));   /// canonical sort + duplicate-name check
    const TreeId id = treeIdFor(encoded);
    const UInt128 logical_hash = hexToU128(id.string());
```

Replace with (compute the Merkle id from the entries BEFORE they are moved into `encodeTree`):

```cpp
    /// Identity is the Merkle id over the logical children (independent of serialization/placement);
    /// the encoded payload below is just the on-disk representation, retained for re-upload.
    const TreeId id = merkleTreeId(entries);
    const String encoded = encodeTree(std::move(entries));   /// canonical sort + duplicate-name check
    const UInt128 logical_hash = hexToU128(id.string());
```

- [ ] **Step 2: Remove any test that asserts the OLD identity rule**

Run: `grep -n "treeIdFor" src/Disks/tests/gtest_cas_codecs.cpp src/Disks/tests/*.cpp`
If any test calls `treeIdFor` or asserts `treeId == CityHash128(encoded)`, delete that specific test case (the rule it pins no longer exists; the Merkle properties are covered by `gtest_cas_tree_id.cpp`). If no hits, no change needed.

- [ ] **Step 3: Build and run the new + codec tests**

Run: `cd build && ninja unit_tests_dbms > cas_2a_build.log 2>&1; tail -3 cas_2a_build.log && ./src/unit_tests_dbms --gtest_filter='CasTreeId.*:CasCodecs.*' > cas_2a_test.log 2>&1; grep -E 'PASSED|FAILED' cas_2a_test.log | tail -5`
Expected: build clean (no `treeIdFor` references remain); `CasTreeId.*` 6 tests pass; `CasCodecs.*` pass.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_tree_id.cpp
# also add gtest_cas_codecs.cpp if it was edited in Step 2
git commit -m "CA: derive treeId as a Merkle hash over children, not CityHash(encoded)

treeId is now merkleTreeId(entries) over the canonical (name, node_kind,
child_hash) of the sorted children -- excluding file_size, placement
(inline/blob) and inline bytes -- so identity is independent of serialization
and storage layout (inline file == standalone blob of the same content => same
id), and robust to future catalog-format changes. node_kind domain-separates a
file leaf from a subtree (RFC 6962-style). stageTree derives the id from the
entries; the old treeIdFor(encoded) is removed. The read path is unchanged --
readTree trusts the envelope logical_hash against the key and never recomputes.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Full regression sweep

**Files:** none modified — verification only.

- [ ] **Step 1: Build + full CAS gtest sweep**

Run: `cd build && ninja unit_tests_dbms > cas_2a_build.log 2>&1; tail -3 cas_2a_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_2a_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_2a_sweep.log | tail -10`
Expected: all pass EXCEPT the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`. The new `CasTreeId.*` suite is green. Build/round-trip tests still pass because the on-disk tree payload and read path are unchanged — only the build-time id derivation moved to Merkle.

- [ ] **Step 2: Confirm no `treeIdFor` references remain**

Run: `grep -rn "treeIdFor" src/Disks/ || echo "clean"`
Expected: `clean`.

- [ ] **Step 3:** (no commit) Plan 2a complete.

---

## Self-Review (performed inline)

**Spec coverage (Plan-2a slice):**
- `treeId = Merkle(sorted (name, kind, child_hash))`, no size, placement-independent, domain-separated `kind`, frozen rule, no `identity_scheme` stamp → Task 1 (`merkleTreeId`, rule version lives only inside the hash). ✓
- Replaces `CityHash128(encoded)` / removes `treeIdFor` → Tasks 1–2. ✓
- Read path unchanged (readTree trusts stored `logical_hash` vs key) → confirmed against `CasStore.cpp:485`; no read-path edits. ✓
- NOT in 2a (later sub-plans): envelope one-header repack (2b), catalog-first/inline-last layout (2c), part-writer inlining (2d). ✓

**Placeholder scan:** No TBD/TODO; complete code for `merkleTreeId` and the `stageTree` edit; exact paths/commands; Step 2 of Task 2 is a conditional grep-then-delete with explicit criteria, not a placeholder. ✓

**Type consistency:** `merkleTreeId(std::vector<TreeEntry>) -> TreeId` used identically in the header, impl, `stageTree`, and tests. `TreeEntry` fields (`name`, `placement`, `file_hash`, `file_size`, `inline_bytes`) match the current struct. `Placement::{Inline,Blob,Subtree}` match (post-pack-removal enum). ✓

**Note for 2b/2c/2d:** 2b reworks `CasEnvelope` (one header `CABL`/`CATR`, `writer`/`min_reader` via `CasFormat`, hole-free core repack, pad blob+tree to `blob_header_len=256`, future-format error → `UNKNOWN_FORMAT_VERSION`, blob-hash-over-payload assert). 2c reworks the tree on-disk payload to catalog-first/inline-data-last and drops the `"CATR"` payload header (magic now in the envelope). 2d teaches the part-writer to inline eager small files (`Placement::Inline`) below a size threshold. All three rely on 2a's identity decoupling so the layout/placement changes do not move any `treeId`.
