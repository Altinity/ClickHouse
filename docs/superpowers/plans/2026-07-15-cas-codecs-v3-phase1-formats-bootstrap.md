# CAS Codecs V3 — Phase 1: `Formats/` Bootstrap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create `Core/Formats/` — the registry (`CasFormat` moved + extended with per-format traits), the shared text file-shape helpers (`CasTextFormat`), the shared format test battery, and the README registry skeleton. Pure addition: no production format is converted (that is phases 2–8).

**Architecture:** Every persisted CAS object becomes a text file: header line `{"type":"cas_<object>","v":N}`, body, optional trailer line, optionally wrapped in one zstd frame. `CasTextFormat` is the only code that knows this shape; per-object codecs (later phases) are key-mapping + invariants on top of it. Spec: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md`; reference: `docs/superpowers/cas/codecs_proposal_v3.md`.

**Tech Stack:** C++ (ClickHouse `dbms`), `ReadBuffer`/`WriteBuffer` + `ReadHelpers`/`WriteHelpers` JSON primitives, libzstd one-shot API (`ZSTD_compress2`/`ZSTD_decompress`), gtest (`unit_tests_dbms`, auto-globbed).

## Global Constraints {#global-constraints}

- **Allman braces** everywhere (CI style check).
- **Layering (physical):** files in `Core/Formats/` may include only: other `Formats/` headers, `Core/CasIds.h`, `Core/CasCodecUtil.h` (until it folds in), `base/`, `src/IO/`, `src/Common/`, `src/Formats/FormatSettings.h`, `<zstd.h>`. NEVER `CasBackend.h`, `CasStore.h`, or any subsystem header.
- **Error taxonomy:** malformed/truncated/over-cap/wrong-type/duplicate-key/whitespace → `CORRUPTED_DATA`; future `v` / unknown `!`-prefixed key → `UNKNOWN_FORMAT_VERSION`. No other codes on decode paths.
- **Canonical text only:** readers accept no whitespace outside JSON strings (our writers emit none). Lines end with `\n`.
- **JSON value conventions:** hashes/ids = lowercase fixed-width hex strings; unbounded `u64` = decimal strings; structurally bounded integers (counts, lengths, ms timestamps) = JSON numbers.
- **zstd:** level 3, whole-object single frame, checksum flag ON, declared content size mandatory; compression threshold 4096 bytes; sniff by magic `28 B5 2F FD`.
- **`v` stamping:** `writeHeaderLine` uses `currentCompatibilityVersion` (= `G_BUILD` = 3 today). The `G_BUILD` 3→4 bump is a **phase 2** decision (first persisted cutover), not phase 1.
- **Naming:** JSON keys 2–5 chars; `type` strings `cas_<object>`.
- **Build:** `ninja -C <build_dir> unit_tests_dbms` with output redirected to a log in the build dir (no `-j`, no `nproc`). Use an existing configured build dir (check `ls -d build*`; examples below use `build_debug` — substitute what exists).
- Commit after every task; never rebase/amend; branch `cas-gc-rebuild`.

---

### Task 1: Move `CasFormat` into `Core/Formats/`

**Files:**
- Create (move): `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h` and `.cpp` (from `Core/CasFormat.{h,cpp}`)
- Modify: the 16 C++ includers of `CasFormat.h` (list below), `src/CMakeLists.txt`

**Interfaces:**
- Consumes: existing `CasFormat.h` API (unchanged in this task).
- Produces: the header at its new path `<Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>`; a `dbms` source group for `Core/Formats`. All later tasks put files in this directory.

- [ ] **Step 1: git mv the two files**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
mkdir -p src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats
git mv src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h
git mv src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.cpp
```

- [ ] **Step 2: Rewrite all include paths**

The includers (16 C++ files): `Core/CasCodecUtil.h`, `Core/CasEnvelope.cpp`, `Core/Formats/CasFormat.cpp` (its own header), `Core/CasGcFormats.cpp`, `Core/CasGcOutcomes.cpp`, `Core/CasGenerationSeal.cpp`, `Core/CasManifestCodec.cpp`, `Core/CasPoolMeta.cpp`, `Core/CasRunFile.cpp`, `Core/CasServerRoot.cpp`, `src/Disks/tests/gtest_cas_codecs.cpp`, `gtest_cas_envelope.cpp`, `gtest_cas_format.cpp`, `gtest_cas_gc_formats.cpp`, `gtest_cas_generation_seal.cpp`, `gtest_cas_pluggable_hash.cpp`.

```bash
grep -rl 'ContentAddressed/Core/CasFormat\.h' src/ | xargs sed -i \
  's|ContentAddressed/Core/CasFormat\.h|ContentAddressed/Core/Formats/CasFormat.h|g'
grep -rn 'Core/CasFormat\.h' src/ ; echo "expect: no output"
```

- [ ] **Step 3: Register the new directory in CMake**

In `src/CMakeLists.txt`, directly after the existing line
`add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core)` (near line 134), add:

```cmake
add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats)
```

- [ ] **Step 4: Build and run the existing format tests**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_p1t1.log 2>&1; echo "NINJA_EXIT=$?"
build_debug/src/unit_tests_dbms --gtest_filter='*CasFormat*:*CasByteOrderGolden*' 2>&1 | tail -5
```
Expected: `NINJA_EXIT=0`; all filtered tests PASS (pure move, zero behavior change).

- [ ] **Step 5: Commit**

```bash
git add -A src/ && git commit -m "cas: formats v3 phase 1 — move CasFormat into Core/Formats/

Pure file move + include-path rewrite + CMake source group. No behavior
change. Formats/ is the new home of the format registry and (next tasks)
the shared text file-shape helpers, per
docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `FormatId` entries for the formats that never had one

**Files:**
- Modify: `Core/Formats/CasFormat.h` (enum), `Core/Formats/CasFormat.cpp` (`changePoints` switch)
- Test: `src/Disks/tests/gtest_cas_text_format.cpp` (new file)

**Interfaces:**
- Produces: `FormatId::RefLog = 19`, `FormatId::RefSnapshot = 20`, `FormatId::BlobMeta = 21`, `FormatId::GcHeartbeat = 22`. Task 3's traits table and phases 2–4 consume these.

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_text_format.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>

using namespace DB::Cas;

