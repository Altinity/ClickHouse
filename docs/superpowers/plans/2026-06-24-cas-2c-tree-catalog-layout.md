# CAS Tree Catalog-First / Inline-Last Layout — Implementation Plan (Plan 2c)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development (or executing-plans). Checkbox steps.

**Goal:** Change the tree object's on-disk **payload** to **catalog-first, inline-data-last**, and drop the now-redundant `"CATR"`+version header from the payload (the envelope's `CATR` magic + `writer`/`min_reader` from Plan 2b are the single version authority). The catalog (names + placement + child hash + size, plus an `(offset,length)` into the data section for inline entries) is a compact contiguous prefix; the concatenated inline-file bytes follow. This lets a directory listing be parsed without materializing inline bytes, and is the layout that Plan 2d's eager-file inlining writes into.

**Architecture:** Pure serialization change to `encodeTree`/`decodeTree` in `CasTreeCodec.cpp`. The function signatures are unchanged (`encodeTree(vector<TreeEntry>) -> String`, `decodeTree(string_view) -> vector<TreeEntry>` with `inline_bytes` reconstructed from the data section), so every caller (`CasStore::readTree`, `CasBuild`, fsck, GC, test helpers) is unaffected. **`treeId` is unaffected** — Plan 2a computes it via `merkleTreeId(entries)` from the logical children, not from these bytes — so this layout change moves no id and breaks no dedup. There is no on-disk data to migrate (pre-release).

**Tech Stack:** C++ (ClickHouse), gtest, ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only, no amend/rebase).

**Scope guards:** Changes ONLY `encodeTree`/`decodeTree` (the tree payload) + tests. Does NOT touch the envelope (2b, done), `merkleTreeId` (2a, done), the part-writer inline decision (2d), or `CasBuild` closure (2e). The part-writer still stages every part file as `Blob` until 2d; this plan only changes how a tree's bytes are laid out, not which placement files get. Out of scope: B92, Part IV, B164b/B147.

**Build & test:**
- Build: `cd build && ninja unit_tests_dbms > cas_2c_build.log 2>&1` — no `-j`/`nproc`; check `tail -5`.
- Tree tests: `build/src/unit_tests_dbms --gtest_filter='CasTree*:CasCodecs.*' > build/cas_2c_test.log 2>&1`.
- Full sweep (final): `--gtest_filter='Cas*:Ca*' > build/cas_2c_sweep.log 2>&1`. Only tolerated red: baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

---

## The new tree payload layout

The payload is the object bytes at `[header_len, EOF)` (after the envelope). No magic/version here (the envelope owns both):

```
[0,4)   u32  entry_count (LE)
CATALOG  (entry_count entries, each, in name-sorted order):
   u16   name_len (LE) + name bytes
   u8    placement            1=Inline, 2=Blob, 3=Subtree
   u128  file_hash (LE)       content hash (file) / child tree id (subtree)
   u64   file_size (LE)
   if placement == Inline:
     u64 data_offset (LE)     offset into the DATA section (relative to its start)
     u64 data_length (LE)     == the inline byte count
DATA section:
   concatenated inline-file bytes; entry i's bytes live at [data_section_start + data_offset, + data_length)
```
A reader parses the count + catalog (a contiguous run) to get the full directory listing and the inline `(offset,length)`s **without** touching the data section; reading one inline file is a seek into the data section. `decodeTree` reconstructs `TreeEntry::inline_bytes` from the data section so its return type is unchanged.

---

