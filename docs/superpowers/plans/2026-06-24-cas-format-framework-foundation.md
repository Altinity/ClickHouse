# CAS Format-Framework Foundation — Implementation Plan (Plan 1 of 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `CasFormat` foundation — the `writer_version`/`min_reader_version` compatibility primitive, the per-class generation change-point tables, the one reader-gate (`gateOnRead`), and the protobuf framing-header helpers — as a self-contained, unit-tested library that Plans 2 (hashed objects) and 3 (mutable encodings) will adopt.

**Architecture:** A new header-light module `CasFormat.{h,cpp}` under the CAS `Core/`. It introduces (a) `FormatId` — the registry of persisted object classes; (b) a compiled append-only `FormatChangePoint` table per class (`generation → min_reader`), all at generation 1 today; (c) `currentWriterVersion(id, floor)` returning the `{writer, min_reader}` stamp; (d) `gateOnRead(min_reader, what)` — the single fail-closed reader rule (`UNKNOWN_FORMAT_VERSION` when `min_reader > G_BUILD`); (e) the `[magic:4][writer:u16][min_reader:u16]` framing-header read/write helpers used by protobuf objects; (f) `tolerateUnknownKeys(writer_version)` — the version-aware JSON rule. **No existing codec is modified in this plan** — the monotone `checkVersion` stays until Plans 2/3 migrate each codec onto `gateOnRead`. This plan's proof is its gtest suite.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), ninja build. Allman braces. `-Werror`.

**Branch:** `cas-vfs-path-mapping` (do NOT branch off / commit to master; add new commits, never amend/rebase).

**Scope guards:** Pre-release feature, no on-disk/wire compatibility needed. This plan adds a NEW module and NEW gtests only — it does not touch envelope/tree/manifest/gc-snap/json codecs (those are Plans 2/3). Out of scope entirely: B92 `tree_size`, Part IV roster/setting/decommission, B164b/B147.

**Build & test conventions (every task):**
- Build dir: `/home/mfilimonov/workspace/ClickHouse/master/build` (has `build.ninja` + a built `unit_tests_dbms`).
- Build: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1` — **no `-j`, no `nproc`**. Check: `tail -5 build/cas_format_build.log` ends with a `[N/N]` link line and no `error:`/`FAILED:`.
- The gtest glob is `CONFIGURE_DEPENDS`, so a new `gtest_*.cpp` is re-globbed automatically on the next ninja; if a stale build misses it, run `cd build && cmake .` once, then rebuild.
- Run new tests: `build/src/unit_tests_dbms --gtest_filter='CasFormat.*' > build/cas_format_test.log 2>&1` then `grep -E 'PASSED|FAILED' build/cas_format_test.log`.
- Full regression (final task): `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/cas_format_sweep.log 2>&1`. The ONLY tolerated red is the known baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

---

## File Structure

- **Create** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h` — the public API: `G_BUILD`, `FormatId`, `FormatChangePoint`, `WriterStamp`, `changePoints`, `currentWriterVersion`, `gateOnRead`, `FRAMING_HEADER_SIZE`, `writeFramingHeader`, `FramingHeader`, `readFramingHeader`, `tolerateUnknownKeys`.
- **Create** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp` — the change-point tables and function bodies (auto-globbed into the CA library and `unit_tests_dbms`).
- **Create** `src/Disks/tests/gtest_cas_format.cpp` — the unit tests (auto-globbed into `unit_tests_dbms`).

No other files are modified in this plan.

---

### Task 1: `CasFormat` core — `FormatId`, change-points, `currentWriterVersion`, `gateOnRead`

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_format.cpp`

- [ ] **Step 1: Write the failing test**