TEST(CasFormatIds, NewIdsExistWithFrozenValues)
{
    EXPECT_EQ(static_cast<uint16_t>(FormatId::RefLog), 19);
    EXPECT_EQ(static_cast<uint16_t>(FormatId::RefSnapshot), 20);
    EXPECT_EQ(static_cast<uint16_t>(FormatId::BlobMeta), 21);
    EXPECT_EQ(static_cast<uint16_t>(FormatId::GcHeartbeat), 22);
    /// Every id, old and new, has a change-point ladder (BASELINE until a real bump).
    for (auto id : {FormatId::RefLog, FormatId::RefSnapshot, FormatId::BlobMeta, FormatId::GcHeartbeat})
        EXPECT_FALSE(changePoints(id).empty());
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_p1t2a.log 2>&1; echo "NINJA_EXIT=$?"
```
Expected: `NINJA_EXIT=1` — compile error: `RefLog` is not a member of `FormatId`.

- [ ] **Step 3: Implement**

In `Core/Formats/CasFormat.h`, append to `enum class FormatId : uint16_t` after `MountLease = 18,`:

```cpp
    /// v3 text-format cutover (2026-07-15 design): ids for persisted objects that predate the
    /// registry — refsnaplog, the blob-meta sidecar, and the GC heartbeat (formerly the 24-byte
    /// unversioned exception). Values are frozen; never reuse.
    RefLog = 19,          /// cas_ref_log     — ref transaction log object
    RefSnapshot = 20,     /// cas_ref_snap    — complete per-namespace ref table
    BlobMeta = 21,        /// cas_blob_meta   — per-blob freshness sidecar
    GcHeartbeat = 22,     /// cas_gc_hb       — GC leader heartbeat
```

In `Core/Formats/CasFormat.cpp`, add the four cases to the `changePoints` switch (they return `BASELINE`, same as every other id):

```cpp
        case FormatId::RefLog:
        case FormatId::RefSnapshot:
        case FormatId::BlobMeta:
        case FormatId::GcHeartbeat:
            return BASELINE;
```

Do NOT touch `magicFor` — the new ids never had binary magics; `magicFor` keeps throwing `LOGICAL_ERROR` for them (it serves only the legacy binary codecs during migration).

- [ ] **Step 4: Run to verify it passes**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_p1t2b.log 2>&1; echo "NINJA_EXIT=$?"
build_debug/src/unit_tests_dbms --gtest_filter='CasFormatIds*' 2>&1 | tail -3
```
Expected: `NINJA_EXIT=0`, `[  PASSED  ] 1 test.`

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/ src/Disks/tests/gtest_cas_text_format.cpp
git commit -m "cas: formats v3 phase 1 — FormatId entries for refsnaplog, blob meta, heartbeat

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Per-format traits registry

**Files:**
- Modify: `Core/Formats/CasFormat.h`, `Core/Formats/CasFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_text_format.cpp`

**Interfaces:**
- Produces (consumed by `CasTextFormat` and every later phase):

```cpp
enum class TextFamily : uint8_t { Control = 1, RecordStream = 2, PayloadHybrid = 3 };
enum class KeyStrictness : uint8_t { Tolerant = 1, Strict = 2 };
enum class CompressionPolicy : uint8_t { Never = 1, Optional = 2, PinnedRaw = 3 };
struct FormatTraits
{
    FormatId id;
    std::string_view type;      /// header-line "type" value
    TextFamily family;
    KeyStrictness strictness;
    CompressionPolicy compression;
    uint64_t object_cap;        /// max DECOMPRESSED object bytes; 0 = uncapped (streamed)
    uint64_t line_cap;          /// max bytes of one text line
};
const FormatTraits & traitsFor(FormatId id);              /// LOGICAL_ERROR for Roster (reserved)
const FormatTraits * traitsForType(std::string_view type); /// nullptr when unknown
```

- [ ] **Step 1: Write the failing tests** (append to `gtest_cas_text_format.cpp`)

```cpp
TEST(CasFormatTraits, CompleteUniqueAndGated)
{
    /// Completeness: every FormatId except the reserved Roster has traits.
    const FormatId all[] = {FormatId::Blob, FormatId::Manifest, FormatId::GcState, FormatId::PoolMeta,
                            FormatId::GcOutcomes, FormatId::PartManifest, FormatId::RunFile,
                            FormatId::FoldSeal, FormatId::Owner, FormatId::ServerEpoch, FormatId::MountLease,
                            FormatId::RefLog, FormatId::RefSnapshot, FormatId::BlobMeta, FormatId::GcHeartbeat};
    std::set<std::string_view> types;
    for (FormatId id : all)
    {
        const FormatTraits & t = traitsFor(id);
        EXPECT_EQ(t.id, id);
        EXPECT_TRUE(t.type.starts_with("cas_")) << t.type;
        EXPECT_TRUE(types.insert(t.type).second) << "duplicate type " << t.type;
        EXPECT_EQ(traitsForType(t.type), &t);
    }
    EXPECT_EQ(traitsForType("cas_nope"), nullptr);
    EXPECT_THROW(traitsFor(FormatId::Roster), DB::Exception);
    /// Deterministic formats are pinned raw + strict; spot-check the two.
    EXPECT_EQ(traitsFor(FormatId::RunFile).compression, CompressionPolicy::PinnedRaw);
    EXPECT_EQ(traitsFor(FormatId::RunFile).strictness, KeyStrictness::Strict);
    EXPECT_EQ(traitsFor(FormatId::FoldSeal).compression, CompressionPolicy::PinnedRaw);
    EXPECT_EQ(traitsFor(FormatId::FoldSeal).strictness, KeyStrictness::Strict);
}
```

Add `#include <set>` and `#include <Common/Exception.h>` at the top of the test file.

- [ ] **Step 2: Run to verify it fails** — same ninja command pattern, expected compile failure (`TextFamily` undeclared).

- [ ] **Step 3: Implement**

Append to `Core/Formats/CasFormat.h` (after `magicFor`), exactly the block from **Interfaces** above, preceded by:

```cpp
/// ---- v3 text-format registry -----------------------------------------------------------------
/// One row per persisted object class (spec 2026-07-15 §corrected-object-inventory). The row is
/// the single source of the header-line `type`, the family, the strictness of unknown keys, the
/// compression policy, and the fail-closed size caps. A format missing here cannot be decoded.
```

Append to `Core/Formats/CasFormat.cpp` (values from the spec dispositions; `KiB`/`MiB` spelled out):

```cpp
namespace
{
constexpr uint64_t kKiB = 1024;
constexpr uint64_t kMiB = 1024 * 1024;

/// Caps are 100-1000x above realistic sizes (hitting one = corrupt object or protocol bug).
/// RefLog/RefSnapshot caps are provisional until phase 3 re-derives the byte budgets for JSON.
constexpr FormatTraits TRAITS[] =
{
    {FormatId::Blob,         "cas_blob",          TextFamily::PayloadHybrid, KeyStrictness::Tolerant, CompressionPolicy::Never,     256,        256},
    {FormatId::BlobMeta,     "cas_blob_meta",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::PoolMeta,     "cas_pool_meta",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::RefLog,       "cas_ref_log",       TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Optional,  64 * kMiB,  64 * kKiB},
    {FormatId::RefSnapshot,  "cas_ref_snap",      TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Optional,  64 * kMiB,  64 * kKiB},
    {FormatId::Manifest,     "cas_ref_shard",     TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Optional,  64 * kMiB,  64 * kKiB},
    {FormatId::PartManifest, "cas_part_manifest", TextFamily::PayloadHybrid, KeyStrictness::Tolerant, CompressionPolicy::Optional,  256 * kMiB, 64 * kKiB},
    {FormatId::RunFile,      "cas_run",           TextFamily::RecordStream,  KeyStrictness::Strict,   CompressionPolicy::PinnedRaw, 0,          4 * kKiB},
    {FormatId::FoldSeal,     "cas_fold_seal",     TextFamily::Control,       KeyStrictness::Strict,   CompressionPolicy::PinnedRaw, 256 * kMiB, 64 * kKiB},
    {FormatId::GcState,      "cas_gc_state",      TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::GcHeartbeat,  "cas_gc_hb",         TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::GcOutcomes,   "cas_gc_outcomes",   TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Optional,  256 * kMiB, 64 * kKiB},
    {FormatId::Owner,        "cas_owner",         TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::ServerEpoch,  "cas_epoch",         TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
    {FormatId::MountLease,   "cas_mount_lease",   TextFamily::Control,       KeyStrictness::Tolerant, CompressionPolicy::Never,     1 * kMiB,   64 * kKiB},
};
}

const FormatTraits & traitsFor(FormatId id)
{
    for (const FormatTraits & t : TRAITS)
        if (t.id == id)
            return t;
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CasFormat: no traits for FormatId {} (reserved?)", static_cast<uint16_t>(id));
}

const FormatTraits * traitsForType(std::string_view type)
{
    for (const FormatTraits & t : TRAITS)
        if (t.type == type)
            return &t;
    return nullptr;
}
```

Note on `FormatId::Manifest`: its legacy magic string is `CARS`, historically the root shard; the ref-shard object class it named is gone. It keeps a `cas_ref_shard` traits row purely so the completeness test holds; phase 3 retires or repurposes the id explicitly. Do not design against it.

- [ ] **Step 4: Run to verify PASS** (`--gtest_filter='CasFormatTraits*'`).

- [ ] **Step 5: Commit** (message: `cas: formats v3 phase 1 — per-format traits registry`, same trailer).

---

### Task 4: JSON micro-vocabulary + `JsonObjectReader`

**Files:**
- Create: `Core/Formats/CasTextFormat.h`, `Core/Formats/CasTextFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_text_format.cpp`

**Interfaces:**
- Consumes: `FormatTraits`/`KeyStrictness` (Task 3), `u128ToHex` (`Core/CasIds.h`), `readJSONString`/`skipJSONField` (`src/IO/ReadHelpers.h` — both take `const FormatSettings::JSON &`), `writeJSONString` (`src/IO/WriteHelpers.h` — takes `const FormatSettings &`).
- Produces (every later phase's codecs consume these):

```cpp
/// write side
void writeKey(WriteBuffer & out, std::string_view key, bool & first); /// '{'/',' + "key":
void writeStringValue(WriteBuffer & out, std::string_view s);         /// JSON-escaped
void writeHex128Value(WriteBuffer & out, const UInt128 & v);          /// "32-lowercase-hex"
void writeU64StringValue(WriteBuffer & out, uint64_t v);              /// "123"
void closeObject(WriteBuffer & out, bool & first);                    /// '}' ('{}' when empty)

/// read side
class JsonObjectReader
{
public:
    JsonObjectReader(ReadBuffer & in, KeyStrictness strictness, std::string_view what);
    bool nextKey(String & key);        /// false when '}' consumed; duplicate key -> CORRUPTED_DATA
    String readString();
    UInt128 readHex128();              /// exactly 32 lowercase hex or CORRUPTED_DATA
    uint64_t readU64String();
    uint64_t readU64Number();
    bool readBool();
    void skipUnknown(const String & key); /// '!'-key -> UNKNOWN_FORMAT_VERSION; Strict -> CORRUPTED_DATA; else skip
};
```

- [ ] **Step 1: Write the failing tests** (append to `gtest_cas_text_format.cpp`)

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>

namespace
{
/// Run `f` and require a DB::Exception with exactly `code`.
template <typename F>
void expectCode(int code, F && f)
{
    try
    {
        f();
        FAIL() << "expected exception code " << code;
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), code);
    }
}
}