### Task 1: Rewrite `encodeTree` / `decodeTree` to the new layout

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp`
- Test: `src/Disks/tests/gtest_cas_tree_layout.cpp` (new)

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_tree_layout.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>

using namespace DB;
using namespace DB::Cas;

namespace
{

TreeEntry blobE(const String & n, UInt128 h, uint64_t sz)
{
    TreeEntry e; e.name = n; e.placement = Placement::Blob; e.file_hash = h; e.file_size = sz; return e;
}
TreeEntry inlineE(const String & n, UInt128 h, const String & bytes)
{
    TreeEntry e; e.name = n; e.placement = Placement::Inline; e.file_hash = h;
    e.file_size = bytes.size(); e.inline_bytes = bytes; return e;
}
TreeEntry subtreeE(const String & n, UInt128 id, uint64_t sz)
{
    TreeEntry e; e.name = n; e.placement = Placement::Subtree; e.file_hash = id; e.file_size = sz; return e;
}

}

TEST(CasTreeLayout, RoundTripMixedPlacements)
{
    std::vector<TreeEntry> in = {
        blobE("col.bin", 0x1111, 4096),
        inlineE("checksums.txt", 0x2222, "checksum-bytes-here"),
        inlineE("count.txt", 0x3333, "42"),
        subtreeE("proj", 0x4444, 128),
    };
    const String encoded = encodeTree(in);
    const std::vector<TreeEntry> out = decodeTree(encoded);

    /// decodeTree returns name-sorted entries; sort `in` the same way to compare.
    std::vector<TreeEntry> expect = in;
    std::sort(expect.begin(), expect.end(), [](auto & a, auto & b){ return a.name < b.name; });
    ASSERT_EQ(out.size(), expect.size());
    for (size_t i = 0; i < out.size(); ++i)
    {
        EXPECT_EQ(out[i].name, expect[i].name);
        EXPECT_EQ(out[i].placement, expect[i].placement);
        EXPECT_EQ(out[i].file_hash, expect[i].file_hash);
        EXPECT_EQ(out[i].file_size, expect[i].file_size);
        EXPECT_EQ(out[i].inline_bytes, expect[i].inline_bytes);   // empty for non-inline
    }
}

TEST(CasTreeLayout, PayloadStartsWithCountNotMagic)
{
    /// The payload no longer carries a "CATR" header (the envelope owns the magic). It begins with
    /// the u32 entry_count.
    const String encoded = encodeTree({blobE("a", 0x1, 1), blobE("b", 0x2, 2)});
    EXPECT_NE(encoded.substr(0, 4), "CATR");
    ReadBufferFromMemory in(encoded.data(), encoded.size());
    uint32_t count = 0;
    readBinaryLittleEndian(count, in);
    EXPECT_EQ(count, 2u);
}

TEST(CasTreeLayout, CatalogPrecedesInlineData)
{
    /// With a large inline blob, the entry's metadata (its name) must appear in the encoded bytes
    /// BEFORE the inline payload bytes — catalog-first.
    const String marker = std::string(1000, 'Z');
    const String encoded = encodeTree({inlineE("small.txt", 0x9, marker)});
    const auto name_pos = encoded.find("small.txt");
    const auto data_pos = encoded.find(marker);
    ASSERT_NE(name_pos, String::npos);
    ASSERT_NE(data_pos, String::npos);
    EXPECT_LT(name_pos, data_pos);
}

TEST(CasTreeLayout, DeterministicAndOrderIndependent)
{
    const String a = encodeTree({blobE("a", 0x1, 1), blobE("b", 0x2, 2)});
    const String b = encodeTree({blobE("b", 0x2, 2), blobE("a", 0x1, 1)});  // reversed input
    EXPECT_EQ(a, b);   // both sort to the same canonical bytes
}

TEST(CasTreeLayout, TruncatedPayloadThrows)
{
    const String encoded = encodeTree({inlineE("x", 0x1, "0123456789")});
    try
    {
        decodeTree(std::string_view(encoded).substr(0, encoded.size() - 3));  // cut the data section
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}
```

Add to the top of the test file (for the `CORRUPTED_DATA` extern):

```cpp
namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }
```

- [ ] **Step 2: Run — verify it fails**

Run: `cd build && ninja unit_tests_dbms > cas_2c_build.log 2>&1; ./src/unit_tests_dbms --gtest_filter='CasTreeLayout.*' > cas_2c_test.log 2>&1; grep -E 'FAILED|PASSED' cas_2c_test.log | tail`
Expected: `PayloadStartsWithCountNotMagic` FAILS (current `encodeTree` writes `"CATR"` first) and `CatalogPrecedesInlineData` FAILS (current layout interleaves inline bytes with each entry).

