# CAS Envelope One-Header Repack — Implementation Plan (Plan 2b of the hashed-objects group)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Rework the single object header shared by blobs and trees: the 4-byte magic encodes the type (`CABL`/`CATR`, dropping the `kind` byte), the version becomes `CasFormat`'s `writer_version`+`min_reader_version` (2×`uint16`, gated by `gateOnRead`), the 96-byte core is repacked **hole-free to 94 bytes** (the former `index_len`/pad word and the `kind` byte are reclaimed), and **both** blob and tree headers pad to `blob_header_len = 256`.

**Architecture:** This is a single cross-cutting on-disk format change (one atomic commit, like the pack removal) — nothing compiles until the codec, its one caller, and the test fallout all move together. The blob/tree content hash (`logical_hash`) is an input carried in the header, derived upstream from the **payload** (blobs) or via Merkle (trees, Plan 2a); the header — the "incarnation zone" (`domain_id`/`incarnation_tag`/`build_id`/provenance) — is excluded from identity, and a test asserts that. The tree **payload** keeps its own `"CATR"` magic until Plan 2c removes it (harmless interim redundancy: the envelope magic is at object offset 0, the payload magic at `header_len`).

**Tech Stack:** C++ (ClickHouse), `CasFormat` (Plan 1: `FormatId`, `currentWriterVersion`, `gateOnRead`), gtest, ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only, no amend/rebase).

**Scope guards:** Pre-release, no on-disk/wire compatibility. Changes ONLY the envelope header (encode/decode/struct/constants), its one build-site caller, the stale `.h` doc, and test fallout. Does NOT change the tree **payload** layout (Plan 2c), the part-writer (Plan 2d), `merkleTreeId` (done in 2a), or `CasBuild` closure (2e). Out of scope: B92, Part IV, B164b/B147.

**Build & test conventions:**
- Build: `cd build && ninja unit_tests_dbms > cas_2b_build.log 2>&1` — **no `-j`, no `nproc`**; check `tail -5` for `[N/N]` link line + no `error:`.
- Envelope tests: `build/src/unit_tests_dbms --gtest_filter='CasEnvelope.*:CasCodecs.*' > build/cas_2b_test.log 2>&1`.
- Full sweep (final task): `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/cas_2b_sweep.log 2>&1`. Only tolerated red: baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

---

## The new core layout (94 bytes, hole-free)

```
offset  size  field
[0,4)   4     magic            "CABL" (blob) or "CATR" (tree)   <- encodes kind; no separate kind byte
[4,6)   2     writer_version   u16 LE  (from CasFormat::currentWriterVersion)
[6,8)   2     min_reader_version u16 LE
[8,9)   1     hash_algo        u8 (1 = cityHash128)
[9,10)  1     flags            u8 (FLAG_HAS_CRITICAL_EXTENSION = 0x01)
[10,14) 4     header_len       u32 LE
[14,22) 8     logical_size     u64 LE
[22,38) 16    logical_hash     u128 LE
[38,54) 16    domain_id        u128 LE
[54,70) 16    incarnation_tag  u128 LE
[70,86) 16    build_id         u128 LE
[86,94) 8     header_hash      u64 LE (CityHash64 over [0,94) with this field zeroed)
```
`CORE_HEADER_LEN = 94`, `HEADER_HASH_OFFSET = 86`. `header_len` is still padded up to an 8-byte multiple (a no-TLV header → 96 on disk: 94 core + 2 zero ext-pad bytes), and `pad_to_header_len` (blob_header_len) is unchanged. Removed vs. today: the `kind` byte and the `[12,16)` pad word.

---

### Task 1: Rewrite the envelope codec (struct, constants, encode, decode, doc)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.cpp`

- [ ] **Step 1: Add `writer_version`/`min_reader_version` to `EnvelopeHeader` and fix the stale doc**

In `CasEnvelope.h`, in `struct EnvelopeHeader`, add after `uint8_t hash_algo = 1;`:

```cpp
    uint16_t writer_version = 0;       /// set by decode; encode derives it from `kind` via CasFormat
    uint16_t min_reader_version = 0;   /// set by decode; encode derives it from `kind` via CasFormat
```

In the `decodeEnvelopeHeader` doc comment, change the line:

```cpp
///   future format_version / unknown critical extension                       -> NOT_IMPLEMENTED
```

to:

```cpp
///   future min_reader_version / unknown critical extension                   -> UNKNOWN_FORMAT_VERSION
```