TEST(CasJsonVocab, WriteAndReadBack)
{
    DB::WriteBufferFromOwnString out;
    bool first = true;
    writeKey(out, "tag", first);
    writeHex128Value(out, hexToU128("000102030405060708090a0b0c0d0e0f"));
    writeKey(out, "seq", first);
    writeU64StringValue(out, 18446744073709551615ULL);
    writeKey(out, "n", first);
    DB::writeIntText(7, out);
    writeKey(out, "ref", first);
    writeStringValue(out, "t-1/all_1_2_0\n\"quoted\"");
    closeObject(out, first);
    out.finalize();
    EXPECT_EQ(out.str().substr(0, 45), R"({"tag":"000102030405060708090a0b0c0d0e0f","se)");

    DB::ReadBufferFromMemory in(out.str().data(), out.str().size());
    JsonObjectReader r(in, KeyStrictness::Strict, "test");
    String key;
    ASSERT_TRUE(r.nextKey(key)); EXPECT_EQ(key, "tag");
    EXPECT_EQ(r.readHex128(), hexToU128("000102030405060708090a0b0c0d0e0f"));
    ASSERT_TRUE(r.nextKey(key)); EXPECT_EQ(key, "seq");
    EXPECT_EQ(r.readU64String(), 18446744073709551615ULL);
    ASSERT_TRUE(r.nextKey(key)); EXPECT_EQ(key, "n");
    EXPECT_EQ(r.readU64Number(), 7u);
    ASSERT_TRUE(r.nextKey(key)); EXPECT_EQ(key, "ref");
    EXPECT_EQ(r.readString(), "t-1/all_1_2_0\n\"quoted\"");
    EXPECT_FALSE(r.nextKey(key));
}