- [ ] **Step 3: Rewrite `encodeTree`**

Replace the body of `encodeTree` in `CasTreeCodec.cpp`. Remove the `"CATR"` + `TREE_VERSION` writes and the interleaved per-entry inline write; build the catalog then the data section:

```cpp
String encodeTree(std::vector<TreeEntry> entries)
{
    sortAndCheckDuplicateNames(entries);

    /// First pass: assign each Inline entry a contiguous (offset,length) within the data section.
    String data;
    {
        WriteBufferFromString data_buf(data);
        for (const auto & e : entries)
            if (e.placement == Placement::Inline)
                writeString(e.inline_bytes, data_buf);
        data_buf.finalize();
    }

    WriteBufferFromOwnString out;
    writeBinaryLittleEndian(static_cast<uint32_t>(entries.size()), out);   /// entry_count

    uint64_t running_offset = 0;
    for (const auto & e : entries)
    {
        writeBinaryLittleEndian(static_cast<uint16_t>(e.name.size()), out);
        writeString(e.name, out);
        writeBinaryLittleEndian(static_cast<uint8_t>(e.placement), out);
        writeU128LE(out, e.file_hash);
        writeBinaryLittleEndian(e.file_size, out);
        if (e.placement == Placement::Inline)
        {
            writeBinaryLittleEndian(running_offset, out);                  /// data_offset
            writeBinaryLittleEndian(static_cast<uint64_t>(e.inline_bytes.size()), out); /// data_length
            running_offset += e.inline_bytes.size();
        }
    }

    writeString(data, out);                                                /// DATA section
    return std::move(out.str());
}
```

(The `running_offset` order must match the data-section write order above — both iterate `entries` in the same name-sorted order, so offsets are consistent. `WriteBufferFromString`/`WriteBufferFromOwnString`/`writeString`/`writeBinaryLittleEndian`/`writeU128LE` are already included.)

- [ ] **Step 4: Rewrite `decodeTree`**

