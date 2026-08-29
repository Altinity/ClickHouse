---
description: 'Phase-1 implementation plan for the CAS semantic wire-keys design: move every codec onto single carriers with the old spellings, persisted wire bytes frozen'
sidebar_label: 'CAS wire keys phase 1'
sidebar_position: 20
slug: /superpowers/plans/cas-wire-keys-phase1-carriers
title: 'CAS wire keys — phase 1 (carriers)'
doc_type: 'guide'
---

# CAS Wire Keys — Phase 1 (Carriers) Implementation Plan {#cas-wire-keys-phase1-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persisted-wire-preserving preparation for the CAS semantic wire-keys cut: every codec's keys and enum words move onto single production carriers (`WireKey` constants, key bundles, `EnumWireTable`) with the OLD spellings, so that phase 2 flips the vocabulary in one place. "Persisted-wire-preserving", not "behavior-preserving": persisted bytes are frozen absolutely; two narrow non-wire surfaces may change where a task says so (a defensive encode-branch error code, and `CasInspect` word normalization).

**Architecture:** Three infrastructure pieces land first (`WireKey` + field write helpers in `CasTextFormat.h`; `EnumWireTable` in a new header with `magic_enum`-backed asserts confined to `.cpp`/tests; the shared envelope-limits header owning `kMinBlobHeaderLen`). Then each of the codecs migrates its writer and reader onto the carriers, codec by codec, with the wire bytes proven unchanged by the existing goldens. Four audit-driven member renames and the test-battery closure ride along. NOTHING in this plan changes a single persisted byte.

**Tech Stack:** C++23, gtest (`unit_tests_dbms`, filter exactly `CAS*`), `magic_enum` (contrib, `.cpp`/tests only), ninja.

**Spec:** `docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md` (revision 14, approved). The plan argues from the spec; read both. Key spec sections: "Codec structure" (carriers, helper-versus-framework boundary), "Implementation shape" (this plan is its phase 1), "Test strategy".

**Follow-up plans (deferred with placement, not dropped):** phase 2 (atomic wire cut: new spellings, generation reset, goldens, tightenings, `static_assert` budget, `writeWordArrayField`+`algos_used` array, `class` words, `min_active_build_sequence`) will be written as `docs/superpowers/plans/<date>-cas-wire-keys-phase2-cut.md` after this plan's Task 21 gate passes; phase 3 (proof and measurement) as `...-phase3-proof.md` after phase 2. The `writeWordArrayField` primitive deliberately does NOT appear in this plan — it lands in phase 2 with its only consumer.

## Global Constraints {#global-constraints}

- **Persisted wire bytes are frozen for this entire plan.** Every existing wire golden stays
  byte-identical. If any `CAS*` test wants a changed expected string, the change is wrong — with
  exactly two sanctioned, per-commit-documented exceptions: (a) Task 2 Step 6 — a *defensive,
  unreachable-by-input* encode-branch error code may change (never a decode behavior); (b) Task 19 —
  `CasInspect` introspection output is normalized onto the wire words, with every changed word
  pinned by a new exact assertion.
- Branch: work directly on `cas-gc-rebuild` (where the spec lives) — no worktrees, no branch
  switches. Never rebase or amend; new commits only. Never push.
- C++ style: Allman braces everywhere. Comments state constraints, not provenance — never cite this plan, the spec, or reviews in code comments.
- The mandatory FULL GATE is `$BUILD/src/unit_tests_dbms --gtest_filter='CAS*'` — as the gate, the
  filter is EXACTLY `CAS*`. Focused filters (e.g. one suite) are fine as fast development runs
  inside a step, but never substitute for the task's closing full gate. `$BUILD` is your build
  directory (e.g. `build`). If the binary is not at `$BUILD/src/unit_tests_dbms`, find it once with
  `find $BUILD -name unit_tests_dbms` and use that path throughout.
- Build command (always redirect, per project rules; the exit status must propagate — do not append
  `echo`): `ninja -C $BUILD unit_tests_dbms > $BUILD/build_wirekeys_task<N>.log 2>&1` — no `-j`, no
  `nproc`. Have a subagent summarize every build and test log (not only failures); logs get unique
  per-task names.
- Run the full gate GREEN once before Task 1 (the plan's baseline); after that, each task ends with
  its own GREEN full gate, which is the next task's baseline. Log to a unique file per run:
  `$BUILD/test_cas_task<N>.log`.
- `magic_enum` may be included ONLY from `.cpp` files and test files, never from headers under `Formats/`.
- No `std::map`/`std::unordered_map`, no heap allocation, no stored callables anywhere in the carriers. `EnumWireTable::toWord` is a direct indexed lookup; `fromWord` is a linear scan.
- Reader grammar stays hand-written and explicit: match helpers never own the read loop, never decide unknown-key policy, never track requiredness (spec: "helper-versus-framework boundary").
- All existing spellings in this plan are verbatim from the current codecs (they were audited); if a constant in this plan disagrees with what a codec file actually writes, the codec file wins — copy from it and note the discrepancy in the commit message.

---

### Task 1: `WireKey` and the per-encoding field write helpers {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h`
- Test: `src/Disks/tests/gtest_cas_json_writer.cpp`

**Interfaces:**
- Consumes: existing `CasJsonWriter`, `writeKey(CasJsonWriter &, std::string_view, bool &)`, `writeStringValue`, `writeU64StringValue`, `writeHex128Value`, `writeBoolValue`, and `writeIntText(value, out)`.
- Produces (all later tasks call these):
  - `struct WireKey { std::string_view text; explicit constexpr WireKey(std::string_view); }` with `friend constexpr bool operator==(std::string_view, const WireKey &)` (and the reversed order via C++20 rewriting) so readers can write `key == SomeWire::state`.
  - `void writeKey(CasJsonWriter & out, WireKey key, bool & first)` — overload delegating to the existing string_view `writeKey`.
  - Field helpers, each `inline`, each exactly key-then-value, no other behavior:
    `writeWordField(CasJsonWriter &, WireKey, std::string_view word, bool & first)`,
    `writeStringField(CasJsonWriter &, WireKey, std::string_view value, bool & first)`,
    `writeU64StringField(CasJsonWriter &, WireKey, uint64_t, bool & first)`,
    `writeNumberField(CasJsonWriter &, WireKey, uint64_t, bool & first)`,
    `writeHex128Field(CasJsonWriter &, WireKey, const UInt128 &, bool & first)`,
    `writeBoolField(CasJsonWriter &, WireKey, bool, bool & first)`.
  - There is deliberately NO `writeField` overloaded on value type (the encoding is range-driven, not type-driven — the helper name must state the encoding).

- [ ] **Step 1: Write the failing test** — append to `gtest_cas_json_writer.cpp`:

```cpp
TEST(CASJsonWriter, WireKeyFieldHelpersMatchThePrimitivePairs)
{
    CasJsonWriter w;
    bool first = true;
    constexpr WireKey k_word{"st"};
    constexpr WireKey k_str{"hn"};
    constexpr WireKey k_u64s{"we"};
    constexpr WireKey k_num{"eat"};
    constexpr WireKey k_hex{"su"};
    constexpr WireKey k_bool{"fen"};
    writeWordField(w, k_word, "clean", first);
    writeStringField(w, k_str, "host-1", first);
    writeU64StringField(w, k_u64s, 7, first);
    writeNumberField(w, k_num, 1752537630000, first);
    writeHex128Field(w, k_hex, DB::UInt128{1}, first);
    writeBoolField(w, k_bool, false, first);
    w.closeObject(first);
    w.newline();
    EXPECT_EQ(std::move(w).take(),
        "{\"st\":\"clean\",\"hn\":\"host-1\",\"we\":\"7\",\"eat\":1752537630000,"
        "\"su\":\"00000000000000000000000000000001\",\"fen\":false}\n");

    /// The reader-side comparison contract: a String key compares against the constant.
    String key = "st";
    EXPECT_TRUE(key == k_word);
    EXPECT_FALSE(key == k_str);
}
```

- [ ] **Step 2: Run to verify it fails** — build, then `$BUILD/src/unit_tests_dbms --gtest_filter='CASJsonWriter.WireKeyFieldHelpersMatchThePrimitivePairs'`. Expected: compile failure (`WireKey` not defined) — a compile failure of the new test IS the failing state.

- [ ] **Step 3: Implement in `CasTextFormat.h`** (near `CasJsonWriter`; Allman):

```cpp
/// A wire-key carrier. The explicit constructor keeps raw string literals out of writer call
/// sites: a codec passes its named constant, and an inline `WireKey{"..."}` is deliberately loud.
struct WireKey
{
    std::string_view text;

    explicit constexpr WireKey(std::string_view text_) : text(text_) {}

    friend constexpr bool operator==(std::string_view s, const WireKey & k) { return s == k.text; }
};

inline void writeKey(CasJsonWriter & out, WireKey key, bool & first)
{
    writeKey(out, key.text, first);
}

inline void writeWordField(CasJsonWriter & out, WireKey key, std::string_view word, bool & first)
{
    writeKey(out, key, first);
    writeStringValue(out, word);
}

inline void writeStringField(CasJsonWriter & out, WireKey key, std::string_view value, bool & first)
{
    writeKey(out, key, first);
    writeStringValue(out, value);
}

inline void writeU64StringField(CasJsonWriter & out, WireKey key, uint64_t value, bool & first)
{
    writeKey(out, key, first);
    writeU64StringValue(out, value);
}

inline void writeNumberField(CasJsonWriter & out, WireKey key, uint64_t value, bool & first)
{
    writeKey(out, key, first);
    out.u64Number(value);
}

inline void writeHex128Field(CasJsonWriter & out, WireKey key, const UInt128 & value, bool & first)
{
    writeKey(out, key, first);
    writeHex128Value(out, value);
}

inline void writeBoolField(CasJsonWriter & out, WireKey key, bool value, bool & first)
{
    writeKey(out, key, first);
    writeBoolValue(out, value);
}
```

If `writeNumberField`'s body does not compile because number writing goes through a free function rather than `out.u64Number`, use the same call the existing codecs use for `writeIntText(value, out)`-style numeric values — copy it from `CasGcStateFormat.cpp`'s `gcs` field.

`writeStringField` and `writeWordField` have identical bodies today — that is intentional; they carry different contracts (open string vs wire-table word) and phase 2 relies on the distinction being visible at call sites.

- [ ] **Step 4: Run the new test and the full gate** — both green: `--gtest_filter='CASJsonWriter.*'` then `--gtest_filter='CAS*'` (log to `$BUILD/test_cas_task1.log`).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h src/Disks/tests/gtest_cas_json_writer.cpp
git commit -m "cas: add WireKey and per-encoding field write helpers"
```

---

### Task 2: `EnumWireTable` with compile-time proofs {#task-2}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTable.h` — the header lives in `Primitives/`, NOT `Formats/`: `Primitives` has zero outward dependencies (its README states the layering) and both `Primitives/CasBlobDigest` and every `Formats/` codec must be able to include the table in the allowed direction
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h`
- Test: Create `src/Disks/tests/gtest_cas_enum_wire_table.cpp` (register it the same way sibling `gtest_cas_*.cpp` files are registered — check `src/Disks/tests/CMakeLists.txt` or the glob the directory uses; if sibling tests need no registration, neither does this one)

**Interfaces:**
- Produces (every table-owning task consumes):
  - `template <typename Enum, size_t N> struct EnumWireTable` with nested `struct Entry { Enum value; std::string_view word; };`, member `std::array<Entry, N> entries;`, constexpr predicates `denseAndOrdered()`, `wordsUnique()`, and methods `std::string_view toWord(Enum, std::string_view what) const` (O(1) indexed; throws `LOGICAL_ERROR` on an out-of-range value) and `Enum fromWord(std::string_view word, std::string_view what) const` (linear; throws `CORRUPTED_DATA` on an unknown word).
  - `CasEnumWireTableAsserts.h`: `template <auto & Table, typename Enum> consteval bool casEnumTableCoversEnum()` proving SET EQUALITY with `magic_enum::enum_values<Enum>()`. This header includes `magic_enum.hpp` and must only ever be included from `.cpp` files and tests.

- [ ] **Step 1: Write the failing test** — `gtest_cas_enum_wire_table.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTable.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>
#include <Disks/tests/cas_test_helpers.h>
#include <gtest/gtest.h>

using namespace DB::Cas;

namespace
{

enum class Fruit : uint8_t
{
    Apple = 0,
    Pear = 1,
    Plum = 2,
};

constexpr EnumWireTable<Fruit, 3> fruits{{{
    {Fruit::Apple, "apple"},
    {Fruit::Pear, "pear"},
    {Fruit::Plum, "plum"},
}}};

static_assert(fruits.denseAndOrdered());
static_assert(fruits.wordsUnique());
static_assert(casEnumTableCoversEnum<fruits, Fruit>());

/// A one-based dense enum exercises the index arithmetic from the first entry's value.
enum class Grade : uint8_t
{
    Low = 1,
    Mid = 2,
    High = 3,
};

constexpr EnumWireTable<Grade, 3> grades{{{
    {Grade::Low, "low"},
    {Grade::Mid, "mid"},
    {Grade::High, "high"},
}}};

static_assert(grades.denseAndOrdered());
static_assert(casEnumTableCoversEnum<grades, Grade>());

}

TEST(CASEnumWireTable, RoundTripsEveryEntryBothWays)
{
    for (const auto & e : fruits.entries)
    {
        EXPECT_EQ(fruits.toWord(e.value, "fruits"), e.word);
        EXPECT_EQ(fruits.fromWord(e.word, "fruits"), e.value);
    }
    for (const auto & e : grades.entries)
        EXPECT_EQ(grades.fromWord(grades.toWord(e.value, "grades"), "grades"), e.value);
}

TEST(CASEnumWireTable, FromWordFailsClosed)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
        [&] { fruits.fromWord("banana", "fruits"); });
}