TEST(CasJsonVocab, FailClosedRules)
{
    auto reader = [](std::string_view text, KeyStrictness s, auto && consume)
    {
        DB::ReadBufferFromMemory in(text.data(), text.size());
        JsonObjectReader r(in, s, "test");
        consume(r);
    };
    /// duplicate key
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { reader(R"({"a":1,"a":2})", KeyStrictness::Tolerant, [](auto & r)
    {
        String k;
        while (r.nextKey(k)) r.readU64Number();
    }); });
    /// unknown key: Tolerant skips (nested value), Strict rejects
    reader(R"({"zz":{"deep":[1,2]},"n":5})", KeyStrictness::Tolerant, [](auto & r)
    {
        String k;
        ASSERT_TRUE(r.nextKey(k)); r.skipUnknown(k);
        ASSERT_TRUE(r.nextKey(k)); EXPECT_EQ(r.readU64Number(), 5u);
        EXPECT_FALSE(r.nextKey(k));
    });
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { reader(R"({"zz":1})", KeyStrictness::Strict, [](auto & r)
    {
        String k;
        ASSERT_TRUE(r.nextKey(k)); r.skipUnknown(k);
    }); });
    /// critical key fails closed regardless of strictness
    expectCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { reader(R"({"!x":1})", KeyStrictness::Tolerant, [](auto & r)
    {
        String k;
        ASSERT_TRUE(r.nextKey(k)); r.skipUnknown(k);
    }); });
    /// whitespace is not canonical
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { reader(R"({ "a":1})", KeyStrictness::Tolerant, [](auto & r)
    {
        String k;
        r.nextKey(k);
    }); });
    /// bad hex width / junk in u64 string
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { reader(R"({"h":"0102"})", KeyStrictness::Tolerant, [](auto & r)
    {
        String k;
        r.nextKey(k); r.readHex128();
    }); });
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { reader(R"({"s":"12x"})", KeyStrictness::Tolerant, [](auto & r)
    {
        String k;
        r.nextKey(k); r.readU64String();
    }); });
}
```

Add to the test file's includes: `#include <IO/WriteHelpers.h>`, and (once, above the anonymous
namespace) the error-code externs every gtest that checks codes needs:

```cpp
namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}
```

- [ ] **Step 2: Run to verify compile failure** (`CasTextFormat.h` does not exist).

- [ ] **Step 3: Implement**

Create `Core/Formats/CasTextFormat.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <optional>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// The v3 text file shape (spec 2026-07-15): header line {"type":"cas_<x>","v":N}, body,
/// optional trailer line, optionally one zstd frame around the whole object. This header is the
/// ONLY code that knows the shape; per-object codecs are key mapping + invariants.
///
/// Canonical text: writers emit no whitespace outside JSON strings; readers reject it
/// (CORRUPTED_DATA). Unbounded u64 values are decimal strings; hashes are 32-char lowercase hex.

/// ---- write-side JSON micro-vocabulary ----

/// Writes '{' on the first call, ',' after, then "key": . `key` must be plain ASCII (written raw).
void writeKey(WriteBuffer & out, std::string_view key, bool & first);
void writeStringValue(WriteBuffer & out, std::string_view s);
void writeHex128Value(WriteBuffer & out, const UInt128 & v);
void writeU64StringValue(WriteBuffer & out, uint64_t v);
/// Writes '}' ("{}"" when no key was written).
void closeObject(WriteBuffer & out, bool & first);

/// ---- read-side pull cursor over one canonical JSON object ----

class JsonObjectReader
{
public:
    /// Consumes the opening '{'.
    JsonObjectReader(ReadBuffer & in_, KeyStrictness strictness_, std::string_view what_);
    /// Advances to the next key; false when the closing '}' was consumed. The caller must
    /// consume the value (one read* / skipUnknown) before the next call. Duplicate keys are
    /// CORRUPTED_DATA.
    bool nextKey(String & key);
    String readString();
    UInt128 readHex128();
    uint64_t readU64String();
    uint64_t readU64Number();
    bool readBool();
    /// The evolution rule for a key the caller does not recognize: '!'-prefixed ->
    /// UNKNOWN_FORMAT_VERSION (critical); Strict -> CORRUPTED_DATA; Tolerant -> skip the value.
    void skipUnknown(const String & key);

private:
    ReadBuffer & in;
    KeyStrictness strictness;
    String what;
    std::vector<String> seen_keys;
    bool first = true;
    bool done = false;
};

}
```

Create `Core/Formats/CasTextFormat.cpp` (first slice; later tasks append):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <Common/Exception.h>
#include <Formats/FormatSettings.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <base/hex.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{
const FormatSettings & jsonWriteSettings()
{
    static const FormatSettings settings;
    return settings;
}

const FormatSettings::JSON & jsonReadSettings()
{
    static const FormatSettings::JSON settings;
    return settings;
}
}

void writeKey(WriteBuffer & out, std::string_view key, bool & first)
{
    writeChar(first ? '{' : ',', out);
    first = false;
    writeChar('"', out);
    out.write(key.data(), key.size());
    writeChar('"', out);
    writeChar(':', out);
}

void writeStringValue(WriteBuffer & out, std::string_view s)
{
    writeJSONString(s, out, jsonWriteSettings());
}

void writeHex128Value(WriteBuffer & out, const UInt128 & v)
{
    writeChar('"', out);
    const String hex = u128ToHex(v);
    out.write(hex.data(), hex.size());
    writeChar('"', out);
}

void writeU64StringValue(WriteBuffer & out, uint64_t v)
{
    writeChar('"', out);
    writeIntText(v, out);
    writeChar('"', out);
}

void closeObject(WriteBuffer & out, bool & first)
{
    if (first)
        writeChar('{', out);
    first = false;
    writeChar('}', out);
}

JsonObjectReader::JsonObjectReader(ReadBuffer & in_, KeyStrictness strictness_, std::string_view what_)
    : in(in_), strictness(strictness_), what(what_)
{
    assertChar('{', in);
}