Create `src/Disks/tests/gtest_cas_format.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
}

using namespace DB::Cas;

TEST(CasFormat, ChangePointsExistForEveryClass)
{
    /// Every registered class has a non-empty, gen-1 baseline.
    for (auto id : {FormatId::Blob, FormatId::Tree, FormatId::Manifest, FormatId::GcSnap,
                    FormatId::GcState, FormatId::RetiredSet, FormatId::Watermark,
                    FormatId::PoolMeta, FormatId::Roster})
    {
        auto cps = changePoints(id);
        ASSERT_FALSE(cps.empty());
        EXPECT_EQ(cps.front().generation, 1u);
        EXPECT_EQ(cps.front().min_reader, 1u);
    }
}

TEST(CasFormat, CurrentWriterVersionIsGen1Baseline)
{
    auto s = currentWriterVersion(FormatId::Tree);
    EXPECT_EQ(s.writer_version, 1u);
    EXPECT_EQ(s.min_reader_version, 1u);
}

TEST(CasFormat, CurrentWriterVersionPicksNewestAtOrBelowFloor)
{
    /// With only generation 1 defined, any floor >= 1 yields {1,1}.
    auto s = currentWriterVersion(FormatId::Manifest, /*floor=*/5);
    EXPECT_EQ(s.writer_version, 1u);
    EXPECT_EQ(s.min_reader_version, 1u);
}

TEST(CasFormat, GateOnReadPassesWhenKnown)
{
    EXPECT_NO_THROW(gateOnRead(/*min_reader=*/1, "tree"));
    EXPECT_NO_THROW(gateOnRead(G_BUILD, "tree"));
}

TEST(CasFormat, GateOnReadFailsClosedOnFuture)
{
    try
    {
        gateOnRead(G_BUILD + 1, "tree");
        FAIL() << "expected UNKNOWN_FORMAT_VERSION";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION);
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -20 cas_format_build.log`
Expected: build FAILS — `CasFormat.h: No such file or directory` (the header does not exist yet).

- [ ] **Step 3: Write the header**

Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DB::Cas
{

/// The highest pool-format generation this build understands. A build keeps every decoder for
/// generations 1..G_BUILD (new code always reads old); an object is readable iff its
/// min_reader_version <= G_BUILD. Bump this (and append a change-point in CasFormat.cpp) when a new
/// format generation is introduced.
constexpr uint16_t G_BUILD = 1;

/// The registry of every self-describing persisted object class. For hashed binary objects the 4-byte
/// magic doubles as the on-disk identifier; this enum is what the version tables key on.
enum class FormatId : uint16_t
{
    Blob = 1,
    Tree = 2,
    Manifest = 3,
    GcSnap = 4,
    GcState = 5,
    RetiredSet = 6,
    Watermark = 7,
    PoolMeta = 8,
    Roster = 9,
};

/// One entry of a class's format history: at global generation `generation` the class's serialization
/// changed, and a reader must understand at least `min_reader` to read an object written at it.
/// Additive change => append {gen, <prior min_reader>}; breaking change => append {gen, gen}.
struct FormatChangePoint
{
    uint16_t generation;
    uint16_t min_reader;
};

/// The append-only change-point history for `id`, oldest first. Generation 1 is the frozen baseline
/// ({1,1} for every class today).
std::span<const FormatChangePoint> changePoints(FormatId id);

/// What a writer stamps onto an object when writing class `id` at write-floor `floor`:
/// writer_version = newest generation <= floor that this class has a format for; min_reader_version =
/// that change-point's min_reader. With one generation defined, always {1,1}.
struct WriterStamp
{
    uint16_t writer_version;
    uint16_t min_reader_version;
};
WriterStamp currentWriterVersion(FormatId id, uint16_t floor = G_BUILD);

/// THE reader rule. `min_reader_version` is read from an object's header; if it exceeds what this
/// build understands (G_BUILD), fail closed with UNKNOWN_FORMAT_VERSION — never misread a future
/// object. `what` names the object in the message.
void gateOnRead(uint16_t min_reader_version, std::string_view what);

}
```

- [ ] **Step 4: Write the implementation**

Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT_VERSION;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

/// Generation-1 baseline for every class. Future format changes APPEND to the matching array (and bump
/// G_BUILD): additive => {gen, prior_min_reader}; breaking => {gen, gen}. Never edit an existing entry.
constexpr FormatChangePoint BASELINE[] = {{1, 1}};

}

std::span<const FormatChangePoint> changePoints(FormatId id)
{
    /// Today every class shares the gen-1 baseline. As classes diverge, give each its own static array
    /// and switch on `id` here.
    switch (id)
    {
        case FormatId::Blob:
        case FormatId::Tree:
        case FormatId::Manifest:
        case FormatId::GcSnap:
        case FormatId::GcState:
        case FormatId::RetiredSet:
        case FormatId::Watermark:
        case FormatId::PoolMeta:
        case FormatId::Roster:
            return BASELINE;
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "CasFormat: unknown FormatId {}", static_cast<int>(id));
}

WriterStamp currentWriterVersion(FormatId id, uint16_t floor)
{
    const auto cps = changePoints(id);
    /// Newest change-point with generation <= floor (cps is oldest-first, non-empty, gen[0] == 1).
    const FormatChangePoint * chosen = &cps.front();
    for (const auto & cp : cps)
    {
        if (cp.generation <= floor)
            chosen = &cp;
        else
            break;
    }
    return {chosen->generation, chosen->min_reader};
}

void gateOnRead(uint16_t min_reader_version, std::string_view what)
{
    if (min_reader_version > G_BUILD)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS {}: object requires reader generation {} but this build supports at most {}",
            what, min_reader_version, G_BUILD);
}

}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -3 cas_format_build.log && ./src/unit_tests_dbms --gtest_filter='CasFormat.*' > cas_format_test.log 2>&1; grep -E 'PASSED|FAILED|OK' cas_format_test.log | tail -5`
Expected: build ends with a `[N/N]` link line, no `error:`; the test run shows `5 tests` and `[  PASSED  ] 5 tests`.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp \
        src/Disks/tests/gtest_cas_format.cpp