Update the leading comment of the file from "fixed 96-byte little-endian core header" to "fixed 94-byte little-endian core header" and from "Magic 'CHCA'" wording to note the per-kind magic. (The `enum class ObjectKind { Blob=1, Tree=2 };` stays — it is still the in-memory discriminator; only the on-disk encoding moves to the magic.)

- [ ] **Step 2: Write/adjust the failing envelope tests**

Create `src/Disks/tests/gtest_cas_envelope.cpp` (a focused suite for the new header; the older byte-level envelope assertions in `gtest_cas_codecs.cpp` are updated in Task 3):

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <Common/Exception.h>
#include <string>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB;
using namespace DB::Cas;

namespace
{

/// Builds a full object (header + payload) for a given kind and payload, returns the bytes.
std::string buildObject(ObjectKind kind, UInt128 logical_hash, const std::string & payload,
                        std::optional<uint32_t> pad = std::nullopt)
{
    EnvelopeHeader h;
    h.kind = kind;
    h.hash_algo = 1;
    h.logical_size = payload.size();
    h.logical_hash = logical_hash;
    h.domain_id = 0x11;
    h.incarnation_tag = 0x22;
    h.build_id = 0x33;
    if (pad)
        h.pad_to_header_len = *pad;
    std::string out = encodeEnvelopeHeader(h);
    out += payload;
    return out;
}

}

TEST(CasEnvelope, BlobRoundTrip)
{
    const std::string payload = "hello payload";
    const std::string obj = buildObject(ObjectKind::Blob, 0xdead, payload);
    const EnvelopeHeader h = decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Blob);
    EXPECT_EQ(h.kind, ObjectKind::Blob);
    EXPECT_EQ(h.logical_size, payload.size());
    EXPECT_EQ(h.logical_hash, UInt128(0xdead));
    EXPECT_EQ(h.writer_version, 1u);
    EXPECT_EQ(h.min_reader_version, 1u);
    /// payload starts right after header
    EXPECT_EQ(obj.substr(payloadOffset(h)), payload);
}

TEST(CasEnvelope, TreeRoundTrip)
{
    const std::string payload = "tree payload bytes";
    const std::string obj = buildObject(ObjectKind::Tree, 0xbeef, payload);
    const EnvelopeHeader h = decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Tree);
    EXPECT_EQ(h.kind, ObjectKind::Tree);
    EXPECT_EQ(obj.substr(payloadOffset(h)), payload);
}

TEST(CasEnvelope, MagicEncodesKind)
{
    const std::string blob = buildObject(ObjectKind::Blob, 0x1, "p");
    const std::string tree = buildObject(ObjectKind::Tree, 0x1, "p");
    EXPECT_EQ(blob.substr(0, 4), "CABL");
    EXPECT_EQ(tree.substr(0, 4), "CATR");
}