bool JsonObjectReader::nextKey(String & key)
{
    if (done)
        return false;
    if (first)
    {
        first = false;
        if (checkChar('}', in))
        {
            done = true;
            return false;
        }
    }
    else
    {
        if (checkChar('}', in))
        {
            done = true;
            return false;
        }
        assertChar(',', in);
    }
    readJSONString(key, in, jsonReadSettings());
    assertChar(':', in);
    if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: duplicate key '{}'", what, key);
    seen_keys.push_back(key);
    return true;
}

String JsonObjectReader::readString()
{
    String s;
    readJSONString(s, in, jsonReadSettings());
    return s;
}

UInt128 JsonObjectReader::readHex128()
{
    const String hex = readString();
    if (hex.size() != 32
        || std::any_of(hex.begin(), hex.end(), [](char c) { return ::unhex(c) == 0xff || (c >= 'A' && c <= 'F'); }))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: expected 32 lowercase hex chars, got '{}'", what, hex);
    return unhexUInt<UInt128>(hex.data());
}

uint64_t JsonObjectReader::readU64String()
{
    const String s = readString();
    ReadBufferFromMemory buf(s.data(), s.size());
    uint64_t v = 0;
    readIntText(v, buf);
    if (s.empty() || !buf.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: expected decimal u64 string, got '{}'", what, s);
    return v;
}

uint64_t JsonObjectReader::readU64Number()
{
    uint64_t v = 0;
    readIntText(v, in);
    return v;
}

bool JsonObjectReader::readBool()
{
    if (checkString("true", in))
        return true;
    assertString("false", in);
    return false;
}

void JsonObjectReader::skipUnknown(const String & key)
{
    if (!key.empty() && key[0] == '!')
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS {}: critical key '{}' is not understood by this build", what, key);
    if (strictness == KeyStrictness::Strict)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown key '{}' in a strict format", what, key);
    skipJSONField(in, key, jsonReadSettings());
}

}
```

Notes for the implementer:
- `assertChar` throws `CANNOT_PARSE_INPUT_ASSERTION_FAILED`, not `CORRUPTED_DATA`. Per-object decoders in later phases run under `decodeGuarded`-style wrappers; **in this phase**, the whitespace test above expects `CORRUPTED_DATA`, so `JsonObjectReader` must not leak parse-assertion codes: wrap the body of `nextKey`, the ctor, and each `read*` in a `try { ... } catch (const Exception & e)` that rethrows `CORRUPTED_DATA` when `e.code()` is one of `CANNOT_PARSE_INPUT_ASSERTION_FAILED`, `CANNOT_PARSE_QUOTED_STRING`, `CANNOT_PARSE_NUMBER`, `CANNOT_READ_ALL_DATA`, `ATTEMPT_TO_READ_AFTER_EOF`, `INCORRECT_DATA` (declare these `extern const int` at the top; grep `src/Common/ErrorCodes.cpp` for exact spellings). Implement one private helper `template <typename F> auto guarded(F && f)` on the class and route every public method through it, preserving `UNKNOWN_FORMAT_VERSION` and `CORRUPTED_DATA` unchanged.
- Codecs never see `first`-bookkeeping: it is fully inside `writeKey`/`closeObject`.

- [ ] **Step 4: Run to verify PASS** (`--gtest_filter='CasJsonVocab*'`). Both tests green.

- [ ] **Step 5: Commit** (`cas: formats v3 phase 1 — JSON micro-vocabulary + JsonObjectReader`).

---

### Task 5: Header line, trailer line, `readLine`

**Files:**
- Modify: `Core/Formats/CasTextFormat.h`, `Core/Formats/CasTextFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_text_format.cpp`

**Interfaces:**
- Produces:

```cpp
struct TextHeader { String type; uint32_t v = 0; };
void writeHeaderLine(WriteBuffer & out, FormatId id);            /// {"type":"…","v":G_BUILD}\n
void writeTrailerLine(WriteBuffer & out, uint64_t n);            /// {"n":N}\n
TextHeader expectHeaderLine(ReadBuffer & in, FormatId id);       /// type check + v gate, first
std::optional<TextHeader> sniffHeaderLine(std::string_view bytes); /// any object; nullopt = not CAS
String readLine(ReadBuffer & in, uint64_t line_cap, std::string_view what); /// excl. '\n'
```

- [ ] **Step 1: Failing tests** (append)

```cpp
TEST(CasTextHeader, WriteExpectSniffGate)
{
    DB::WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::PoolMeta);
    out.finalize();
    EXPECT_EQ(out.str(), "{\"type\":\"cas_pool_meta\",\"v\":3}\n");

    DB::ReadBufferFromMemory in(out.str().data(), out.str().size());
    const TextHeader h = expectHeaderLine(in, FormatId::PoolMeta);
    EXPECT_EQ(h.type, "cas_pool_meta");
    EXPECT_EQ(h.v, 3u);
    EXPECT_TRUE(in.eof());

    const auto sniffed = sniffHeaderLine(out.str());
    ASSERT_TRUE(sniffed.has_value());
    EXPECT_EQ(sniffed->type, "cas_pool_meta");
    EXPECT_FALSE(sniffHeaderLine("PAR1 not a cas object").has_value());

    /// wrong type -> CORRUPTED_DATA; future v -> UNKNOWN_FORMAT_VERSION
    const String wrong = "{\"type\":\"cas_owner\",\"v\":3}\n";
    DB::ReadBufferFromMemory in2(wrong.data(), wrong.size());
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { expectHeaderLine(in2, FormatId::PoolMeta); });
    const String future = "{\"type\":\"cas_pool_meta\",\"v\":4}\n";
    DB::ReadBufferFromMemory in3(future.data(), future.size());
    expectCode(DB::ErrorCodes::UNKNOWN_FORMAT_VERSION, [&] { expectHeaderLine(in3, FormatId::PoolMeta); });
}

TEST(CasTextLines, ReadLineAndTrailer)
{
    DB::WriteBufferFromOwnString out;
    writeTrailerLine(out, 42);
    out.finalize();
    EXPECT_EQ(out.str(), "{\"n\":42}\n");

    const String two = "abc\ndef\n";
    DB::ReadBufferFromMemory in(two.data(), two.size());
    EXPECT_EQ(readLine(in, 16, "test"), "abc");
    EXPECT_EQ(readLine(in, 16, "test"), "def");
    /// missing terminator and over-cap both fail closed
    const String noterm = "abc";
    DB::ReadBufferFromMemory in2(noterm.data(), noterm.size());
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { readLine(in2, 16, "test"); });
    DB::ReadBufferFromMemory in3(two.data(), two.size());
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { readLine(in3, 2, "test"); });
}
```

- [ ] **Step 2: Verify compile failure.**

- [ ] **Step 3: Implement** (append to the header the block from **Interfaces**, with a doc comment; append to the .cpp):

```cpp
void writeHeaderLine(WriteBuffer & out, FormatId id)
{
    const FormatTraits & t = traitsFor(id);
    bool first = true;
    writeKey(out, "type", first);
    writeStringValue(out, t.type);
    writeKey(out, "v", first);
    writeIntText(currentCompatibilityVersion(), out);
    closeObject(out, first);
    writeChar('\n', out);
}