/// `LOGICAL_ERROR` aborts the process in debug/sanitizer builds (`handle_error_code`), so the
/// defensive toWord branch needs the death-test split this test directory already uses (see
/// `gtest_cas_gc_state_format.cpp`'s `RejectsZeroGcShardsOnEncode` pair) — a bare EXPECT_THROW
/// would SIGABRT the whole gate binary on those lanes.
#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(CASEnumWireTableDeathTest, ToWordAbortsOnOutOfRangeValue)
{
    EXPECT_DEATH(fruits.toWord(static_cast<Fruit>(99), "fruits"), "outside the wire vocabulary");
}
#else
TEST(CASEnumWireTable, ToWordThrowsLogicalErrorOnOutOfRangeValue)
{
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::LOGICAL_ERROR,
        [&] { fruits.toWord(static_cast<Fruit>(99), "fruits"); });
}
#endif

/// The compile-time proofs must also be exercised in the direction where they can fail — a
/// predicate rewritten to `return true;` must break this file. All three are constexpr, so the
/// negative cases are plain static_asserts over deliberately bad tables:
namespace bad_tables
{

enum class Sparse : uint8_t { A = 0, B = 2 };
constexpr EnumWireTable<Sparse, 2> sparse{{{{Sparse::A, "a"}, {Sparse::B, "b"}}}};
static_assert(!sparse.denseAndOrdered());

constexpr EnumWireTable<Fruit, 3> dup_words{{{
    {Fruit::Apple, "apple"}, {Fruit::Pear, "apple"}, {Fruit::Plum, "plum"}}}};
static_assert(!dup_words.wordsUnique());

constexpr EnumWireTable<Fruit, 3> dup_value{{{
    {Fruit::Apple, "apple"}, {Fruit::Apple, "pear"}, {Fruit::Plum, "plum"}}}};
static_assert(!casEnumTableCoversEnum<dup_value, Fruit>());

constexpr EnumWireTable<Fruit, 3> invalid_value{{{
    {Fruit::Apple, "apple"}, {Fruit::Pear, "pear"}, {static_cast<Fruit>(99), "plum"}}}};
static_assert(!casEnumTableCoversEnum<invalid_value, Fruit>());

}
```

If `expectThrowsCode` lives elsewhere or has a different argument order, copy the call shape from any existing `gtest_cas_*` file that uses it.

- [ ] **Step 2: Run to verify it fails** — compile failure: headers do not exist.

- [ ] **Step 3: Implement `CasEnumWireTable.h`:**

```cpp
#pragma once

#include <array>
#include <string_view>

#include <base/types.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}

namespace DB::Cas
{

/// One persisted enum <-> wire-word vocabulary: the single carrier the encoder, the decoder, the
/// introspection renderer, and the tests all read. Every persisted enum is dense, so `toWord` is a
/// direct indexed lookup; `fromWord` is a linear pass over a handful of words. Coverage is proven
/// at each table's definition site by `casEnumTableCoversEnum` (CasEnumWireTableAsserts.h — .cpp
/// and tests only) together with the `denseAndOrdered`/`wordsUnique` predicates below.
template <typename Enum, size_t N>
struct EnumWireTable
{
    struct Entry
    {
        Enum value;
        std::string_view word;
    };

    std::array<Entry, N> entries;

    constexpr bool denseAndOrdered() const
    {
        for (size_t i = 0; i < N; ++i)
            if (static_cast<uint64_t>(entries[i].value) != static_cast<uint64_t>(entries[0].value) + i)
                return false;
        return true;
    }

    constexpr bool wordsUnique() const
    {
        for (size_t i = 0; i < N; ++i)
            for (size_t j = i + 1; j < N; ++j)
                if (entries[i].word == entries[j].word)
                    return false;
        return true;
    }

    std::string_view toWord(Enum value, std::string_view what) const
    {
        const uint64_t index = static_cast<uint64_t>(value) - static_cast<uint64_t>(entries.front().value);
        if (index >= entries.size() || entries[index].value != value)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "{}: value {} is outside the wire vocabulary", what, static_cast<uint64_t>(value));
        return entries[index].word;
    }

    Enum fromWord(std::string_view word, std::string_view what) const
    {
        for (const auto & entry : entries)
            if (entry.word == word)
                return entry.value;
        throw Exception(ErrorCodes::CORRUPTED_DATA, "{}: unknown word '{}'", what, word);
    }
};

}
```

- [ ] **Step 4: Implement `CasEnumWireTableAsserts.h`:**

```cpp
#pragma once

/// Compile-time coverage proof for EnumWireTable: SET EQUALITY with the enum's declared values.
/// Size-plus-uniqueness is not enough (an invalid casted value satisfies both while an enumerator
/// goes missing). This header pulls in magic_enum and therefore MUST be included only from .cpp
/// files and tests, never from another header.

#include <magic_enum.hpp>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTable.h>