Replace the body of `decodeTree`. Parse count + catalog (recording each Inline entry's `(offset,length)`), then read the data section and slice each Inline entry's bytes out of it:

```cpp
std::vector<TreeEntry> decodeTree(std::string_view data)
{
    return decodeGuarded("tree", [&]
    {
        ReadBufferFromMemory in(data.data(), data.size());

        uint32_t count = 0;
        readBinaryLittleEndian(count, in);

        struct Pending { size_t offset; size_t length; };
        std::vector<TreeEntry> entries;
        std::vector<Pending> inline_slices;          /// index-aligned with Inline entries
        entries.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            TreeEntry e;
            uint16_t name_len = 0;
            readBinaryLittleEndian(name_len, in);
            e.name = readFixedBytes(in, name_len);

            uint8_t placement = 0;
            readBinaryLittleEndian(placement, in);
            if (placement != static_cast<uint8_t>(Placement::Inline)
                && placement != static_cast<uint8_t>(Placement::Blob)
                && placement != static_cast<uint8_t>(Placement::Subtree))
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS tree: unknown placement {}", placement);
            e.placement = static_cast<Placement>(placement);

            e.file_hash = readU128LE(in);
            readBinaryLittleEndian(e.file_size, in);

            if (e.placement == Placement::Inline)
            {
                uint64_t off = 0;
                uint64_t len = 0;
                readBinaryLittleEndian(off, in);
                readBinaryLittleEndian(len, in);
                inline_slices.push_back({static_cast<size_t>(off), static_cast<size_t>(len)});
            }
            entries.push_back(std::move(e));
        }

        /// The DATA section is the remainder. Slice each Inline entry's bytes out of it; an out-of-range
        /// (offset+length) is corruption.
        const size_t data_start = in.count();
        const std::string_view data_section = data.substr(data_start);
        size_t slice_idx = 0;
        for (auto & e : entries)
        {
            if (e.placement != Placement::Inline)
                continue;
            const Pending & p = inline_slices[slice_idx++];
            if (p.offset > data_section.size() || p.length > data_section.size() - p.offset)
                throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                    "CAS tree: inline slice [{}, {}) overruns data section of {} bytes",
                    p.offset, p.offset + p.length, data_section.size());
            e.inline_bytes = String(data_section.substr(p.offset, p.length));
        }

        return entries;
    });
}
```

(If `TREE_VERSION` is now unused after removing the `"CATR"` header, delete the `constexpr uint8_t TREE_VERSION = 1;` line to avoid an unused-constant warning.)

- [ ] **Step 5: Run — verify the tree tests pass**

Run: `cd build && ninja unit_tests_dbms > cas_2c_build.log 2>&1; tail -3 cas_2c_build.log && ./src/unit_tests_dbms --gtest_filter='CasTreeLayout.*:CasTreeId.*:CasCodecs.*' > cas_2c_test.log 2>&1; grep -E 'PASSED|FAILED' cas_2c_test.log | tail -5`
Expected: build clean; `CasTreeLayout.*` (5) pass; `CasTreeId.*` (6) still pass (id is layout-independent); `CasCodecs.*` pass. If a `CasCodecs` test asserted the old `"CATR"`-prefixed payload or the interleaved layout, update it to the new layout (round-trip semantics only; do not weaken).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp \
        src/Disks/tests/gtest_cas_tree_layout.cpp
# plus any gtest_cas_codecs.cpp tree-payload test updated in Step 5
git commit -m "CA: tree payload is catalog-first / inline-data-last (drop CATR header)

The tree payload now begins with the entry count + a contiguous catalog (name,
placement, file_hash, file_size, and (offset,length) into the data section for
inline entries), followed by the concatenated inline-file bytes. A listing is
parseable without materializing inline bytes. The redundant 'CATR'+version
payload header is removed -- the envelope's CATR magic + writer/min_reader (2b)
are the single version authority. treeId is unchanged (2a computes it from the
logical children, not these bytes), so no id moves and dedup is preserved.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Full regression sweep

**Files:** none — verification only.

- [ ] **Step 1: Build + full sweep**

Run: `cd build && ninja unit_tests_dbms > cas_2c_build.log 2>&1; tail -3 cas_2c_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_2c_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_2c_sweep.log | tail -8`
Expected: all pass except the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`. The build/store/GC tests that round-trip trees through `encodeTree`/`decodeTree` still pass (the function contract is unchanged; only the bytes between them changed).

- [ ] **Step 2:** (no commit) Plan 2c complete.

---

## Self-Review (inline)

**Spec coverage (2c slice):** catalog-first / inline-data-last payload → Task 1 layout + tests (`CatalogPrecedesInlineData`). Drop the `"CATR"` payload header (envelope owns the magic) → `encodeTree` no longer writes it; `PayloadStartsWithCountNotMagic` asserts it. `treeId` unaffected → `CasTreeId.*` re-run green in Step 5. Inline `(offset,length)` addressing (enables catalog-only listing later) → in the catalog. The catalog-only `decodeTreeCatalog` reader is **not** added here (no consumer yet; YAGNI) — the layout supports it when 2d/a listing path needs it. ✓
**Placeholder scan:** complete code for both functions + 5 tests; Step 5 codec-test update is conditional with explicit criteria (round-trip only, no weakening). ✓
**Type consistency:** `encodeTree(vector<TreeEntry>)`/`decodeTree(string_view)` signatures unchanged; `Placement::{Inline,Blob,Subtree}` (post-2a values 1/2/3); `sortAndCheckDuplicateNames` (added in 2a) reused. Offsets `uint64_t`, count `uint32_t`, name_len `uint16_t` — consistent encode/decode. ✓
**Note for 2d/2e:** 2d makes the part-writer choose `Inline` for eager small files below a threshold (which this layout stores efficiently); 2e collapses `CasBuild`'s dependency-closure into the Merkle-fold walk.