git commit -m "CA: add CasFormat foundation (FormatId, change-points, gateOnRead)

The writer_version/min_reader_version primitive: per-class generation
change-point tables (gen-1 baseline), currentWriterVersion picking the newest
format at/below a write-floor, and gateOnRead -- the single fail-closed reader
rule (UNKNOWN_FORMAT_VERSION when min_reader > G_BUILD). No existing codec is
wired to it yet (Plans 2/3 adopt it). Unit-tested.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Protobuf framing-header helpers

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_format.cpp`

- [ ] **Step 1: Write the failing test**

Append to `src/Disks/tests/gtest_cas_format.cpp` (and add the includes `#include <IO/WriteBufferFromString.h>` and `#include <IO/ReadBufferFromMemory.h>` at the top):

```cpp
namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

TEST(CasFormat, FramingHeaderRoundTrip)
{
    DB::WriteBufferFromOwnString out;
    writeFramingHeader(out, "CARS", WriterStamp{1, 1});
    out.finalize();
    const std::string bytes = out.str();
    ASSERT_EQ(bytes.size(), FRAMING_HEADER_SIZE);

    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    FramingHeader h = readFramingHeader(in, "CARS", "manifest");
    EXPECT_EQ(h.writer_version, 1u);
    EXPECT_EQ(h.min_reader_version, 1u);
    /// Cursor is left at the body (here: end of buffer).
    EXPECT_TRUE(in.eof());
}

TEST(CasFormat, FramingHeaderRejectsBadMagic)
{
    DB::WriteBufferFromOwnString out;
    writeFramingHeader(out, "CARS", WriterStamp{1, 1});
    out.finalize();
    const std::string bytes = out.str();

    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    try
    {
        readFramingHeader(in, "CAGS", "gc-snap");   // wrong expected magic
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasFormat, FramingHeaderGatesFutureMinReader)
{
    DB::WriteBufferFromOwnString out;
    writeFramingHeader(out, "CARS", WriterStamp{/*writer=*/2, /*min_reader=*/2});  // a future object
    out.finalize();
    const std::string bytes = out.str();

    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    try
    {
        readFramingHeader(in, "CARS", "manifest");
        FAIL() << "expected UNKNOWN_FORMAT_VERSION";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::UNKNOWN_FORMAT_VERSION);
    }
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -20 cas_format_build.log`
Expected: build FAILS — `writeFramingHeader`/`readFramingHeader`/`FRAMING_HEADER_SIZE`/`FramingHeader` not declared.

- [ ] **Step 3: Add the declarations to `CasFormat.h`**

Add these `#include`s at the top of `CasFormat.h` (after the existing includes):

```cpp
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
```

Add this block before the closing `}` of `namespace DB::Cas`:

```cpp
/// A protobuf object is prefixed with this 8-byte framing header so its version is checked BEFORE the
/// body is parsed (and so length-delimited streaming objects carry the version up front):
///   [magic:4 bytes][writer_version:u16 LE][min_reader_version:u16 LE]
constexpr size_t FRAMING_HEADER_SIZE = 8;

/// Writes the framing header. `magic` must be exactly 4 bytes (BAD_ARGUMENTS otherwise).
void writeFramingHeader(WriteBuffer & out, std::string_view magic, WriterStamp stamp);

struct FramingHeader
{
    uint16_t writer_version;
    uint16_t min_reader_version;
};

/// Reads + validates the framing header: a magic mismatch => CORRUPTED_DATA; then applies gateOnRead on
/// min_reader_version (a future object => UNKNOWN_FORMAT_VERSION). Returns the versions and leaves `in`
/// positioned at the body. `expected_magic` must be 4 bytes.
FramingHeader readFramingHeader(ReadBuffer & in, std::string_view expected_magic, std::string_view what);
```

- [ ] **Step 4: Add the implementation to `CasFormat.cpp`**

Add `#include`s at the top of `CasFormat.cpp`:

```cpp
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
```

Add `BAD_ARGUMENTS` and `CORRUPTED_DATA` to the `ErrorCodes` block:

```cpp
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
```

Add these function bodies before the closing `}` of `namespace DB::Cas`:

```cpp
void writeFramingHeader(WriteBuffer & out, std::string_view magic, WriterStamp stamp)
{
    if (magic.size() != 4)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "CasFormat: framing magic must be 4 bytes, got {}", magic.size());
    out.write(magic.data(), 4);
    writeBinaryLittleEndian(stamp.writer_version, out);
    writeBinaryLittleEndian(stamp.min_reader_version, out);
}

FramingHeader readFramingHeader(ReadBuffer & in, std::string_view expected_magic, std::string_view what)
{
    if (expected_magic.size() != 4)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "CasFormat: expected magic must be 4 bytes, got {}", expected_magic.size());

    char got[4];
    in.readStrict(got, 4);
    if (std::string_view(got, 4) != expected_magic)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS {}: bad framing magic (expected '{}')", what, expected_magic);

    FramingHeader h{};
    readBinaryLittleEndian(h.writer_version, in);
    readBinaryLittleEndian(h.min_reader_version, in);
    gateOnRead(h.min_reader_version, what);
    return h;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -3 cas_format_build.log && ./src/unit_tests_dbms --gtest_filter='CasFormat.*' > cas_format_test.log 2>&1; grep -E 'PASSED|FAILED' cas_format_test.log | tail -3`
Expected: build clean; `[  PASSED  ] 8 tests`.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp \
        src/Disks/tests/gtest_cas_format.cpp
git commit -m "CA: add CasFormat protobuf framing-header helpers