namespace DB::Cas
{

template <const auto & Table, typename Enum>
consteval bool casEnumTableCoversEnum()
{
    /// One assert per table carries all three obligations: a table author cannot forget density
    /// or word uniqueness, because coverage subsumes them.
    if (!Table.denseAndOrdered() || !Table.wordsUnique())
        return false;
    constexpr auto declared = magic_enum::enum_values<Enum>();
    if (declared.size() != Table.entries.size())
        return false;
    for (size_t i = 0; i < declared.size(); ++i)
    {
        bool found = false;
        for (const auto & entry : Table.entries)
            if (entry.value == declared[i])
                found = true;
        if (!found)
            return false;
    }
    return true;
}

}
```

If `magic_enum.hpp`'s include path differs in this tree, find it with `grep -rn "include.*magic_enum" src/ | head -3` and copy that form.

- [ ] **Step 5: Run the new tests and the full gate** — green; log `$BUILD/test_cas_task2.log`.

- [ ] **Step 6: Sanctioned taxonomy note (read before every table-conversion task).** The defensive encode branch (`toWord` on a garbage enum value) throws `LOGICAL_ERROR` from the table. Some existing per-codec `*ToWord` switches throw `CORRUPTED_DATA` or `BAD_ARGUMENTS` on that same unreachable branch. Decode behavior (`fromWord` = `CORRUPTED_DATA`) is identical everywhere and MUST NOT change. When converting a vocabulary, grep the tests for the old defensive message text (e.g. `grep -rn "unknown op kind" src/Disks/tests/`); if a test pins the old code on the *encode* branch, update that one expectation in the same commit and say so in the commit message. That is the single sanctioned test change of this plan.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTable.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h \
        src/Disks/tests/gtest_cas_enum_wire_table.cpp
git commit -m "cas: add EnumWireTable with set-equality coverage proofs"
```

---

### Task 3: One compile-time owner for `kMinBlobHeaderLen` {#task-3}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasEnvelopeLimits.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp` (delete the file-local `kMinBlobHeaderLen`, include the new header)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.cpp` (include the new header; no behavior change yet — the budget `static_assert` is phase 2)

**Interfaces:**
- Produces: `namespace DB::Cas { inline constexpr uint64_t kMinBlobHeaderLen = 240; }` in `CasEnvelopeLimits.h`, read by `validatePoolBlobHeaderLen` and (from phase 2 on) by the envelope worst-case formula.

- [ ] **Step 1: Create the header:**

```cpp
#pragma once

#include <cstdint>

namespace DB::Cas
{

/// The pool-wide floor for `blob_header_len`. One compile-time owner, read by BOTH
/// `validatePoolBlobHeaderLen` (pool creation / decode) and the blob-envelope codec, so the
/// mandatory-descriptor worst-case proof and the enforced floor can never guard different numbers.
/// The derivation of the floor lives in `CasPoolMetaFormat.cpp` next to the worst-case table.
inline constexpr uint64_t kMinBlobHeaderLen = 240;

}
```