void writeTrailerLine(WriteBuffer & out, uint64_t n)
{
    bool first = true;
    writeKey(out, "n", first);
    writeIntText(n, out);
    closeObject(out, first);
    writeChar('\n', out);
}

String readLine(ReadBuffer & in, uint64_t line_cap, std::string_view what)
{
    String line;
    while (true)
    {
        if (in.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: truncated object (line without terminator)", what);
        const char c = *in.position();
        ++in.position();
        if (c == '\n')
            return line;
        line.push_back(c);
        if (line.size() > line_cap)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: line exceeds the {}-byte cap", what, line_cap);
    }
}

namespace
{
TextHeader parseHeaderObject(std::string_view line, std::string_view what)
{
    ReadBufferFromMemory buf(line.data(), line.size());
    JsonObjectReader r(buf, KeyStrictness::Tolerant, what);
    String key;
    if (!r.nextKey(key) || key != "type")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: header line must start with \"type\"", what);
    TextHeader h;
    h.type = r.readString();
    if (!r.nextKey(key) || key != "v")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: header line must carry \"v\" second", what);
    h.v = static_cast<uint32_t>(r.readU64Number());
    while (r.nextKey(key))
        r.skipUnknown(key);
    if (!buf.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: junk after the header object", what);
    return h;
}
}

TextHeader expectHeaderLine(ReadBuffer & in, FormatId id)
{
    const FormatTraits & t = traitsFor(id);
    const String line = readLine(in, t.line_cap, t.type);
    const TextHeader h = parseHeaderObject(line, t.type);
    if (h.type != t.type)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: object is a '{}', not a '{}'", t.type, h.type, t.type);
    checkCompatibility(h.v, t.type);
    return h;
}

std::optional<TextHeader> sniffHeaderLine(std::string_view bytes)
{
    constexpr uint64_t kSniffLineCap = 64 * 1024;
    try
    {
        ReadBufferFromMemory buf(bytes.data(), bytes.size());
        const String line = readLine(buf, kSniffLineCap, "sniff");
        TextHeader h = parseHeaderObject(line, "sniff");
        if (traitsForType(h.type) == nullptr)
            return std::nullopt;
        return h;
    }
    catch (const Exception &)
    {
        return std::nullopt;
    }
}
```

(`sniffHeaderLine` swallows only to answer "is this a CAS object" for `fsck`/dispatch; `expectHeaderLine` is the load-bearing gate and swallows nothing.)

- [ ] **Step 4: Run to verify PASS** (`--gtest_filter='CasTextHeader*:CasTextLines*'`).

- [ ] **Step 5: Commit** (`cas: formats v3 phase 1 — header/trailer lines + readLine`).

---

### Task 6: The zstd arm — `sealObject` / `openObject`

**Files:**
- Modify: `Core/Formats/CasTextFormat.h`, `Core/Formats/CasTextFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_text_format.cpp`

**Interfaces:**
- Consumes: `<zstd.h>` (available transitively via `clickhouse_common_io PUBLIC ch_contrib::zstd`); one-shot pattern per `src/Compression/CompressionCodecZSTD.cpp:36-64` plus `ZSTD_c_checksumFlag` per `src/IO/ZstdDeflatingWriteBuffer.cpp:46`.
- Produces:

```cpp
bool looksZstd(std::string_view bytes);              /// magic 28 B5 2F FD
String sealObject(FormatId id, String text);         /// policy-driven writer wrap
String openObject(FormatId id, std::string_view stored); /// sniff, cap-before-alloc, decompress
```

- [ ] **Step 1: Failing tests** (append)

```cpp
TEST(CasZstdArm, SealOpenPolicyAndCaps)
{
    /// Small Optional object stays raw; big one compresses; both open identically.
    String small = "{\"type\":\"cas_ref_snap\",\"v\":3}\n{}\n";
    EXPECT_EQ(sealObject(FormatId::RefSnapshot, small), small);
    EXPECT_EQ(openObject(FormatId::RefSnapshot, small), small);

    String big = "{\"type\":\"cas_ref_snap\",\"v\":3}\n{\"pad\":\"";
    big += String(8192, 'a');
    big += "\"}\n";
    const String sealed = sealObject(FormatId::RefSnapshot, big);
    ASSERT_TRUE(looksZstd(sealed));
    EXPECT_LT(sealed.size(), big.size());
    EXPECT_EQ(openObject(FormatId::RefSnapshot, sealed), big);

    /// PinnedRaw formats never compress on write and reject compressed input on read.
    EXPECT_EQ(sealObject(FormatId::FoldSeal, big), big);
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { openObject(FormatId::FoldSeal, sealed); });

    /// Declared content size over the cap fails BEFORE the output allocation: 65 MiB of text
    /// against RefSnapshot's 64 MiB cap (compresses to ~nothing, so the test is cheap on disk
    /// bytes; the 65 MiB source string is the only big allocation).
    const String over(65 * 1024 * 1024, 'b');
    const String sealed_over = sealObject(FormatId::RefSnapshot, over);
    ASSERT_TRUE(looksZstd(sealed_over));
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { openObject(FormatId::RefSnapshot, sealed_over); });

    /// A flipped byte inside the frame is caught by zstd (frame checksum is on).
    String corrupted = sealed;
    corrupted[corrupted.size() / 2] ^= 0x01;
    expectCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { openObject(FormatId::RefSnapshot, corrupted); });
}
```

- [ ] **Step 2: Verify compile failure.**

- [ ] **Step 3: Implement** (header: the three declarations with a policy doc comment; .cpp appends — add `#include <zstd.h>` at the top):