[magic:4][writer:u16][min_reader:u16] prefix for protobuf objects, so the
version is checked pre-parse and works for length-delimited streaming objects.
readFramingHeader validates the magic (CORRUPTED_DATA) then applies gateOnRead.
Unit-tested. Used by the manifest and gc-snap in Plan 3.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Version-aware JSON unknown-key rule (`tolerateUnknownKeys`)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_format.cpp`

- [ ] **Step 1: Write the failing test**

Append to `src/Disks/tests/gtest_cas_format.cpp`:

```cpp
TEST(CasFormat, TolerateUnknownKeysOnlyForFutureWriter)
{
    /// Same-or-older object: an unknown key is corruption -> strict (do NOT tolerate).
    EXPECT_FALSE(tolerateUnknownKeys(/*writer_version=*/1));
    EXPECT_FALSE(tolerateUnknownKeys(G_BUILD));
    /// Future writer: unknown keys are forward additions -> tolerate (ignore them).
    EXPECT_TRUE(tolerateUnknownKeys(G_BUILD + 1));
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -20 cas_format_build.log`
Expected: build FAILS — `tolerateUnknownKeys` not declared.

- [ ] **Step 3: Add the declaration to `CasFormat.h`**

Add before the closing `}` of `namespace DB::Cas`:

```cpp
/// The version-aware JSON unknown-key rule. A JSON metadata object is strict at or below this build's
/// generation (an unknown key is CORRUPTED_DATA — the incident-surface safety), but unknown keys are
/// tolerated (ignored) when the object is from a FUTURE writer (`writer_version > G_BUILD`), where
/// those keys are forward additions. The caller still applies gateOnRead on min_reader_version first.
bool tolerateUnknownKeys(uint16_t writer_version);
```

- [ ] **Step 4: Add the implementation to `CasFormat.cpp`**

Add before the closing `}` of `namespace DB::Cas`:

```cpp
bool tolerateUnknownKeys(uint16_t writer_version)
{
    return writer_version > G_BUILD;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -3 cas_format_build.log && ./src/unit_tests_dbms --gtest_filter='CasFormat.*' > cas_format_test.log 2>&1; grep -E 'PASSED|FAILED' cas_format_test.log | tail -3`
Expected: build clean; `[  PASSED  ] 9 tests`.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp \
        src/Disks/tests/gtest_cas_format.cpp
git commit -m "CA: add CasFormat version-aware JSON unknown-key rule

tolerateUnknownKeys(writer_version): strict at/below G_BUILD (unknown key =
CORRUPTED_DATA), lenient only for a future writer (forward additions skipped).
The JSON path (parseJsonDocument) adopts this in Plan 3 when pool-meta/roster
get the version-aware header. Unit-tested.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Full regression sweep (no live codec changed)

**Files:** none modified — verification only.

- [ ] **Step 1: Build and run the full CAS gtest sweep**

Run: `cd build && ninja unit_tests_dbms > cas_format_build.log 2>&1; tail -3 cas_format_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_format_sweep.log 2>&1; grep -E 'PASSED|FAILED|\[  FAILED  \]' cas_format_sweep.log | tail -20`
Expected: all pass EXCEPT the single known baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow`. The new `CasFormat.*` suite (9 tests) is included and green. Because no existing codec was modified, every other previously-green test stays green.

- [ ] **Step 2: Confirm `checkVersion` is still present and untouched**

Run: `grep -n "checkVersion" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h`
Expected: the monotone `checkVersion` is still defined (Plan 1 ADDS `gateOnRead` alongside; `checkVersion` is removed only when its last user migrates in Plans 2/3). No code change needed — this is a confirmation step.

- [ ] **Step 3: (No commit)** Plan 1 is complete; the foundation is in place for Plans 2 and 3.

---

## Self-Review (run after writing; performed inline)

**Spec coverage (Plan-1 slice of the spec):**
- `writer_version`/`min_reader_version` primitive + the one reader rule → Task 1 (`WriterStamp`, `gateOnRead`). ✓
- Global generation numbering + per-class change-point table (no dense map) → Task 1 (`changePoints`, `currentWriterVersion`). ✓
- 2-byte version field → `uint16_t` throughout. ✓
- Protobuf framing header `[magic][writer:u16][min_reader:u16]` → Task 2. ✓
- Version-aware JSON unknown-key rule → Task 3 (`tolerateUnknownKeys`). ✓
- `gateOnRead` replaces monotone `checkVersion` → introduced in Task 1; the actual replacement at call sites is deferred to Plans 2/3 (each codec migrates as its format is reworked), confirmed not-yet-removed in Task 4 Step 2. ✓ (intentional scope boundary)
- NOT in Plan 1 (Plans 2/3): envelope one-header/Merkle/tree layout/inlining; gc-snap→protobuf; gc-state/retired-set/watermark→protobuf; manifest `published_at_ms`; pool-meta/roster JSON migration; error-code unification at call sites. ✓ (these consume CasFormat)

**Placeholder scan:** No TBD/TODO; every code step has complete code; exact paths and commands throughout. ✓

**Type consistency:** `WriterStamp{writer_version,min_reader_version}`, `FramingHeader{writer_version,min_reader_version}`, `FormatChangePoint{generation,min_reader}`, `FormatId`, `G_BUILD` used identically across Tasks 1–3 and the tests. `gateOnRead(uint16_t, string_view)` and `currentWriterVersion(FormatId, uint16_t)` signatures match their call sites in tests. ✓

**Note for Plans 2 & 3:** They will (a) `#include CasFormat.h`, (b) stamp objects via `currentWriterVersion(FormatId::X)`, (c) gate reads via `gateOnRead`/`readFramingHeader`, and (d) remove `checkVersion` once the last JSON/binary caller is migrated. Plan 2 = hashed objects (one header `CABL`/`CATR`, hole-free core repack, Merkle `treeId`, catalog-first/inline-last, eager-file inlining). Plan 3 = mutable encodings (gc-snap streaming protobuf; gc-state/retired-set/watermark → protobuf; manifest framing + `published_at_ms`; pool-meta/roster version-aware JSON; error-code unification).