- [ ] **Step 2: In `CasPoolMetaFormat.cpp`** delete the line `static constexpr uint64_t kMinBlobHeaderLen = 240;` (keeping the derivation comment above it in place), add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasEnvelopeLimits.h>`. Do NOT touch `CasBlobEnvelopeFormat.cpp` in this task — an unused include is dead scaffolding; phase 2 adds the include together with the `static_assert` it exists for.

- [ ] **Step 3: Build + full gate green** (the boundary tests in `gtest_cas_pool.cpp` — floor 240, multiple-of-8, 16 KiB ceiling — pass unchanged). Log `$BUILD/test_cas_task3.log`.

- [ ] **Step 4: Commit** — `git commit -m "cas: give kMinBlobHeaderLen one compile-time owner"` (with the three files staged).

---

### Task 4: Shared vocabulary — enum tables for `TokenType`, `ObjectKind`, `BlobHashAlgo` {#task-4}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h` (the `TokenType`/`ObjectKind` tables live here — no `magic_enum`; asserts go in the `.cpp`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobDigest.h` and `.cpp` — `kBlobHashAlgoWords` is defined HERE, next to `BlobHashAlgo` in `Primitives/` (layering: `Primitives` must not include `Formats/`; `Formats/CasWireVocab` then reads the table in the allowed `Formats → Primitives` direction). Today the words are split between `blobHashAlgoName` here and `blobHashAlgoFromWord` in `CasWireVocab.cpp` — the live drift instance the spec names. The pre-existing violation in `CasTypes.h` is not a precedent.
- Test: `src/Disks/tests/gtest_cas_wire_vocab.cpp`

**Interfaces:**
- Consumes: Task 2's `EnumWireTable`, `casEnumTableCoversEnum`.
- Produces: `inline constexpr EnumWireTable<TokenType, 3> kTokenTypeWords` and `inline constexpr EnumWireTable<ObjectKind, 1> kObjectKindWords` in `CasWireVocab.h`; `inline constexpr EnumWireTable<BlobHashAlgo, 3> kBlobHashAlgoWords` in `Primitives/CasBlobDigest.h`. The existing public word functions (`tokenTypeToWord`/`tokenTypeFromWord`, `objectKindToWord`/`objectKindFromWord`, `blobHashAlgoName`/`blobHashAlgoFromWord`) keep their exact signatures and become one-line delegates — every existing caller, `CasInspect` included, is unified for free.

- [ ] **Step 1: Add a coverage test** in `gtest_cas_wire_vocab.cpp` (fails to compile until the tables exist):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasEnumWireTableAsserts.h>

static_assert(DB::Cas::casEnumTableCoversEnum<DB::Cas::kTokenTypeWords, DB::Cas::TokenType>());
static_assert(DB::Cas::casEnumTableCoversEnum<DB::Cas::kObjectKindWords, DB::Cas::ObjectKind>());
static_assert(DB::Cas::casEnumTableCoversEnum<DB::Cas::kBlobHashAlgoWords, DB::Cas::BlobHashAlgo>());

TEST(CASWireVocab, EnumTablesPinTheCurrentWords)
{
    using namespace DB::Cas;
    EXPECT_EQ(kTokenTypeWords.toWord(TokenType::ETag, "t"), "etag");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(BlobHashAlgo::CityHash128, "t"), "ch128");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(BlobHashAlgo::XXH3_128, "t"), "xxh3");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(BlobHashAlgo::Sha256, "t"), "sha256");
    EXPECT_EQ(kObjectKindWords.toWord(ObjectKind::Blob, "t"), "blob");
}
```

Before writing the table entries, open the current sources and copy the EXACT enumerators and words: `tokenTypeToWord`/`tokenTypeFromWord` (`CasWireVocab.cpp`, words `etag`, `generation`, `emulated`), `objectKindToWord` (`CasWireVocab.cpp:44`, sole word `blob`), `blobHashAlgoName` (`CasBlobDigest.cpp:6`, words `ch128`, `xxh3`, `sha256`) and the enum declarations they switch over. If `TokenType`/`BlobHashAlgo` enumerators are not dense from their first value, STOP and report — the spec's density premise would be wrong and the table needs the offset the enums actually have.

- [ ] **Step 2: Define the tables** — `kTokenTypeWords`/`kObjectKindWords` in `CasWireVocab.h`, `kBlobHashAlgoWords` in `Primitives/CasBlobDigest.h` (entries copied from the switches; shown here with the audited spellings):

```cpp
inline constexpr EnumWireTable<TokenType, 3> kTokenTypeWords{{{
    {TokenType::ETag, "etag"},
    {TokenType::Generation, "generation"},
    {TokenType::Emulated, "emulated"},
}}};

inline constexpr EnumWireTable<ObjectKind, 1> kObjectKindWords{{{
    {ObjectKind::Blob, "blob"},
}}};

inline constexpr EnumWireTable<BlobHashAlgo, 3> kBlobHashAlgoWords{{{
    {BlobHashAlgo::CityHash128, "ch128"},
    {BlobHashAlgo::XXH3_128, "xxh3"},
    {BlobHashAlgo::Sha256, "sha256"},
}}};
```

Adjust enumerator spellings to the real declarations. In `CasWireVocab.cpp`, add `#include .../CasEnumWireTableAsserts.h` and the coverage asserts (`denseAndOrdered`, `wordsUnique`, `casEnumTableCoversEnum`) for `kTokenTypeWords`/`kObjectKindWords`; rewrite `tokenTypeToWord`/`tokenTypeFromWord`/`objectKindToWord`/`objectKindFromWord`/`blobHashAlgoFromWord` as one-line delegates preserving their exact signatures and `what`-style error context. In `CasBlobDigest.cpp`, add the `kBlobHashAlgoWords` coverage assert and make `blobHashAlgoName` return `kBlobHashAlgoWords.toWord(algo, "blobHashAlgoName")` — apply Step 6 of Task 2 if a test pins its old `BAD_ARGUMENTS` defensive branch. Do NOT touch `blobHashAlgoFromConfigValue`-style config parsing (`cityhash128`/`xxh3-128` spellings): the configuration vocabulary is a separate contract and stays where it is.

- [ ] **Step 3: Build + full gate green** — every golden byte-identical. Log `$BUILD/test_cas_task4.log`.

- [ ] **Step 4: Commit** — `git commit -m "cas: move TokenType/ObjectKind/BlobHashAlgo words into EnumWireTable"`.

---

### Task 5: Shared vocabulary — key constants, bundles, and match/build collectors {#task-5}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.cpp`
- Modify (call sites of the changed signatures, same commit): `Formats/CasRefLogFormat.cpp`, `Formats/CasRefSnapshotFormat.cpp`, `Formats/CasPartManifestFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_wire_vocab.cpp`

**Interfaces:**
- Consumes: Task 1's `WireKey`/field helpers, Task 4's tables.
- Produces (codec tasks 6–18 consume):

```cpp
namespace SharedWire
{
    inline constexpr WireKey algo{"ha"};
    inline constexpr WireKey digest{"h"};
    inline constexpr WireKey token_type{"tt"};
    inline constexpr WireKey token{"tv"};
}

struct ManifestRefWireKeys
{
    WireKey epoch;
    WireKey build;
    WireKey ord;
};

inline constexpr ManifestRefWireKeys kBareManifestRefKeys{WireKey{"me"}, WireKey{"mb"}, WireKey{"mo"}};
inline constexpr ManifestRefWireKeys kOldManifestRefKeys{WireKey{"ome"}, WireKey{"omb"}, WireKey{"omo"}};
inline constexpr ManifestRefWireKeys kNewManifestRefKeys{WireKey{"nme"}, WireKey{"nmb"}, WireKey{"nmo"}};

struct BindingWireKeys
{
    WireKey kind;
    WireKey ref;
    ManifestRefWireKeys manifest;
};

inline constexpr BindingWireKeys kOldBindingKeys{WireKey{"obk"}, WireKey{"orn"}, kOldManifestRefKeys};
inline constexpr BindingWireKeys kNewBindingKeys{WireKey{"nbk"}, WireKey{"nrn"}, kNewManifestRefKeys};
```

  (Bundle member names carry the NEW semantic roles; the strings are the OLD spellings — phase 2 flips only the strings.)
  - Signature changes: `writeManifestRefFields(CasJsonWriter &, bool & first, const ManifestRefWireKeys &, const ManifestRef &)` replaces the `std::string_view prefix` parameter; `writeBindingFields(...)` likewise takes `const BindingWireKeys &`. The two-part `CasJsonWriter::key(prefix, name, first)` loses its last user — delete it once call sites are migrated.
  - Collectors with the match-plus-build contract (readers adopt them in Tasks 11–18):

```cpp
struct ManifestRefFields
{
    std::optional<uint64_t> epoch;
    std::optional<uint64_t> build;
    std::optional<uint64_t> ord;

    ManifestRef buildRef(std::string_view what, std::string_view context) const;   /// group requiredness + range checks; CORRUPTED_DATA
};

struct BlobRefFields
{
    std::optional<String> algo_word;
    std::optional<String> digest_hex;

    BlobRef build(std::string_view what) const;   /// requires both; word parse; digest width BEFORE fromHex; CORRUPTED_DATA
};

struct TokenFields
{
    std::optional<String> type_word;
    std::optional<String> value;
};

bool matchManifestRefFields(std::string_view key, JsonObjectReader & r, const ManifestRefWireKeys & keys, ManifestRefFields & fields);
bool matchBlobRefFields(std::string_view key, JsonObjectReader & r, BlobRefFields & fields);
bool matchTokenFields(std::string_view key, JsonObjectReader & r, TokenFields & fields);
```

  Each `match*` consumes exactly one recognized field and returns whether the key was theirs; they never loop and never validate the group. `TokenFields` deliberately has NO `build` in phase 1 — the unified both-required `build` is the phase-2 `GcOutcomes` tightening; phase-1 callers keep their local requiredness checks.
  - `ManifestRefFields::buildRef` reuses the existing `manifestRefFromFields(me, mb, mo, what, context)` logic — move/delegate, do not duplicate. `BlobRefFields::build` centralizes the digest-width-before-`fromHex` check by moving the EXISTING validation code from `CasPartManifestFormat.cpp` (blob entries) — `CasGcOutcomesFormat` adopts it in Task 18.

- [ ] **Step 1: Write failing tests** in `gtest_cas_wire_vocab.cpp` — bundle write equivalence and match/build:

```cpp
TEST(CASWireVocab, ManifestRefBundleWritesTheOldPrefixedKeys)
{
    using namespace DB::Cas;
    CasJsonWriter w;
    bool first = true;
    writeManifestRefFields(w, first, kOldManifestRefKeys, ManifestRef{1, 2, 3});
    w.closeObject(first);
    EXPECT_EQ(std::move(w).take(), R"({"ome":"1","omb":"2","omo":3})");
}

TEST(CASWireVocab, MatchAndBuildRoundTripsABlobRef)
{
    using namespace DB::Cas;
    const String rendered = R"({"ha":"ch128","h":"00112233445566778899aabbccddeeff"})";
    DB::ReadBufferFromMemory in(rendered.data(), rendered.size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    BlobRefFields fields;
    String key;
    while (r.nextKey(key))
    {
        if (matchBlobRefFields(key, r, fields))
            continue;
        r.skipUnknown(key);
    }
    const BlobRef ref = fields.build("t");
    EXPECT_EQ(kBlobHashAlgoWords.toWord(ref.algo, "t"), "ch128");
}

TEST(CASWireVocab, BlobRefBuildFailsClosedOnHalfAGroupAndOnBadWidth)
{
    using namespace DB::Cas;
    BlobRefFields only_algo;
    only_algo.algo_word = "ch128";
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { only_algo.build("t"); });

    BlobRefFields short_digest;
    short_digest.algo_word = "ch128";
    short_digest.digest_hex = "00112233445566778899aabbccddee";   /// 30 hex chars, needs 32
    DB::Cas::tests::expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { short_digest.build("t"); });
}
```

Adapt the exact error-message expectations to what the moved validation code already throws — the messages must not change, since existing corruption tests pin them.

- [ ] **Step 2: Run — compile failure expected.**

- [ ] **Step 3: Implement** the constants, bundles, signature changes, collectors, and match helpers in `CasWireVocab.{h,cpp}` per the Interfaces block. Match helpers are defined `inline` in the header (spec: no added call on hot decode paths). Update the three call sites of the old prefix signatures in the same commit: `CasRefLogFormat.cpp` passes `kOldBindingKeys`/`kNewBindingKeys`/`kBareManifestRefKeys`, `CasRefSnapshotFormat.cpp` and `CasPartManifestFormat.cpp` pass `kBareManifestRefKeys` (their current prefix argument is `""`). Their READERS stay untouched in this task.

- [ ] **Step 4: Build + full gate green** — goldens byte-identical (the bundle strings are the old spellings). Log `$BUILD/test_cas_task5.log`.

- [ ] **Step 5: Commit** — `git commit -m "cas: key bundles and match/build collectors in the shared vocabulary"`.

---

### Task 6: Codec `cas_blob_meta` onto carriers (+ `MetaState` table) {#task-6}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.cpp`
- Test: existing `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (no edits — it is the proof)

**Interfaces:**
- Consumes: Tasks 1, 2. Produces nothing new — this is the template every following codec task copies in spirit (each with its own constants, spelled out per task).

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.

- [ ] **Step 2: Add the carriers** at the top of `CasBlobMetaFormat.cpp` (anonymous namespace):

```cpp
namespace BlobMetaWire
{
    constexpr WireKey state{"st"};
    constexpr WireKey condemn_round{"cr"};
    constexpr WireKey size{"sz"};
}

constexpr EnumWireTable<MetaState, 2> kMetaStateWords{{{
    {MetaState::Clean, "clean"},
    {MetaState::Condemned, "condemned"},
}}};
```

Copy the true `MetaState` enumerators from the file (`metaStateToWord` switch); add the coverage assert with `CasEnumWireTableAsserts.h`; delete `metaStateToWord`/`metaStateFromWord` and route both directions through the table (preserving each direction's error message per the existing corruption tests). Because the old functions were file-local and Task 19 needs the word, declare ONE public narrow delegate in `CasBlobMetaFormat.h` — `std::string_view metaStateToWireWord(MetaState state);` — implemented as the table call; the table itself stays private to the `.cpp`. (Every codec task below whose vocabulary `CasInspect` renders adds the same kind of `<enum>ToWireWord` delegate; each task names its own.)

- [ ] **Step 3: Migrate the writer** — the three `writeKey`+value pairs become:

```cpp
writeWordField(out, BlobMetaWire::state, kMetaStateWords.toWord(meta.state, "CAS blob meta"), first);
writeU64StringField(out, BlobMetaWire::condemn_round, meta.condemn_round, first);
writeU64StringField(out, BlobMetaWire::size, meta.size, first);
```

- [ ] **Step 4: Migrate the reader** — literals become the constants; grammar untouched:

```cpp
if (key == BlobMetaWire::state)
{
    m.state = kMetaStateWords.fromWord(r.readString(), "CAS blob meta");
    saw_state = true;
}
else if (key == BlobMetaWire::condemn_round)
    m.condemn_round = r.readU64String();
else if (key == BlobMetaWire::size)
    m.size = r.readU64String();
else
    r.skipUnknown(key);
```

Note: `fromWord`'s message differs from the old `metaStateFromWord` text — check `FailsClosedOnUnknownStateAndTruncation`-style tests; if they pin only the CODE (`CORRUPTED_DATA` via `EXPECT_THROW`), nothing to do; if they pin the message, keep the old message by throwing from a thin wrapper around the table lookup.

- [ ] **Step 5: Build + full gate green** (goldens prove byte identity). Log `$BUILD/test_cas_task6.log`. **Commit:** `git commit -m "cas: blob-meta codec onto WireKey constants and the MetaState table"`.

---

### Task 7: Codec `cas_pool_meta` onto carriers {#task-7}

**Files:** Modify `Formats/CasPoolMetaFormat.cpp`. Tests: existing (`gtest_cas_format_battery.cpp`, `gtest_cas_pool.cpp`).

Constants (verbatim current spellings) — the CSV `alg` value representation stays EXACTLY as is in phase 1:

```cpp
namespace PoolMetaWire
{
    constexpr WireKey pool_id{"pid"};
    constexpr WireKey blob_header_len{"hln"};
    constexpr WireKey gc_shards{"gcs"};
    constexpr WireKey min_reader_generation{"mrg"};
    constexpr WireKey algos_used{"alg"};
}
```

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2: Writer** — `writeHex128Field(out, PoolMetaWire::pool_id, pm.pool_id, first)`, `writeNumberField` for `blob_header_len`/`gc_shards`/`min_reader_generation`; `algos_used` keeps its current hand-written join, only its `writeKey` call switching to `writeKey(out, PoolMetaWire::algos_used, first)`.
- [ ] **Step 3: Reader** — replace the five key literals with the constants; grammar, CSV split loop, and all error messages untouched.
- [ ] **Step 4: Build + full gate green.** Log `$BUILD/test_cas_task7.log`. **Commit:** `git commit -m "cas: pool-meta codec onto WireKey constants"`.

---

### Task 8: Codecs `cas_gc_state`, `cas_gc_hb`, `cas_gc_maintenance_state` onto carriers {#task-8}

**Files:** Modify `Formats/CasGcStateFormat.cpp`, `Formats/CasGcMaintenanceStateFormat.cpp`. Tests: existing (`gtest_cas_gc_state_format.cpp`, `gtest_cas_gc_maintenance_state_format.cpp`).

Constants:

```cpp
namespace GcStateWire
{
    constexpr WireKey round{"rnd"};
    constexpr WireKey gc_shards{"gcs"};
    constexpr WireKey snap_generation{"sg"};
    constexpr WireKey snap_pruned_through{"spt"};
    constexpr WireKey snap_attempt{"sa"};
    constexpr WireKey manifest_sweep_cursor{"msc"};
    constexpr WireKey lease_owner{"lo"};
    constexpr WireKey lease_seq{"ls"};
}

namespace GcHeartbeatWire
{
    constexpr WireKey owner{"by"};
    constexpr WireKey hb_seq{"seq"};
}

namespace GcMaintenanceWire
{
    constexpr WireKey janitor_cursor{"cur"};
}
```

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2: Writers** — `writeU64StringField` for `round`/`snap_*`/`lease_seq`/`hb_seq`, `writeNumberField` for `gc_shards`, `writeStringField` for the two cursors, `writeHex128Field` for `lease_owner`/`owner`.
- [ ] **Step 3: Readers** — literals → constants; the `saw_gcs` fail-closed check and every message untouched. `GcMaintenanceState` is `Strict` — behavior identical since only spellings of comparisons moved.
- [ ] **Step 4: Build + full gate green.** Log `$BUILD/test_cas_task8.log`. **Commit:** `git commit -m "cas: gc state/heartbeat/maintenance codecs onto WireKey constants"`.

---

### Task 9: Codec `CasServerRootFormats` (`cas_owner`, `cas_epoch`, `cas_mount_lease`) onto carriers {#task-9}

**Files:** Modify `Formats/CasServerRootFormats.cpp`. Tests: existing (`gtest_cas_server_root_format.cpp`).

Constants:

```cpp
namespace OwnerWire
{
    constexpr WireKey server_uuid{"su"};
    constexpr WireKey retired_at_ms{"rt"};
}

namespace ServerEpochWire
{
    constexpr WireKey next_writer_epoch{"nwe"};
}

namespace MountLeaseWire
{
    constexpr WireKey server_uuid{"su"};
    constexpr WireKey writer_epoch{"we"};
    constexpr WireKey hostname{"hn"};
    constexpr WireKey pid{"pid"};
    constexpr WireKey started_at_ms{"sat"};
    constexpr WireKey seq{"seq"};
    constexpr WireKey expires_at_ms{"eat"};
    constexpr WireKey min_active{"ma"};
    constexpr WireKey gc_fenced{"fen"};
    constexpr WireKey write_attempt_id{"write_attempt_id"};
}
```

(`MountLease::min_active` the MEMBER is NOT renamed here — `min_active_build_sequence` tracks the wire cut in phase 2.)

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2: Writers** — owner: `writeHex128Field` + conditional `writeNumberField(…, retired_at_ms, …)` keeping the `if (o.retired_at_ms)` guard; epoch: `writeU64StringField`; mount lease: `writeHex128Field`×2 (`server_uuid`, `write_attempt_id`), `writeU64StringField`×3 (`writer_epoch`, `seq`, `min_active`), `writeStringField` (`hostname`), `writeNumberField`×3 (`pid`, `started_at_ms`, `expires_at_ms`), `writeBoolField` (`gc_fenced`).
- [ ] **Step 3: Readers** — literals → constants; required-field checks (`su`, `we`, nonzero `write_attempt_id`) untouched.
- [ ] **Step 4: Build + full gate green.** Log `$BUILD/test_cas_task9.log`. **Commit:** `git commit -m "cas: server-root codecs onto WireKey constants"`.

---

### Task 10: Codec `cas_blob` envelope onto carriers (+ `ProvenanceOp` table) {#task-10}

**Files:** Modify `Formats/CasBlobEnvelopeFormat.cpp`. Tests: existing (`gtest_cas_blob_envelope_format.cpp`, `gtest_cas_envelope.cpp`).

Constants and table (words from the two switches this task deletes — `opToWord`/`opFromWord`):

```cpp
namespace EnvelopeWire
{
    constexpr WireKey type{"type"};
    constexpr WireKey version{"v"};
    constexpr WireKey tag{"tag"};
    constexpr WireKey build{"bld"};
    constexpr WireKey time_ms{"ts"};
    constexpr WireKey creator{"by"};
    constexpr WireKey op{"op"};
    constexpr WireKey chver{"ch"};
    constexpr WireKey ref{"ref"};
}

constexpr EnumWireTable<ProvenanceOp, 6> kProvenanceOpWords{{{
    {ProvenanceOp::Other, "other"},
    {ProvenanceOp::Insert, "insert"},
    {ProvenanceOp::Merge, "merge"},
    {ProvenanceOp::Mutation, "mutation"},
    {ProvenanceOp::Attach, "attach"},
    {ProvenanceOp::Repack, "repack"},
}}};
```

(Bundle member names already carry phase-2 semantics: `build` holds `"bld"`, `time_ms` holds `"ts"`, and so on.)

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** Add the table + coverage assert; delete `opToWord`/`opFromWord`, routing encode through `kProvenanceOpWords.toWord(op, "CAS blob envelope")` and decode through `fromWord` — the decode message today is `CAS blob envelope: unknown operation '{}'` with `CORRUPTED_DATA`; keep code and check no test pins the exact text (apply Task 2 Step 6 if the encode branch is pinned). Declare the public narrow delegate `std::string_view provenanceOpToWireWord(ProvenanceOp op);` in `CasBlobEnvelopeFormat.h` (table stays private) — Task 19 consumes it.
- [ ] **Step 3:** Migrate `encodeEnvelopeHeader`'s field writes onto the constants (the `writeKey(buf, "...", first)` calls take `EnvelopeWire::…`). The truncated-`ref` writer (`writeEnvelopeRefField`, its escaper, and the `,\"ref\":` budget arithmetic) is CODEC-OWNED per the spec — do not convert it to field helpers; only its key spelling may read from `EnvelopeWire::ref.text`. Migrate `decodeEnvelopeHeader`'s key comparisons to the constants. The test-only `"!x"` literal stays a literal.
- [ ] **Step 4: Build + full gate green.** Log `$BUILD/test_cas_task10.log`. **Commit:** `git commit -m "cas: blob envelope onto WireKey constants and the ProvenanceOp table"`.

---

### Task 11: Codec `cas_ref_log` onto carriers (+ `RefOpKind`/`RefOwnerKind` tables, `BindingFields` renames) {#task-11}

**Files:** Modify `Formats/CasRefLogFormat.cpp`. Tests: existing (`gtest_cas_ref_log_format.cpp`, `gtest_cas_ref_epoch_seal_format.cpp`, `gtest_cas_encoding_pins.cpp`).

Constants and tables:

```cpp
namespace RefLogWire
{
    constexpr WireKey ns{"ns"};
    constexpr WireKey txn_epoch{"we"};
    constexpr WireKey txn_seq{"rs"};
    constexpr WireKey prev_epoch{"!pse"};
    constexpr WireKey prev_seq{"!pss"};
    constexpr WireKey op{"op"};
    constexpr WireKey ref{"rn"};
    constexpr WireKey published_ms{"ts"};
}

constexpr EnumWireTable<RefOpKind, 5> kRefOpWords{{{
    {RefOpKind::NamespaceBirth, "namespace_birth"},
    {RefOpKind::OwnerTransition, "owner_transition"},
    {RefOpKind::SetPublishedAt, "set_published_at"},
    {RefOpKind::RemoveNamespace, "remove_namespace"},
    {RefOpKind::EpochSeal, "epoch_seal"},
}}};

constexpr EnumWireTable<RefOwnerKind, 2> kRefOwnerKindWords{{{
    {RefOwnerKind::Committed, "committed"},
    {RefOwnerKind::Precommit, "precommit"},
}}};
```

Copy true enumerator spellings from `RefOpKind`/`RefOwnerKind` declarations; check density (if `RefOwnerKind` starts at a nonzero value the table still works — `denseAndOrdered` is offset-based). `kRefOwnerKindWords` may belong in `CasRefWireVocab` if `refOwnerKindToWord` lives there — put the table where the function it replaces lives.

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** Replace `opKindToWord`/`opKindFromWord` and `refOwnerKindToWord`/`refOwnerKindFromWord` with table delegates (decode messages preserved; Task 2 Step 6 for the encode branch, whose current text is `RefLogTxn: unknown operation kind {}` with `CORRUPTED_DATA`). Where the replaced `toWord` was file-local, declare public narrow delegates in the owning header — `refOpKindToWireWord` in `CasRefLogFormat.h`; `refOwnerKindToWord` keeps whatever public home it has today (`CasRefWireVocab`).
- [ ] **Step 3:** Rename the collector members — `ManifestFields` local struct is REPLACED by the shared `ManifestRefFields` from Task 5 (members `epoch`/`build`/`ord`, method `buildRef`); `BindingFields` members `bk`/`rn`/`mf` become `kind`/`ref`/`manifest_fields` (type of `manifest_fields` = shared `ManifestRefFields`). Update `readOpRecord`: the fifteen key comparisons read the constants and bundles (`kOldBindingKeys.kind`, `kOldBindingKeys.manifest.epoch`, `kBareManifestRefKeys.epoch`, `RefLogWire::published_ms`, …); adopt `matchManifestRefFields` for the three bare-triple keys. The `"pl"`-sentinel branch keeps its literal `"pl"` — sentinels are not live vocabulary and get no constant. `writeLogMeta`/`writeOp` writes go through the constants and Task 5's bundle-taking writers.
- [ ] **Step 4: Build + full gate green** — `CASEncodingPins.RefLogTxnAllOpKinds` byte-identical is the hard proof. Log `$BUILD/test_cas_task11.log`. **Commit:** `git commit -m "cas: ref-log codec onto carriers; shared ManifestRefFields; BindingFields renamed"`.

---

### Task 12: Codec `cas_ref_snap` onto carriers {#task-12}

**Files:** Modify `Formats/CasRefSnapshotFormat.cpp`. Tests: existing (`gtest_cas_ref_snapshot_format.cpp`, `gtest_cas_encoding_pins.cpp`).

Constants:

```cpp
namespace RefSnapWire
{
    constexpr WireKey ns{"ns"};
    constexpr WireKey snapshot_epoch{"we"};
    constexpr WireKey snapshot_seq{"rs"};
    constexpr WireKey lifecycle{"lc"};
    constexpr WireKey kind{"k"};
    constexpr WireKey ref{"rn"};
    constexpr WireKey published_ms{"ts"};
}
```

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** Writers (`writeCommittedRow`, `writePrecommitRow`, `writeSnapshotMeta`) onto constants/field helpers; row tags `"c"`/`"p"` become local `constexpr std::string_view kCommittedTag = "c"; kPrecommitTag = "p";` (tag VALUES are words, not keys — they stay `string_view`, and phase 2 flips these two strings). Reader: local `ManifestFields` replaced by shared `ManifestRefFields` + `matchManifestRefFields(key, r, kBareManifestRefKeys, fields)`; the `lc == "live"` hard check, the `rte`/`rts`/`pl` sentinel literals, and both row-variant requiredness checks stay exactly as written.
- [ ] **Step 3: Build + full gate green** (`CASEncodingPins.RefSnapshotLive` byte-identical). Log `$BUILD/test_cas_task12.log`. **Commit:** `git commit -m "cas: ref-snapshot codec onto carriers"`.

---

### Task 13: Codec `cas_ref_ckpt` onto carriers {#task-13}

**Files:** Modify `Formats/CasRefCkptFormat.cpp`. Tests: existing (`gtest_cas_ref_ckpt.cpp`).

```cpp
namespace RefCkptWire
{
    constexpr WireKey life_epoch{"le"};
    constexpr WireKey committed_epoch{"cte"};
    constexpr WireKey committed_seq{"cts"};
    constexpr WireKey snapshot_epoch{"cse"};
    constexpr WireKey snapshot_seq{"css"};
    constexpr WireKey seal_epoch{"lse"};
    constexpr WireKey seal_seq{"lss"};
}
```

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** The `writeRefTxnIdFields(out, first, "cte", "cts", …)` calls pass `RefCkptWire::committed_epoch.text, RefCkptWire::committed_seq.text` (or overload `writeRefTxnIdFields` for `WireKey` pairs — prefer the overload, matching Task 1's pattern). Reader comparisons onto the constants; every both-or-neither check and message untouched. This is a Strict, trailer-less codec — nothing else moves.
- [ ] **Step 3: Build + full gate green** (`CommittedThroughHasCanonicalExactWireEncoding` byte-identical). Log `$BUILD/test_cas_task13.log`. **Commit:** `git commit -m "cas: ref-ckpt codec onto carriers"`.

---

### Task 14: Codec `cas_ref_catalog` onto carriers (+ `NsState` table) {#task-14}

**Files:** Modify `Formats/CasRefCatalogFormat.cpp`. Tests: existing (`gtest_cas_ref_catalog.cpp`).

```cpp
namespace RefCatalogWire
{
    constexpr WireKey kind{"k"};
    constexpr WireKey ns{"ns"};
    constexpr WireKey state{"st"};
    constexpr WireKey life{"inc"};
    constexpr WireKey remove_round{"rsr"};
    constexpr WireKey creator{"csr"};
    constexpr WireKey creator_epoch{"cwe"};
    constexpr WireKey creator_fence{"cfg"};
}

constexpr EnumWireTable<NsState, 3> kNsStateWords{{{
    {NsState::Creating, "creating"},
    {NsState::Live, "live"},
    {NsState::Removing, "removing"},
}}};
```

Record tag: local `constexpr std::string_view kEntryTag = "ent";`.

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** `nsStateToWord`/`nsStateFromWord` → table delegates (decode message preserved); writer and Strict reader onto constants; the creator/state and removal/state pairing rules, the empty-namespace rejection, and the zero-`inc` rejection untouched.
- [ ] **Step 3: Build + full gate green.** Log `$BUILD/test_cas_task14.log`. **Commit:** `git commit -m "cas: ref-catalog codec onto carriers and the NsState table"`.

---

### Task 15: Codec `cas_part_manifest` onto carriers (+ `EntryPlacement` table, `BlobRefFields` adoption) {#task-15}

**Files:** Modify `Formats/CasPartManifestFormat.cpp`. Tests: existing (`gtest_cas_part_manifest_format.cpp`).

```cpp
namespace PartManifestWire
{
    constexpr WireKey ns{"ns"};
    constexpr WireKey payload_digest{"pd"};
    constexpr WireKey path{"p"};
    constexpr WireKey place{"pm"};
    constexpr WireKey size{"sz"};
    constexpr WireKey inline_size{"il"};
}

constexpr EnumWireTable<EntryPlacement, 2> kEntryPlacementWords{{{
    {EntryPlacement::Inline, "inline"},
    {EntryPlacement::Blob, "blob"},
}}};
```

(`size` and `inline_size` remain two constants in phase 1 because the OLD wire has two spellings; phase 2 collapses both to `"size"` — two constants, same flip discipline.)

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** `placementToWord`/`placementFromWord` → table delegates, plus the public narrow delegate `entryPlacementToWireWord` in `CasPartManifestFormat.h` (table private; Task 19 consumes it); writer onto constants/helpers (blob entries: `writeBlobRefFields` then `writeNumberField(out, PartManifestWire::size, e.blob_size, first)`; inline: `writeNumberField(out, PartManifestWire::inline_size, …)`). Reader: adopt `matchBlobRefFields` + `BlobRefFields::build` — the digest-width-before-`fromHex` code MOVES into `build` (Task 5 defined it); the placement-vs-fields cross-checks (`blob` requires the group + `sz`, `inline` requires `il`) stay in the codec grammar. Banner code (`bannerFor`, `il=` literal) untouched — payload zones are codec-owned.
- [ ] **Step 3: Build + full gate green.** Log `$BUILD/test_cas_task15.log`. **Commit:** `git commit -m "cas: part-manifest codec onto carriers; digest-width check centralized"`.

---

### Task 16: Codec `cas_run` onto carriers (+ `RunMarker` typed enum, `RunRef::key_generation` rename) {#task-16}

**Files:** Modify `Formats/CasRecordStreamFormat.h`, `Formats/CasRecordStreamFormat.cpp`, `Formats/CasFoldSealFormat.h` (the `RunRef` member), `Gc/CasBlobInDegree.h`/`.cpp` and `Gc/CasGc.cpp` (marker-byte and `generation` call sites — find them all with `grep -rn "kEdgeActive\|kZeroMarker\|kCondemned\|\.generation" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -v Formats/CasFormat`). Tests: existing (`gtest_cas_record_stream_format.cpp`, `gtest_cas_encoding_pins.cpp`, fold-seal/GC suites for the rename fallout).

```cpp
enum class RunMarker : char
{
    Zero = 0x00,
    Edge = 0x01,
    Condemned = 0x02,
};

namespace RunWire
{
    constexpr WireKey ref{"b"};
    constexpr WireKey src{"s"};
    constexpr WireKey mark{"m"};
    constexpr WireKey pending{"pend"};
    constexpr WireKey size{"sz"};
    constexpr WireKey condemn_round{"cr"};
    constexpr WireKey confirmed{"mc"};
}

constexpr EnumWireTable<RunMarker, 3> kRunMarkerWords{{{
    {RunMarker::Zero, "zero"},
    {RunMarker::Edge, "edge"},
    {RunMarker::Condemned, "condemned"},
}}};
```

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2: `RunMarker` enum with a checked byte boundary.** Replace the three `constexpr char` markers with the enum above (same header position, byte values pinned — they persist in the in-degree payload representation). The raw-byte boundary gets exactly two functions in `CasRecordStreamFormat.h`; a bare `static_cast<RunMarker>(byte)` is forbidden, because it lets a corrupted persisted payload byte become an invalid enum value whose later `toWord` throws `LOGICAL_ERROR` where the input deserves `CORRUPTED_DATA`:

```cpp
constexpr char runMarkerByte(RunMarker marker)
{
    return static_cast<char>(marker);
}

inline RunMarker runMarkerFromByte(char byte, std::string_view what)
{
    if (byte != runMarkerByte(RunMarker::Zero) && byte != runMarkerByte(RunMarker::Edge)
        && byte != runMarkerByte(RunMarker::Condemned))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "{}: unknown marker byte {}", what, static_cast<int>(byte));
    return static_cast<RunMarker>(byte);
}
```

`SourceEdgeRecord::marker` becomes `RunMarker`; every site found by the grep uses `runMarkerByte` to store a raw byte and `runMarkerFromByte` to read one — only a value that passed `runMarkerFromByte` may reach `kRunMarkerWords`. Add a negative test: an in-degree payload with an unknown marker byte (e.g. `0x03`) is rejected with `CORRUPTED_DATA` (place it next to the existing decode-corruption tests of the payload it corrupts). `markerToWord`/`markerFromWord` → `kRunMarkerWords` delegates (+ coverage assert; `magic_enum` handles a `char`-based enum), plus the public narrow delegate `runMarkerToWireWord` in `CasRecordStreamFormat.h` for Task 19.
- [ ] **Step 3: Member rename** `RunRef::generation` → `RunRef::key_generation` (declaration in `CasFoldSealFormat.h`; update every `.generation` use of `RunRef` — the wire key `"gen"` stays untouched, it is written in `CasFoldSealFormat.cpp` and migrates in Task 17).
- [ ] **Step 4:** Writer/reader onto the constants; `matchTokenFields` adopted for `tt`/`tv` with the LOCAL condemned-row requiredness check kept exactly as is (`have_tt`, `have_tv` sextet logic — phase 2 owns any unification); the condemned/active variant exclusivity checks untouched. The header line writer keeps its literal `"type"`/`"v"`/`"kind"` framing (framing is `CasTextFormat`'s, not this plan's).
- [ ] **Step 5: Build + full gate green** (`CASEncodingPins.SourceEdgeRunLines` byte-identical proves the enum swap changed nothing on the wire). Log `$BUILD/test_cas_task16.log`. **Commit:** `git commit -m "cas: run codec onto carriers; RunMarker typed enum; RunRef::key_generation"`.

---

### Task 17: Codec `cas_fold_seal` onto carriers (+ `HoldReason` table) {#task-17}

**Files:** Modify `Formats/CasFoldSealFormat.cpp`. Tests: existing (`gtest_cas_fold_seal_format.cpp`, `gtest_cas_gc_hold_grammar.cpp`).

```cpp
namespace FoldSealWire
{
    constexpr WireKey generation{"g"};
    constexpr WireKey parent_generation{"pg"};
    constexpr WireKey kind{"k"};
    constexpr WireKey run_key{"key"};
    constexpr WireKey checksum{"ck"};
    constexpr WireKey shard{"shard"};
    constexpr WireKey key_generation{"gen"};
    constexpr WireKey life{"life"};
    constexpr WireKey classification{"cls"};
    constexpr WireKey fold_epoch{"lfe"};
    constexpr WireKey fold_seq{"lfs"};
    constexpr WireKey hold_reason{"hr"};
    constexpr WireKey hold_epoch{"hpe"};
    constexpr WireKey hold_seq{"hps"};
    constexpr WireKey retries{"hrc"};
    constexpr WireKey retry_round{"hnr"};
    constexpr WireKey remove_epoch{"rte"};
    constexpr WireKey remove_seq{"rts"};
    constexpr WireKey condemned_total{"ct"};
    constexpr WireKey pending_total{"pt"};
    constexpr WireKey oldest_round{"ocr"};
}
```

Record tags: `constexpr std::string_view kRefLifeTag = "rfl"; kBlobRunTag = "btr"; kCondemnedTag = "cnd";`. HoldReason table (words from `holdReasonToWord` — `gap_below_witness`, `unconsumed_seal_crossing`, `witness_disappeared`, `body_undecodable`, `manifest_body_missing`, `checkpoint_undecodable`; copy true enumerators, add coverage assert). The numeric `cls` value stays a number in phase 1 (`CoverageClass` words are phase 2).

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** Writers (`writeRun`, the `rfl` block, the `cnd` block, the meta line) onto constants/helpers (note `RunRef::key_generation` from Task 16 feeds the `"gen"` key here); Strict readers onto constants — the closed-set `cls` validation, hold grammar (iff classification 4), cleanup-evidence rules, shard totals, and every message untouched.
- [ ] **Step 3: Build + full gate green** (hold-grammar suite is the sharpest watchdog here). Log `$BUILD/test_cas_task17.log`. **Commit:** `git commit -m "cas: fold-seal codec onto carriers and the HoldReason table"`.

---

### Task 18: Codec `cas_gc_outcomes` onto carriers (+ `OutcomeKind` table) {#task-18}

**Files:** Modify `Formats/CasGcOutcomesFormat.cpp`. Tests: existing (`gtest_cas_gc_outcomes_format.cpp`).

```cpp
namespace GcOutcomesWire
{
    constexpr WireKey kind{"k"};
    constexpr WireKey outcome{"oc"};
}

constexpr EnumWireTable<OutcomeKind, 4> kOutcomeKindWords{{{
    {OutcomeKind::Deleted, "deleted"},
    {OutcomeKind::Absent, "absent"},
    {OutcomeKind::Replaced, "replaced"},
    {OutcomeKind::Spared, "spared"},
}}};
```

- [ ] **Step 1: Baseline.** The previous task's post-gate is this task's baseline — do not re-run the full gate here; run it only if the tree changed outside this plan.
- [ ] **Step 2:** `outcomeKindToWord`/`outcomeKindFromWord` → table delegates, plus the public narrow delegate `outcomeKindToWireWord` in `CasGcOutcomesFormat.h` if the old function was file-local (table private; Task 19 consumes it); writer onto constants (`writeBlobRefFields`/`writeTokenFields` calls unchanged); reader adopts `matchBlobRefFields` + `BlobRefFields::build` and `matchTokenFields` — **requiredness stays EXACTLY today's**: `ha`/`h`/`tt` required, missing `tv` reads as an empty token value (the both-required tightening is phase 2's own change with its own negative test; do not sneak it in here). The `have_ha || have_h || have_tt` check text stays.
- [ ] **Step 3: Build + full gate green.** Log `$BUILD/test_cas_task18.log`. **Commit:** `git commit -m "cas: gc-outcomes codec onto carriers; requiredness unchanged"`.

---

### Task 19: `CasInspect` renders enum words through the tables {#task-19}

**Files:** Modify `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.cpp`; modify whichever `CAS*` unit tests pin inspect output — find them ALL with `grep -rln "renderMountLease\|CasInspect\|inspect" src/Disks/tests/gtest_cas_*.cpp` and by grepping the tests for the old non-wire words the sweep below uncovers (candidates to check explicitly: `gtest_cas_inspect.cpp`, `gtest_cas_observability.cpp`, `gtest_cas_blob_meta_format.cpp` — whichever of these exist in the tree). The `05010`/`05012` stateless tests are NOT run here — unit gate only; if this task changes a word they assert, note it in the commit message for the phase-2 stateless sweep.

This task is the second sanctioned non-wire change (Global Constraints): inspect output is
introspection, not persisted wire, and it is NORMALIZED here onto the wire words.

- [ ] **Step 1:** `grep -n "switch\|case \|\"" Tools/CasInspect.cpp` — list every place it renders a persisted enum with its own words. For each: call the public `*ToWireWord`/`*ToWord` delegate the owning codec task declared (Tasks 4–18); delete the parallel switch.
- [ ] **Step 2:** For every site whose printed word CHANGES (e.g. `Merge` → `merge`, `Inline` → `inline`): update the pinned assertions in the test files found above, and where no assertion pinned that word before, ADD one exact assertion pinning the new word — every normalized word ends this task pinned. List each old → new word in the commit message.
- [ ] **Step 3: Build + full gate green.** Log `$BUILD/test_cas_task19.log`. **Commit:** `git commit -m "cas: CasInspect renders enum values through the wire tables"`.

---

### Task 20: Battery closure and registry set-equality {#task-20}

**Files:**
- Modify: `Formats/CasFormat.h`/`CasFormat.cpp` (export a registry enumeration accessor — `TRAITS` is in an anonymous namespace today, so nothing outside the TU can iterate it)
- Modify: `src/Disks/tests/gtest_cas_ref_ckpt.cpp`, `src/Disks/tests/gtest_cas_gc_maintenance_state_format.cpp`, `src/Disks/tests/gtest_cas_record_stream_format.cpp` (add battery rows)
- Modify: `src/Disks/tests/gtest_cas_text_format.cpp` (set-equality assertion)

**Interfaces:**
- Produces: `std::span<const FormatId> allRegisteredFormatIds()` declared in `CasFormat.h` and implemented in `CasFormat.cpp` as a PROJECTION of `TRAITS` — never a hand-maintained second list:

```cpp
std::span<const FormatId> allRegisteredFormatIds()
{
    static const auto ids = []
    {
        std::array<FormatId, std::size(TRAITS)> out{};
        for (size_t i = 0; i < std::size(TRAITS); ++i)
            out[i] = TRAITS[i].id;
        return out;
    }();
    return ids;
}
```

- Produces: a battery-coverage registrar in `cas_format_test_battery.h` — coverage is declared statically NEXT TO each battery test, never recorded dynamically inside `runFormatBattery` (dynamic recording would make the set-equality test depend on test execution order):

```cpp
namespace DB::Cas::tests
{
std::set<FormatId> & batteryCoveredIds();   /// function-local static; test-only, std::set is fine here

struct BatteryCoverageRegistrar
{
    explicit BatteryCoverageRegistrar(FormatId id) { batteryCoveredIds().insert(id); }
};
}

#define CAS_BATTERY_COVERS(format_id) \
    static const DB::Cas::tests::BatteryCoverageRegistrar battery_covers_##format_id{DB::Cas::FormatId::format_id}
```

- [ ] **Step 1: Add the three missing battery rows.** Each needs `{id, encode, decode, golden}` with a LITERAL golden (never built from the constants — spec rule). RefCkpt (reuse the exact bytes already pinned by `CommittedThroughHasCanonicalExactWireEncoding`):

```cpp
TEST(CASFormatBattery, RefCkpt)
{
    RefCkpt ckpt{.life_epoch = std::optional<uint64_t>{7},
                 .committed_through = RefTxnId{9, 11},
                 .checkpoint_snapshot_id = RefTxnId{9, 10},
                 .last_epoch_seal = RefTxnId{8, 12}};
    runFormatBattery({FormatId::RefCkpt,
        [&] { return sealObject(FormatId::RefCkpt, encodeRefCkpt(ckpt)); },
        [](std::string_view s) { decodeRefCkpt(std::string(openObject(FormatId::RefCkpt, s))); },
        currentFormatHeader("cas_ref_ckpt") +
        "{\"le\":\"7\",\"cte\":\"9\",\"cts\":\"11\",\"cse\":\"9\",\"css\":\"10\",\"lse\":\"8\",\"lss\":\"12\"}\n"});
}
```

GcMaintenanceState golden: `currentFormatHeader("cas_gc_maintenance_state") + "{\"cur\":\"cas/ns/a\"}\n"` with a matching `GcMaintenanceState{.janitor_cursor = "cas/ns/a"}`. RunFile golden: header `{"type":"cas_run","v":<current>,"kind":"source_edge"}\n` + one edge record + `{"n":1}\n` — copy the exact record line from `CASEncodingPins.SourceEdgeRunLines`; if `runFormatBattery`'s v+1 gate needs the run header's three-key shape, pass the same `make_future_version` hook style the envelope battery uses (see `blobEnvelopeWithFutureVersion` for the pattern; the run header has no pad, so a plain replace works).

- [ ] **Step 2: Set-equality assertion** in `gtest_cas_text_format.cpp`:

```cpp
TEST(CASFormatBattery, EveryRegisteredFormatIsBatteryCovered)
{
    std::set<FormatId> registered;
    for (FormatId id : allRegisteredFormatIds())
        registered.insert(id);
    EXPECT_EQ(registered, DB::Cas::tests::batteryCoveredIds())
        << "a registered codec is missing from the common battery (or vice versa)";
}
```

Coverage exists before ANY test runs because every registrar is a namespace-scope static. Place one
`CAS_BATTERY_COVERS(<FormatId>)` line immediately next to EVERY `runFormatBattery` call — the
complete list of files (15 existing calls across 12 files, plus the 3 rows this task adds):

| File | Registrar lines |
|---|---|
| `gtest_cas_blob_envelope_format.cpp` | `Blob` |
| `gtest_cas_blob_meta_format.cpp` | `BlobMeta` |
| `gtest_cas_format_battery.cpp` | `PoolMeta` |
| `gtest_cas_ref_log_format.cpp` | `RefLog` |
| `gtest_cas_ref_epoch_seal_format.cpp` | `RefLog` (second call site — `std::set` dedups; give the registrar variable a distinct name, e.g. suffix `_seal`) |
| `gtest_cas_ref_snapshot_format.cpp` | `RefSnapshot` |
| `gtest_cas_ref_catalog.cpp` | `RefCatalog` |
| `gtest_cas_part_manifest_format.cpp` | `PartManifest` |
| `gtest_cas_fold_seal_format.cpp` | `FoldSeal` |
| `gtest_cas_gc_state_format.cpp` | `GcState`, `GcHeartbeat` |
| `gtest_cas_gc_outcomes_format.cpp` | `GcOutcomes` |
| `gtest_cas_server_root_format.cpp` | `Owner`, `ServerEpoch`, `MountLease` |
| `gtest_cas_ref_ckpt.cpp` (new row, Step 1) | `RefCkpt` |
| `gtest_cas_gc_maintenance_state_format.cpp` (new row, Step 1) | `GcMaintenanceState` |
| `gtest_cas_record_stream_format.cpp` (new row, Step 1) | `RunFile` |

If the grep `grep -rn runFormatBattery src/Disks/tests/` finds a call site this table misses, add
its registrar too — the set-equality test is the backstop either way.

- [ ] **Step 3: Build + full gate green.** Log `$BUILD/test_cas_task20.log`. **Commit:** `git commit -m "cas: close the format battery and assert registry set-equality"`.

---

### Task 21: Phase-1 gate {#task-21}

**Files:** none created; this task is verification + the phase boundary record.

- [ ] **Step 1: Full gate** — `--gtest_filter='CAS*'` green, logged to `$BUILD/test_cas_phase1_gate.log`.
- [ ] **Step 2: Golden immutability audit** (a per-file review, not a one-liner). With `<base>` = the
  commit before Task 1:
  1. `git diff --name-only <base>..HEAD -- src/Disks/tests/` — the changed-file set must be a
     subset of: new files (`gtest_cas_enum_wire_table.cpp`), the battery/registrar files Task 20
     names, the inspect-assertion files Task 19 names, and any file named in a Task-2-Step-6
     commit message.
  2. For each changed EXISTING file, open `git diff <base>..HEAD -- <file>` and verify hunk by hunk
     that no expected wire literal changed: additions of NEW assertions and registrar lines are
     fine; any modified byte inside a pre-existing expected string is a wire regression unless its
     commit message sanctioned it (Task 2 Step 6 defensive code, Task 19 inspect word). Record a
     one-line verdict per file in the task report — the audit is only done when every file has one.
- [ ] **Step 3: Carrier-completeness sweeps** (writer AND reader; run each, expect empty output
  outside the allowlist):
  1. Writer keys, any buffer variable, line breaks tolerated:
     `rg -Un 'writeKey\(\s*\w+,\s*"' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/`
     — allowlist: `CasTextFormat.{h,cpp}` framing (`type`/`v`/`n`), the run-header framing
     (`type`/`v`/`kind`) in `CasRecordStreamFormat.cpp`, and the test-only `"!x"` envelope line.
  2. The deleted two-part writer must not have crept back:
     `rg -n '\.key\(' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/` —
     allowlist: `CasTextFormat.{h,cpp}` internals only.
  3. Reader keys compared against literals:
     `rg -n 'key == "' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/` —
     allowlist: the known-field sentinels (`"pl"`, `"rte"`, `"rts"`), framing in
     `CasTextFormat.cpp`/`CasRecordStreamFormat.cpp`, and the test-only `"!x"`. Record-TAG value
     comparisons (`kind == "rfl"`-style, where the variable is a decoded VALUE, not the key) are
     out of scope — but eyeball every hit before allowlisting it.
  4. Any other hit in sweeps 1–3 is an unmigrated call site: migrate it and re-run the gate.
- [ ] **Step 4: Commit anything outstanding**, then report: phase 1 complete; the phase-2 plan (`...-cas-wire-keys-phase2-cut.md`) can now be written against the landed carrier names.

---

## Self-Review (performed at write time) {#self-review}

- **Spec coverage (phase-1 scope):** WireKey+helpers (T1), EnumWireTable+proofs (T2), envelope-limits owner (T3), shared tables incl. the `BlobHashAlgo` split unification (T4), bundles + prefix-API removal + match/build with the phase-2 boundary respected (T5), all 17 registered formats' codecs onto carriers (T6–T18 cover the 14 codec files behind them), introspection unification (T19), battery closure + set-equality with the anonymous-namespace export the spec flags (T20), gate + golden-immutability + completeness sweeps (T21). Deliberately OUT (phase 2, recorded in header): every new spelling, generation reset, sentinel/test churn, `static_assert` budget, `writeWordArrayField`/array, `class` words, tightenings, `min_active_build_sequence`. Deliberately OUT (phase 3): measurements, assembly review, ca-soak sweep.
- **Placeholder scan:** no TBDs; every codec task carries its own verbatim constants; the two "copy from the file" instructions are verification directives (spellings must come from the tree), not gaps.
- **Type consistency:** `WireKey{text}` + `operator==(string_view, WireKey)` used uniformly; `EnumWireTable<E,N>` methods `toWord(value, what)`/`fromWord(word, what)` uniform; bundles `ManifestRefWireKeys{epoch,build,ord}` / `BindingWireKeys{kind,ref,manifest}` consistent between T5 and T11–T12; collectors `ManifestRefFields::buildRef`, `BlobRefFields::build`, `TokenFields` (no build in phase 1) consistent across T5/T11/T15/T16/T18.