```cpp
bool looksZstd(std::string_view bytes)
{
    static constexpr char kMagic[4] = {'\x28', '\xB5', '\x2F', '\xFD'};
    return bytes.size() >= 4 && memcmp(bytes.data(), kMagic, 4) == 0;
}

namespace
{
constexpr int kZstdLevel = 3;
constexpr size_t kCompressThreshold = 4096;
}

String sealObject(FormatId id, String text)
{
    const FormatTraits & t = traitsFor(id);
    if (t.compression != CompressionPolicy::Optional || text.size() < kCompressThreshold)
        return text;

    ZSTD_CCtx * cctx = ZSTD_createCCtx();
    if (cctx == nullptr)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: cannot create zstd context", t.type);
    SCOPE_EXIT({ ZSTD_freeCCtx(cctx); });
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, kZstdLevel);
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);

    String out;
    out.resize(ZSTD_compressBound(text.size()));
    const size_t written = ZSTD_compress2(cctx, out.data(), out.size(), text.data(), text.size());
    if (ZSTD_isError(written))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: zstd compression failed: {}", t.type, ZSTD_getErrorName(written));
    out.resize(written);
    return out;
}

String openObject(FormatId id, std::string_view stored)
{
    const FormatTraits & t = traitsFor(id);
    if (!looksZstd(stored))
        return String(stored);
    if (t.compression != CompressionPolicy::Optional)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: compressed object in a format whose policy is raw", t.type);

    const unsigned long long content = ZSTD_getFrameContentSize(stored.data(), stored.size());
    if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: zstd frame without a declared content size", t.type);
    if (t.object_cap != 0 && content > t.object_cap)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: declared decompressed size {} exceeds the {}-byte cap", t.type, content, t.object_cap);

    String out;
    out.resize(content);
    const size_t got = ZSTD_decompress(out.data(), out.size(), stored.data(), stored.size());
    if (ZSTD_isError(got) || got != content)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: zstd decompression failed: {}",
            t.type, ZSTD_isError(got) ? ZSTD_getErrorName(got) : "short output");
    return out;
}
```

Add `#include <base/scope_guard.h>` for `SCOPE_EXIT`.

- [ ] **Step 4: Run to verify PASS** (`--gtest_filter='CasZstdArm*'`).

- [ ] **Step 5: Commit** (`cas: formats v3 phase 1 — whole-object zstd arm with cap-before-alloc`).

---

### Task 7: Shared format test battery + proving instance

**Files:**
- Create: `src/Disks/tests/cas_format_test_battery.h`, `src/Disks/tests/gtest_cas_format_battery.cpp`

**Interfaces:**
- Consumes: everything from Tasks 3–6.
- Produces (phases 2–7 register every real format through this):

```cpp
struct FormatBatteryCase
{
    DB::Cas::FormatId id;
    std::function<String()> encode;               /// full stored object bytes
    std::function<void(std::string_view)> decode; /// must succeed on encode() output
    String golden;                                /// pinned canonical TEXT ("" = skip golden check)
};
void runFormatBattery(const FormatBatteryCase & c);
```

- [ ] **Step 1: Create the battery header** (`src/Disks/tests/cas_format_test_battery.h`):

```cpp
#pragma once
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <fmt/format.h>
#include <functional>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}

/// The shape-level failure-mode battery every v3 format registers with (spec §testing): one call
/// exercises decode-of-encode, golden text, truncation at line boundaries and inside line 1,
/// the v+1 gate, wrong-type, and leading garbage. Key-level rules (tolerant/strict/critical/
/// duplicate) are unit-tested once on JsonObjectReader — the battery stays format-agnostic.

struct FormatBatteryCase
{
    DB::Cas::FormatId id;
    std::function<String()> encode;
    std::function<void(std::string_view)> decode;
    String golden;
};

namespace cas_battery_detail
{
template <typename F>
void expectCode(int code, F && f, const String & context)
{
    try
    {
        f();
        FAIL() << context << ": expected exception " << code;
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), code) << context << ": " << e.message();
    }
}
}

inline void runFormatBattery(const FormatBatteryCase & c)
{
    using namespace DB::Cas;
    namespace ec = DB::ErrorCodes;
    const FormatTraits & t = traitsFor(c.id);

    const String stored = c.encode();
    c.decode(stored); /// round-trip: must not throw

    /// Work on the canonical text (identical to `stored` for raw formats).
    const String text = openObject(c.id, stored);
    ASSERT_TRUE(text.starts_with("{\"type\":\"")) << t.type;

    if (!c.golden.empty())
        EXPECT_EQ(text, c.golden) << "golden text drifted for " << t.type;
    if (looksZstd(stored) && !c.golden.empty())
        EXPECT_EQ(stored, sealObject(c.id, c.golden)) << "pinned compressed arm drifted for " << t.type;

    /// Truncation at every line boundary (drop the terminator too) fails closed.
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n')
            cas_battery_detail::expectCode(ec::CORRUPTED_DATA,
                [&] { c.decode(text.substr(0, i)); }, fmt::format("{}: cut at line boundary {}", t.type, i));

    /// Truncation inside line 1.
    const size_t line1 = text.find('\n');
    ASSERT_NE(line1, String::npos);
    for (size_t i = 1; i < line1; i += 3)
        cas_battery_detail::expectCode(ec::CORRUPTED_DATA,
            [&] { c.decode(text.substr(0, i)); }, fmt::format("{}: cut inside header at {}", t.type, i));

    /// v+1 gate.
    const String v_now = fmt::format("\"v\":{}", currentCompatibilityVersion());
    const String v_next = fmt::format("\"v\":{}", currentCompatibilityVersion() + 1);
    String future = text;
    future.replace(future.find(v_now), v_now.size(), v_next);
    cas_battery_detail::expectCode(ec::UNKNOWN_FORMAT_VERSION, [&] { c.decode(future); },
        fmt::format("{}: v+1", t.type));

    /// Wrong type: another VALID registered type in the header.
    const std::string_view other = (t.id == FormatId::PoolMeta) ? "cas_owner" : "cas_pool_meta";
    String mistyped = text;
    mistyped.replace(mistyped.find(t.type), t.type.size(), String(other));
    cas_battery_detail::expectCode(ec::CORRUPTED_DATA, [&] { c.decode(mistyped); },
        fmt::format("{}: wrong type", t.type));

    /// Leading garbage.
    cas_battery_detail::expectCode(ec::CORRUPTED_DATA, [&] { c.decode("X" + text); },
        fmt::format("{}: garbage byte", t.type));
}
```

- [ ] **Step 2: Create the proving instance** (`src/Disks/tests/gtest_cas_format_battery.cpp`). It uses `FormatId::PoolMeta` traits with a toy body — **replaced by the real `cas_pool_meta` codec case in phase 2**:

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

using namespace DB::Cas;