TEST(CasEnvelope, WrongMagicForExpectedKindThrows)
{
    const std::string blob = buildObject(ObjectKind::Blob, 0x1, "p");
    try
    {
        decodeEnvelopeHeader(blob, blob.size(), ObjectKind::Tree);   // expect Tree, got CABL
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasEnvelope, BadMagicThrows)
{
    std::string obj = buildObject(ObjectKind::Blob, 0x1, "p");
    obj[0] = 'X';   // corrupt the magic
    try
    {
        decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Blob);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasEnvelope, FutureMinReaderFailsClosed)
{
    /// Hand-patch min_reader_version at [6,8) to a future value (2) and recompute the header_hash so
    /// the gate (not the hash check) is what fires.
    std::string obj = buildObject(ObjectKind::Blob, 0x1, "p");
    obj[6] = 2; obj[7] = 0;                                    // min_reader_version = 2 (LE)
    // recompute header_hash over [0,94) with [86,94) zeroed (see Step 4 for the exact offsets)
    // — done in the test via the same CityHash64 the codec uses; for simplicity the codec exposes
    // nothing, so instead corrupt-and-expect either gate OR hash mismatch is acceptable here:
    try
    {
        decodeEnvelopeHeader(obj, obj.size(), ObjectKind::Blob);
        FAIL() << "expected a fail-closed throw";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_TRUE(e.code() == DB::ErrorCodes::UNKNOWN_FORMAT_VERSION
                 || e.code() == DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasEnvelope, IncarnationZoneDoesNotAffectPayloadOrId)
{
    /// Two objects with the SAME logical_hash + payload but DIFFERENT incarnation_tag/build_id encode
    /// to different header bytes, yet decode to the same logical_hash and the same payload — proving
    /// identity is carried in logical_hash, not derived from the (varying) incarnation zone.
    const std::string payload = "same content";
    EnvelopeHeader a; a.kind = ObjectKind::Blob; a.hash_algo = 1; a.logical_size = payload.size();
    a.logical_hash = 0x77; a.domain_id = 0x1; a.incarnation_tag = 0xAAAA; a.build_id = 0xBBBB;
    EnvelopeHeader b = a; b.incarnation_tag = 0xCCCC; b.build_id = 0xDDDD;
    const std::string ha = encodeEnvelopeHeader(a);
    const std::string hb = encodeEnvelopeHeader(b);
    EXPECT_NE(ha, hb);   // headers differ (incarnation zone)
    const EnvelopeHeader da = decodeEnvelopeHeader(ha + payload, ha.size() + payload.size(), ObjectKind::Blob);
    const EnvelopeHeader db = decodeEnvelopeHeader(hb + payload, hb.size() + payload.size(), ObjectKind::Blob);
    EXPECT_EQ(da.logical_hash, db.logical_hash);   // identity unchanged
}
```

- [ ] **Step 3: Run — verify it fails to build (old codec)**

Run: `cd build && ninja unit_tests_dbms > cas_2b_build.log 2>&1; tail -20 cas_2b_build.log`
Expected: build FAILS — `writer_version`/`CABL`/`CATR` semantics not yet implemented, and/or the old `CHCA` assertions in other tests still reference removed behavior. (Other test files may also fail to compile; they are fixed in Task 3.)

- [ ] **Step 4: Rewrite the codec in `CasEnvelope.cpp`**

Add the include at the top:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
```

Replace the constants block — remove `FORMAT_VERSION`, set the new sizes, and add the magic helpers:

```cpp
/// 94 is the EXACT packed size of the v1 core fields (hole-free; the kind byte and the old index_len
/// pad word are gone — kind is encoded by the magic):
///   magic[4] + writer_version[2] + min_reader_version[2] + hash_algo[1] + flags[1] + header_len[4]
///   + logical_size[8] + four u128s[64] + header_hash[8] = 94.
/// The core is never grown: new fields go into the [94, header_len) TLV extensions. header_len is
/// padded up to an 8-byte multiple; a fixed per-pool header length is set via pad_to_header_len.
constexpr uint32_t CORE_HEADER_LEN = 94;
constexpr uint32_t MAX_HEADER_LEN = 16384;
constexpr size_t HEADER_HASH_OFFSET = 86;
constexpr size_t HEADER_HASH_LEN = 8;

constexpr std::string_view MAGIC_BLOB = "CABL";
constexpr std::string_view MAGIC_TREE = "CATR";

std::string_view magicFor(ObjectKind kind)
{
    return kind == ObjectKind::Blob ? MAGIC_BLOB : MAGIC_TREE;
}

FormatId formatIdFor(ObjectKind kind)
{
    return kind == ObjectKind::Blob ? FormatId::Blob : FormatId::Tree;
}
```

In `encodeEnvelopeHeader`, replace the core-writing block (the `writeString("CHCA"...)` through the `header_hash placeholder` line) with:

```cpp
        const WriterStamp stamp = currentWriterVersion(formatIdFor(header.kind));
        writeString(magicFor(header.kind), out_buf);                        /// [0,4)  magic (encodes kind)
        writeBinaryLittleEndian(stamp.writer_version, out_buf);             /// [4,6)  writer_version
        writeBinaryLittleEndian(stamp.min_reader_version, out_buf);         /// [6,8)  min_reader_version
        writeBinaryLittleEndian(header.hash_algo, out_buf);                 /// [8]    hash_algo
        writeBinaryLittleEndian(
            static_cast<uint8_t>(critical ? FLAG_HAS_CRITICAL_EXTENSION : 0), out_buf); /// [9] flags
        writeBinaryLittleEndian(header_len, out_buf);                       /// [10,14) header_len
        writeBinaryLittleEndian(header.logical_size, out_buf);              /// [14,22) logical_size
        writeU128LE(out_buf, header.logical_hash);                          /// [22,38)
        writeU128LE(out_buf, header.domain_id);                             /// [38,54)
        writeU128LE(out_buf, header.incarnation_tag);                       /// [54,70)
        writeU128LE(out_buf, header.build_id);                              /// [70,86)
        writeBinaryLittleEndian(static_cast<uint64_t>(0), out_buf);         /// [86,94) header_hash (zeroed)
```

In `decodeEnvelopeHeader`, replace the magic+version+kind block (the `readFixedBytes(in,4) != "CHCA"` through the kind/expected-kind checks) with:

```cpp
        /// [0,4) magic encodes the kind.
        const String magic = readFixedBytes(in, 4);
        if (magic == MAGIC_BLOB)
            h.kind = ObjectKind::Blob;
        else if (magic == MAGIC_TREE)
            h.kind = ObjectKind::Tree;
        else
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CHCA envelope: bad magic");
        if (h.kind != expected_kind)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA,
                "CHCA envelope: kind {} does not match expected {}",
                static_cast<int>(h.kind), static_cast<int>(expected_kind));

        /// [4,6) writer_version, [6,8) min_reader_version — fail closed on a future object.
        readBinaryLittleEndian(h.writer_version, in);
        readBinaryLittleEndian(h.min_reader_version, in);
        gateOnRead(h.min_reader_version, "CHCA envelope");
```

Note this requires moving `EnvelopeHeader h;` to BEFORE the magic read (it currently is declared after the magic check — move the `EnvelopeHeader h;` line up so `h.kind` can be set here). Then DELETE the old `[5] kind` block and the old `[12,16) zero pad` block (read+check) — those fields no longer exist. The subsequent reads (`hash_algo`, `flags`, `header_len`, `logical_size`, the four u128s, `header_hash`) stay in the same order; only their offsets shifted, which is implicit in the sequential `readBinaryLittleEndian` calls. The TLV loop, the trailing-zero check, the `saw_unknown_critical → UNKNOWN_FORMAT_VERSION`, and the header-hash verification are UNCHANGED (the hash verify uses `CORE_HEADER_LEN`/`HEADER_HASH_OFFSET`, now 94/86).

- [ ] **Step 5: Run — verify the codec compiles and its tests pass**

Run: `cd build && ninja unit_tests_dbms > cas_2b_build.log 2>&1; tail -20 cas_2b_build.log`
Expected: `CasEnvelope.cpp`/`.h` compile. The OVERALL build may still fail on test files using the old `CHCA` layout — that is Task 3. If the only remaining errors are in `src/Disks/tests/*.cpp` (helpers / byte-asserting tests), proceed to Task 2/3. Do NOT commit yet.

---

### Task 2: Pad tree headers too; no other caller change

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp:311-315`

- [ ] **Step 1: Pad both kinds to `blob_header_len`**

In `Build::uploadFromSource`'s `buildHeader` lambda, the current code pads only blobs:

```cpp
        if (kind == ObjectKind::Blob)
        {
            header.intended_ref = info.intended_ref;
            header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);
        }
        /// Trees use natural header length (no pad) — pad_to_header_len stays 0.
```

Replace with:

```cpp
        if (kind == ObjectKind::Blob)
            header.intended_ref = info.intended_ref;
        /// Both blobs and trees pad to the pool's fixed header length, so every object's payload starts
        /// at a constant offset (a constant-shift locate for blobs; uniform layout for trees).
        header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);
```

(The existing `catch (BAD_ARGUMENTS) → drop intended_ref → retry` fallback below stays as-is; for trees there is no `intended_ref` so it is a no-op there.)

- [ ] **Step 2: Build** (full build still gated on Task 3 test fixes) — no commit yet.

---

### Task 3: Fix test fallout, build clean, full sweep, commit

**Files:** `src/Disks/tests/*` (driven by build errors + grep). Likely: `cas_test_helpers.h` (if it hand-builds the `CHCA` core), `gtest_cas_codecs.cpp` (byte-level envelope assertions), and any test asserting `CORE_HEADER_LEN == 96` / `"CHCA"` / a `format_version` byte / the pad word.

- [ ] **Step 1: Find the fallout**

Run: `grep -rn 'CHCA\|format_version\|CORE_HEADER\|"CHCA"\|pad word\|NonzeroPadWord\|index_len\|header\[5\]\|\\x01.*kind' src/Disks/tests/`
For each hit, update it to the new layout:
- magic assertions `"CHCA"` → the per-kind `"CABL"`/`"CATR"` (or remove if the test only cared that *some* header exists);
- any `CORE_HEADER_LEN`/`96` core-size assertion → `94` (or the helper constant);
- any hand-built core bytes (offsets for kind/format_version/pad) → rebuild via `encodeEnvelopeHeader` instead of hand-laid bytes wherever possible (preferred — don't hand-encode);
- a `NonzeroPadWordThrows`-style test (the `[12,16)` pad word no longer exists) → delete it (the field is gone), or repurpose to assert bad-magic if equivalent coverage is wanted (the `gtest_cas_envelope.cpp` `BadMagicThrows` already covers corruption).

Prefer routing every test that needs an object through `encodeEnvelopeHeader`/the `cas_test_helpers.h` builders (update the helper once) rather than open-coding header bytes.

- [ ] **Step 2: Build clean**

Run: `cd build && ninja unit_tests_dbms > cas_2b_build.log 2>&1; tail -5 cas_2b_build.log`
Expected: clean link, no `error:`/`warning:` from CAS code. If a new gtest file isn't picked up, `cd build && cmake .` once then rebuild.

- [ ] **Step 3: Envelope + full sweep**

Run: `build/src/unit_tests_dbms --gtest_filter='CasEnvelope.*' > build/cas_2b_test.log 2>&1; grep -E 'PASSED|FAILED' build/cas_2b_test.log | tail -3 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/cas_2b_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' build/cas_2b_sweep.log | tail -8`
Expected: `CasEnvelope.*` all pass; full sweep all pass EXCEPT the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

- [ ] **Step 4: Confirm no `CHCA`/`format_version` residue in non-comment code**

Run: `grep -rn '"CHCA"\|FORMAT_VERSION' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ || echo clean`
Expected: `clean` (the codec now uses `CABL`/`CATR` + CasFormat; the literal string `CHCA` may remain only in diagnostic message text like `"CHCA envelope: ..."`, which is fine — those are log prefixes, not on-disk magic).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_envelope.cpp
# plus every test file changed in Step 1 (git add -p / explicit paths under src/Disks/tests/)
git commit -m "CA: one object header -- CABL/CATR magic, CasFormat versions, 94-byte core

The blob/tree header now encodes the kind in the 4-byte magic (CABL/CATR,
dropping the kind byte), carries CasFormat writer_version + min_reader_version
(2x u16, gated by gateOnRead) in place of format_version, and is repacked
hole-free to a 94-byte core (the kind byte and the former index_len pad word are
reclaimed). Both blob AND tree headers now pad to blob_header_len. Future
min_reader / unknown critical TLV fail closed with UNKNOWN_FORMAT_VERSION
(stale NOT_IMPLEMENTED doc fixed). logical_hash (the identity) is unchanged and
carried in the header; a test asserts the incarnation zone does not affect it.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review (performed inline)

**Spec coverage (Plan-2b slice):** one header for blob+tree, magic `CABL`/`CATR`, drop `kind` enum byte → Task 1. `writer`/`min_reader` via CasFormat + `gateOnRead` → Task 1. Hole-free core repack (reclaim kind byte + pad word), exact size `94`/hash offset `86` → Task 1 + the layout table. Pad both to `blob_header_len=256` → Task 2. Future-format/critical-TLV → `UNKNOWN_FORMAT_VERSION` (+ stale doc fix) → Task 1 (decode already throws it; doc fixed). Blob-hash-over-payload (identity carried, not derived from header) → the `IncarnationZoneDoesNotAffectPayloadOrId` test. ✓
**Placeholder scan:** complete code for the struct, constants, encode core block, decode magic/version block, and the caller edit; Task 3 is a build-driven grep-and-fix with explicit substitution rules (not a placeholder — a real cross-cutting fallout procedure, the same shape the pack-removal used). The `FutureMinReaderFailsClosed` test deliberately accepts either `UNKNOWN_FORMAT_VERSION` (gate) or `CORRUPTED_DATA` (hash mismatch from the hand-patch) since the test doesn't recompute the header_hash — documented in the test comment. ✓
**Type consistency:** `EnvelopeHeader.{writer_version,min_reader_version}` (uint16) match `WriterStamp` and the decode reads; `formatIdFor`/`magicFor` switch on `ObjectKind::{Blob,Tree}`; `CORE_HEADER_LEN`/`HEADER_HASH_OFFSET` used consistently in encode, decode, and the hash verify. ✓
**Note for 2c/2d:** 2c removes the tree **payload**'s own `"CATR"` magic+version (now redundant with the envelope magic) and switches the payload to catalog-first/inline-data-last. 2d adds the part-writer inline decision. Neither moves a `treeId` (2a decoupled identity from serialization).