namespace
{
String toyEncode()
{
    DB::WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::PoolMeta);
    bool first = true;
    writeKey(out, "pid", first);
    writeHex128Value(out, hexToU128("00112233445566778899aabbccddeeff"));
    writeKey(out, "gen", first);
    writeU64StringValue(out, 3);
    closeObject(out, first);
    DB::writeChar('\n', out);
    out.finalize();
    return sealObject(FormatId::PoolMeta, out.str());
}

void toyDecode(std::string_view stored)
{
    const String text = openObject(FormatId::PoolMeta, stored);
    DB::ReadBufferFromMemory in(text.data(), text.size());
    expectHeaderLine(in, FormatId::PoolMeta);
    const String body = readLine(in, traitsFor(FormatId::PoolMeta).line_cap, "toy");
    DB::ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "toy");
    String key;
    bool saw_pid = false;
    while (r.nextKey(key))
    {
        if (key == "pid") { r.readHex128(); saw_pid = true; }
        else if (key == "gen") { r.readU64String(); }
        else r.skipUnknown(key);
    }
    if (!saw_pid)
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "toy: missing pid");
    if (!in.eof())
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "toy: junk after body");
}
}

TEST(CasFormatBattery, ProvingInstance)
{
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::PoolMeta,
        .encode = toyEncode,
        .decode = toyDecode,
        .golden = "{\"type\":\"cas_pool_meta\",\"v\":3}\n"
                  "{\"pid\":\"00112233445566778899aabbccddeeff\",\"gen\":\"3\"}\n"});
}
```

Add `namespace DB { namespace ErrorCodes { extern const int CORRUPTED_DATA; } }` at the top of the file (before the anonymous namespace).

- [ ] **Step 3: Build and run**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_p1t7.log 2>&1; echo "NINJA_EXIT=$?"
build_debug/src/unit_tests_dbms --gtest_filter='CasFormatBattery*' 2>&1 | tail -3
```
Expected: `NINJA_EXIT=0`, `[  PASSED  ] 1 test.` If a battery expectation fires, fix the helper (battery is under test here as much as the toy).

- [ ] **Step 4: Run the full CAS test slice to prove no regression**

```bash
build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -3
```
Expected: all PASS.

- [ ] **Step 5: Commit** (`cas: formats v3 phase 1 — shared format test battery + proving instance`).

---

### Task 8: `Formats/README.md` — the living registry skeleton

**Files:**
- Create: `Core/Formats/README.md`

(In-tree developer doc next to code — NOT under `docs/`, so no docusaurus frontmatter/anchor rules apply.)

- [ ] **Step 1: Write the README**

```markdown
# CAS persisted formats — the living registry

Every persisted CAS object is a text file: header line `{"type":"cas_<object>","v":N}`, body
(one JSON object / sorted NDJSON records / raw payload zone), optional `{"n":…}` trailer,
optionally wrapped in ONE zstd frame (sniff by magic; checksum on; declared content size checked
against the cap before allocation). `CasTextFormat.{h,cpp}` is the only code that knows this
shape. Design: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md`; reference:
`docs/superpowers/cas/codecs_proposal_v3.md`.

**Rule:** any change to a persisted format lands in the SAME commit as its row here.

## Bucket map

| Key (under the pool prefix) | Object | Codec | Writer |
|---|---|---|---|
| `_pool_meta` | pool identity + floors | `CasPoolMetaFormat`* | pool create/admit |
| `cas/refs/<ns>/…_log` | ref transaction log | `CasRefLogFormat`* | writer commit path |
| `cas/refs/<ns>/…_snap` | complete ref table | `CasRefSnapshotFormat`* | writer/GC fold |
| `cas/refs/<ns>/…` cleanup marker | key-only presence marker (empty body) | — | GC |
| `cas/manifests/<ns>/<epoch>/<seq>/<ordinal>` | part manifest | `CasPartManifestFormat`* | part build |
| blob keys (`CasLayout::blobKey`) | blob envelope + payload | `CasBlobEnvelopeFormat`* | uploads |
| blob-meta keys (`CasLayout::blobMetaKey`) | freshness sidecar | `CasBlobMetaFormat`* | dedup/GC |
| `gc/state`, `gc/hb` | GC state / leader heartbeat | `CasGcStateFormat`* | GC |
| `gc/gen/<g>/attempt/<a>/…` | fold seal, runs, outcomes | `CasFoldSealFormat`* / `CasRecordStreamFormat`* / `CasGcOutcomesFormat`* | GC |
| `gc/server-roots/<srid>/{owner,epoch,mount}` | server-root singletons | `CasServerRootFormats`* | mount |
| `roots/…` | raw passthrough (verbatim) | — (never interpreted) | upper layers |

`*` = still the legacy binary codec; the row flips as each phase of the migration lands
(phases 2–8 in the design spec).

## Codec table

Authoritative per-format traits (type string, family, strictness, compression policy, caps) live
in `CasFormat.cpp` (`TRAITS`), asserted complete by `gtest_cas_text_format.cpp`. Key naming: keys
2–5 chars; hashes = 32-char lowercase hex strings; unbounded u64 = decimal strings; bounded
counts/lengths/ms-timestamps = numbers; units documented here per object as codecs land.

## Evolution rules (one screen)

- `v` (header line) is the ONLY version field; reader gate: `v > G_BUILD` →
  `UNKNOWN_FORMAT_VERSION`, checked before the body.
- Additive change = new tolerant key, no `v` bump; on MUTABLE objects the field is best-effort
  until the pool floor rises (an old writer's fresh re-encode drops it).
- Breaking change = `v` bump + `changePoints` + write-down-to-floor; the floor raise is what
  fences old builds out (mount gates: `min_reader_generation` forward, pool-meta `v` backward).
- Deterministic formats (`cas_fold_seal`, `cas_run`): strict keys, pinned raw, and the adoption
  pin — on a `putDeterministicArtifact` conflict, re-encode at the `v` of the EXISTING object.
- A key prefixed `!` is critical: a reader that does not understand it fails closed.
- Padding zones (blob header pad, manifest banners) are deterministic and verified — no
  unaccounted bytes in any object.
```

- [ ] **Step 2: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/README.md
git commit -m "cas: formats v3 phase 1 — Formats/README.md living-registry skeleton

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Phase-1 Exit Criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- No production codec changed; `Formats/` contains registry + shape + battery only.
- Phases 2–8 get their own plans (written just-in-time against this foundation), per the design
  spec's migration order.
