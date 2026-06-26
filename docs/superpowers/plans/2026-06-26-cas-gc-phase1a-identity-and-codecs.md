---
description: "Identity, on-wire codecs (PartManifest, RunFile), and layout foundation for the CAS GC redesign."
sidebar_label: "GC redesign — Phase 1a (codecs)"
sidebar_position: 3
slug: /superpowers/plans/2026-06-26-cas-gc-phase1a-identity-and-codecs
title: "Phase 1a - Identity, Codecs, and Layout - Implementation Plan"
doc_type: reference
---

# Phase 1a - Identity, Codecs, and Layout - Implementation Plan {#phase-1a-identity-codecs-and-layout-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax. Each step is one 2-5 minute action. Read `2026-06-26-cas-gc-redesign-overview.md` first, then this. This phase ships **only** the identity + on-wire formats + layout foundation; it changes **no** production behavior and consumes no GC/build/read code paths (those are Phases 1b/1c/1d).

**Goal:** Land the canonical identity types (`ManifestRef`/`ManifestId`), the on-wire formats (`PartManifest` codec, dense block-framed `RunFile` runs + k-way `RunMerger`), the body-validation helpers (`refMatchesBody`/`manifestNamespaceMatches`), and the `CasLayout` manifest key + `_manifests` reservation. Every name this plan emits is consumed verbatim by Phases 1b/1c/1d (see [Canonical Contract](#canonical-contract)).

**Gate (precondition, not a task here):** The Phase 0 suite (`CaGcRootLocalPartManifestCore` stages + `live` + 22 `_sab_*` + witnesses) must be **GREEN** before any step below begins. This is stated by the overview's [Execution Model & Gates]; Phase 1a does not re-run TLA+.

**Architecture:** New header-only `CasManifestId.h` (identity, ordering, `manifestAa`); new `CasManifestCodec.{h,cpp}` (`PartManifest`/`ManifestEntry` deterministic encode/decode with block-framed entries); new `CasRunFile.{h,cpp}` (the `RunFile`/`DataBlock`/`RunFooter` writer/reader + range-seek + `RunMerger`); extensions to the existing header-only `CasLayout.h` (`manifestKey`, `_manifests` rejection) and `CasFormat.{h,cpp}` (three new `FormatId`s + magics). All under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`. Tests are new `gtest_*.cpp` files under `src/Disks/tests/` (auto-globbed into `unit_tests_dbms`; no CMake edit needed).

**Tech Stack:** C++ (ClickHouse coding standards, Allman braces). Reuses `CasCodecUtil.h` (`writeU128LE`/`readU128LE`/`u128ToBytesBE`/`u128FromBytesBE`/`readFixedBytes`/`decodeGuarded`), `CasIds.h` (`u128ToHex`/`hexToU128`, `UInt128`), IO buffers (`WriteBufferFromOwnString`, `ReadBufferFromMemory`, `WriteHelpers`/`ReadHelpers`), and `ch_contrib::crc32c` (header `<crc32c/crc32c.h>`, function `crc32c::Crc32c(const uint8_t *, size_t)` - hardware-portable, deterministic value). gtest (`unit_tests_dbms`, filter `Cas*:Ca*`).

## Global Constraints {#global-constraints}

*Repeated verbatim from `2026-06-26-cas-gc-redesign-overview.md` so an implementer who only sees one task still has them.*

**Branch & git**
- All implementation commits land on **`cas-gc-part-manifest-impl`**, created off `codex-gc-proposal-2026-06-26` (the design branch). **Never commit to `master`.**
- **Add new commits only - never `amend` or `rebase`.**
- Every commit message ends with these two trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```

**Requirements (from spec §Goals - non-negotiable)**
- **R0 - safety is TLA+-provable.** `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` must be *proved by the model*, not argued. No code task in a phase may begin until that phase's TLA+ gate is green.
- **R1 - bounded streaming round.** Work is proportional to changed owner transitions and the entries of the manifests they name; memory is bounded by stream buffers; GC state is coarse write-once objects.
- **R2 - target-shardable.** Default `gc_shards = 1`; sharded mode is optional (Phase 4).
- **R3 - simple, debuggable, idempotent, resumable.** Durable state explains what each round folded, retired, fenced, rechecked, deleted, trimmed.

**CA is pre-release**
- **ZERO on-disk compatibility scaffolding.** No reader for the old CA tree format, no dual-format code paths, no migration. Version fields in *new* formats are allowed; multi-version *handling* code is forbidden (per `feedback_ca_no_compat_scaffolding_predev`).

**Safety invariants that must never relax** (carried from `CaIncarnationCore.tla` + `CaBuildRootPrecommit.tla`)
- exact-token delete (`deleteExact`) is the only destructive authority; token mismatch is spared/replaced, never destructive;
- global registry fence precedes root-shard fences; fold-through-fence recheck precedes delete;
- `ViewableRound`: a round is writer-visible only after all its retired sets + part-manifest cleanup bundles are durable;
- `deadTok` / no-return: a deleted or overwritten token is never accepted as a future dependency;
- a writer that must resurrect a condemned blob re-uploads from its own source - **never** `GET`s the condemned object (per `feedback_ca_resurrect_invariant`);
- GC must never throw/fail-closed on a 404 during fold (record what you can and continue - per `feedback_ca_gc_never_throw_on_404`).

**Code style** (CI-enforced)
- Allman braces (opening brace on its own line).
- In prose/comments/commit messages: literal SQL keywords, class names, and function names in backticks (`MergeTree`); write a function as `f`, not `f()`; say "ASan" not "ASAN"; say "exception" not "crash" for logical errors.
- **Never use `sleep` in C++ to fix a race.**

**Build** (per CLAUDE.md)
- Build into a `build_*` directory (this plan uses `build`). Always redirect ninja output to `build/build.log`. **Analyze the build log with a subagent and return only a concise summary** - never paste raw build output.
- Do **not** pass `-j` to ninja and do **not** use `nproc`; let ninja decide.

**Tests**
- Redirect each test run to `build/test_<name>.log` (unique name per test). **Analyze each log with a subagent**; return a concise summary.
- Run CA gtests via the gtest binary built in the build dir with `--gtest_filter='Cas*:Ca*'`.

## Resolved Open Questions consumed here {#resolved-open-questions-consumed-here}

*Restated from the overview; these are vetoable plan edits, not code rewrites.*

- **OQ1 (`PartManifest` fields).** The `PartManifest` body carries exactly: `ref` (`ManifestRef`), `root_namespace_id`, `payload_digest` (integrity/debug only - never a key, never dedup, never in-degree), and `entries`. **No additional debug fields in Phase 1a.** The `header{magic, format_version, writer_version}` framing is provided by the envelope/format-id boundary of Task 5, not by extra struct fields. The optional `DirectoryIndex` is **OQ3-deferred** (see below).
- **OQ3 (internal manifest indexing).** Phase 1a stores `entries` in canonical path order as block-framed `RunFile` `DataBlock`s with a sparse footer index. **No `DirectoryIndex` type is defined here** - it is deferred and off by default; add it only when `listDirectory` profiles demand it.
- **OQ5 (block-run details).** Defaults: `block_size` target **256 KiB**, hard cap **1 MiB**; per-`DataBlock` **CRC32C** checksum; sparse footer index = one `(min_key, max_key, block_offset)` per block; **compression off by default** (hashes are high-entropy); hashes stored fixed-width; key schemas fixed per `kind`; encoding is **deterministic** (fixed block boundaries, no nondeterministic compression) so a write-once run is byte-reproducible for resume/adoption.

## Canonical Contract (names are fixed; later phases consume verbatim) {#canonical-contract-names-are-fixed-later-phases-consume-verbatim}

All under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/` unless noted. These are the **exact** types/functions Phase 1a emits. **Do not invent type names outside this contract.**

- `CasManifestId.h`:
  ```cpp
  struct ManifestRef { String writer_instance_id; uint64_t build_sequence; UInt128 manifest_instance_id; };
  // writer_instance_id is the writer-incarnation token formatted "<server_id_hex>:<process_epoch>"
  // operator== and operator< (for std::map / std::set)
  struct ManifestId { RootNamespace root_namespace; ManifestRef ref; };
  // operator== and operator< (root_namespace is the owning namespace and is NOT serialized into the journal ref)
  String manifestAa(const ManifestRef &);  // first 2 hex chars of manifest_instance_id
  // std::hash<ManifestId> and std::hash<ManifestRef> specializations (for unordered containers)
  ```
- `CasManifestCodec.h`/`.cpp`:
  ```cpp
  enum class EntryPlacement : uint8_t { Inline = 1, Blob = 2 };
  struct ManifestEntry { String path; EntryPlacement placement; UInt128 blob_hash; uint64_t blob_size; String inline_bytes; };
  struct PartManifest { ManifestRef ref; RootNamespace root_namespace_id; UInt128 payload_digest; std::vector<ManifestEntry> entries; };
  String encodePartManifest(const PartManifest &);   // deterministic, streaming-capable, entries in canonical path order, duplicate-path => throw CORRUPTED_DATA
  PartManifest decodePartManifest(std::string_view);
  UInt128 computePayloadDigest(const PartManifest &);  // BLAKE3 of the canonical encoded body; callers set PartManifest.payload_digest from it
  bool refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body);
  bool manifestNamespaceMatches(const RootNamespace & owning, const PartManifest & body);
  ```
- `CasRunFile.h`/`.cpp`:
  ```cpp
  enum class RunKind : uint8_t { BlobInDegree = 1, BlobDelta = 2, SourceEdge = 3, ManifestEntries = 4, TargetShardDelta = 5 };
  struct RunHeader { char magic[4]; uint16_t format_version; RunKind kind; uint8_t key_schema; uint8_t codec; uint32_t block_size; };
  class RunFileWriter { RunFileWriter(WriteBuffer &, RunHeader); void append(std::string_view key, std::string_view payload); void finish(); };
  class RunFileReader { RunFileReader(ReadBuffer &); bool next(String & key, String & payload); void seek(std::string_view key); };
  class RunMerger { RunMerger(std::vector<std::unique_ptr<RunFileReader>>); bool next(String & key, std::vector<String> & payloads_for_key); };
  ```
  Block-framed: target block 256 KiB, hard cap 1 MiB; per-`DataBlock` CRC32C; sparse footer index one `(min_key, max_key, offset)` per block; compression off by default; deterministic byte output. `RunMerger` is a k-way merge, memory `O(inputs * block_size)`.
- `CasLayout.h`: add `String manifestKey(const ManifestId & id) const` = `<prefix>/roots/<ns>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto`; `checkNamespace` ALSO rejects any segment `== "_manifests"`.
- `CasFormat.h`: add `FormatId::PartManifest = 12` (magic per [Magic Collision note](#magic-collision)), `FormatId::RunFile = 13` (magic "CARN"), `FormatId::FoldSeal = 14` (magic "CAFS"), `FormatId::CompletionSeal = 15` (magic "CACS"). (The single `GenerationSeal`/"CAGN" is gone — rev. 15 splits the generation seal into the write-once `CasFoldSeal` and `CasCompletionSeal`. Neither "CAFS" nor "CACS" collides with an existing magic.) (Phase 1d deletes `Tree`/`GcSnap`.)

## File Structure {#file-structure}

- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h` (add `manifestKey`, reserve `_manifests`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h` (add 3 `FormatId`s)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp` (add 3 magics + change points)
- Create: `src/Disks/tests/gtest_cas_manifest_id.cpp`
- Create: `src/Disks/tests/gtest_cas_run_file.cpp`
- Create: `src/Disks/tests/gtest_cas_manifest_codec.cpp`
- Modify: `src/Disks/tests/gtest_cas_format.cpp` (assert the 3 new magics/distinctness)
- Modify: `src/Disks/tests/gtest_cas_layout.cpp` (assert `manifestKey` shape + `_manifests` rejection)

CMake: `unit_tests_dbms` globs `gtest*.cpp` under `src/` (`src/CMakeLists.txt` `grep_gtest_sources`), so new gtest files need no registration. `CasManifestCodec.cpp` and `CasRunFile.cpp` are compiled into `dbms`/`clickhouse_common_io` by the existing CAS-core source glob; if a clean build does not pick them up, add them to the CAS-core source list - the Task 8 build step's subagent analysis will surface an undefined-symbol link error if so.

## Magic Collision (must be surfaced) {#magic-collision-must-be-surfaced}

The contract specifies `FormatId::PartManifest = 12` with magic **"CAPM"**. **"CAPM" is already taken**: `CasFormat.cpp` maps `FormatId::PoolMeta` to `0x4D504143u` (= "CAPM"), and `gtest_cas_format.cpp` asserts `magicFor(FormatId::PoolMeta) == "CAPM"`, and `cas_format.proto` documents `PoolMetaProto "CAPM"`. Reusing "CAPM" for `PartManifest` would (a) break `MagicsAreDistinct`, and (b) make bad-magic detection ambiguous between two object classes.

**Resolution used by this plan (vetoable):** keep the contract's **type name and enum value** (`FormatId::PartManifest = 12`) verbatim - those are the load-bearing identifiers later phases reference - but assign `PartManifest` the **non-colliding magic "CAPT"** (`'C','A','P','T'` => little-endian `0x54504143u`). The string "CAPM" stays with `PoolMeta`. Task 2 encodes this and Task 2's gtest asserts distinctness so a future re-collision fails closed. **If the user prefers to rename `PoolMeta`'s magic to free up "CAPM" for `PartManifest`, that is a one-line edit to this plan + `CasFormat.cpp` + the two assertions - flag it before proceeding.**

---

## Tasks {#tasks}

### Task 1: `CasManifestId.h` - identity types, ordering, `manifestAa` {#task-1-casmanifestid-h-identity-types-ordering-manifestaa}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h`
- Create: `src/Disks/tests/gtest_cas_manifest_id.cpp`

**Interfaces produced:** `ManifestRef`, `ManifestId`, `manifestAa`.

- [ ] **Step 1: Write `CasManifestId.h`** with the contract types, total orderings, and `manifestAa`.

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <tuple>

namespace DB::Cas
{

/// The compact reference a root journal stores for a part manifest (spec §Part Manifest Reference
/// And Identity). It is NOT a string key: `CasLayout::manifestKey` derives the object key from this
/// ref plus the owning root namespace. `root_namespace_id` is deliberately NOT a field here - it
/// comes from the owning root context and must never be serialized into the journal ref.
///
///   writer_instance_id   - writer-incarnation token, formatted "<server_id_hex>:<process_epoch>"; a
///                          new process epoch must not reuse the same build prefix. It is a String,
///                          used verbatim as the `<writer_instance_id>` path segment of `manifestKey`.
///   build_sequence       - monotone build sequence inside one writer incarnation; part of identity
///                          and of the build-scoped debris prefix.
///   manifest_instance_id - random 128-bit; gives collision-safety and the never-reused guarantee.
struct ManifestRef
{
    String writer_instance_id;
    uint64_t build_sequence = 0;
    UInt128 manifest_instance_id{};

    bool operator==(const ManifestRef & o) const = default;

    /// Total order for std::map / std::set keys. Field order is arbitrary but stable.
    bool operator<(const ManifestRef & o) const
    {
        return std::tie(writer_instance_id, build_sequence, manifest_instance_id)
             < std::tie(o.writer_instance_id, o.build_sequence, o.manifest_instance_id);
    }
};

/// The protocol identity GC uses (spec §Object Identity And Ownership): namespace-qualified
/// `ManifestId = (root_namespace_id, ManifestRef)`. It keys source edges / blob deltas / cleanup work
/// and addressing — distinct from `ManifestSafetyId = (root_namespace, manifest_instance_id)`, which is
/// a TLA+-abstraction-only term (Phase 0) and never appears in this code.
/// Two namespaces may legally carry the same `ManifestRef` tuple without addressing the same object;
/// keying source edges / blob deltas / cleanup work by `ManifestRef` alone is the modeled
/// `SabotageKeyByRefNotId` hazard. Always key by `ManifestId`.
struct ManifestId
{
    RootNamespace root_namespace;   /// owning namespace; NOT part of the journal ref
    ManifestRef ref;

    bool operator==(const ManifestId & o) const = default;

    bool operator<(const ManifestId & o) const
    {
        if (root_namespace.string() != o.root_namespace.string())
            return root_namespace.string() < o.root_namespace.string();
        return ref < o.ref;
    }
};

/// The 2-char fanout segment of a manifest key: the first 2 lowercase-hex chars of
/// `manifest_instance_id` (spec §S3 Layout: the `<aa>` fanout is derived from `manifest_instance_id`,
/// NOT from the payload digest). 32-char hex always has >= 2 chars, so this never underflows.
inline String manifestAa(const ManifestRef & ref)
{
    return u128ToHex(ref.manifest_instance_id).substr(0, 2);
}

}

/// std::hash specializations so ManifestRef / ManifestId can key unordered containers (the read-path
/// `(ManifestId, Token)` cache in Phase 1c, plus any GC-side unordered map). Equality is the
/// `operator==` above; these hashes must agree with it (equal values => equal hash).
namespace std
{

template <>
struct hash<DB::Cas::ManifestRef>
{
    size_t operator()(const DB::Cas::ManifestRef & r) const
    {
        const size_t h1 = std::hash<DB::String>{}(r.writer_instance_id);
        const size_t h2 = std::hash<uint64_t>{}(r.build_sequence);
        const size_t h3 = std::hash<DB::UInt128>{}(r.manifest_instance_id);
        size_t h = h1;
        h ^= h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= h3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

template <>
struct hash<DB::Cas::ManifestId>
{
    size_t operator()(const DB::Cas::ManifestId & id) const
    {
        const size_t h1 = std::hash<DB::String>{}(id.root_namespace.string());
        const size_t h2 = std::hash<DB::Cas::ManifestRef>{}(id.ref);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

}
```

- [ ] **Step 2: Write `gtest_cas_manifest_id.cpp`** - equality, ordering, namespace qualification, and `manifestAa`.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

using namespace DB::Cas;

namespace
{

/// writer_instance_id is the String "<server_id_hex>:<process_epoch>"; the helper renders `w` as the
/// process epoch so the ordering/equality tests below still vary it.
ManifestRef ref(uint64_t w, uint64_t seq, uint64_t m)
{
    return ManifestRef{"srv-a:" + std::to_string(w), seq, DB::UInt128(m)};
}

ManifestId id(const char * ns, uint64_t w, uint64_t seq, uint64_t m)
{
    return ManifestId{RootNamespace(ns), ref(w, seq, m)};
}

}

TEST(CasManifestId, RefEqualityAndOrdering)
{
    EXPECT_EQ(ref(1, 2, 3), ref(1, 2, 3));
    EXPECT_NE(ref(1, 2, 3), ref(1, 2, 4));
    /// Strict total order: distinct by manifest_instance_id, then build_sequence, then writer.
    EXPECT_LT(ref(1, 2, 3), ref(1, 2, 4));
    EXPECT_LT(ref(1, 2, 9), ref(1, 3, 0));
    EXPECT_LT(ref(1, 9, 9), ref(2, 0, 0));
    EXPECT_FALSE(ref(1, 2, 3) < ref(1, 2, 3));
}

TEST(CasManifestId, IdIsNamespaceQualified)
{
    /// Same ref tuple, different namespace => DIFFERENT ids (the SabotageKeyByRefNotId guard).
    EXPECT_NE(id("nsA", 1, 1, 1), id("nsB", 1, 1, 1));
    EXPECT_EQ(id("nsA", 1, 1, 1), id("nsA", 1, 1, 1));
    /// Ordering separates by namespace first.
    EXPECT_LT(id("nsA", 9, 9, 9), id("nsB", 0, 0, 0));
}

TEST(CasManifestId, UsableAsMapAndSetKey)
{
    std::set<ManifestId> s;
    s.insert(id("nsA", 1, 1, 1));
    s.insert(id("nsB", 1, 1, 1));   /// distinct namespace -> distinct key
    s.insert(id("nsA", 1, 1, 1));   /// duplicate -> no growth
    EXPECT_EQ(s.size(), 2u);

    std::map<ManifestRef, int> m;
    m[ref(1, 1, 1)] = 10;
    m[ref(1, 1, 2)] = 20;
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m[ref(1, 1, 1)], 10);
}

TEST(CasManifestId, UsableInUnorderedContainers)
{
    /// std::hash<ManifestRef> / std::hash<ManifestId> let the read-path cache (Phase 1c) and GC use
    /// unordered_map/set. Equal values => equal hash; distinct values => (overwhelmingly) distinct.
    std::unordered_set<ManifestId> s;
    s.insert(id("nsA", 1, 1, 1));
    s.insert(id("nsB", 1, 1, 1));   /// distinct namespace -> distinct key
    s.insert(id("nsA", 1, 1, 1));   /// duplicate -> no growth
    EXPECT_EQ(s.size(), 2u);

    std::unordered_map<ManifestRef, int> m;
    m[ref(1, 1, 1)] = 10;
    m[ref(1, 1, 1)] = 11;           /// same key overwrites
    m[ref(1, 1, 2)] = 20;
    EXPECT_EQ(m.size(), 2u);
    EXPECT_EQ(m.at(ref(1, 1, 1)), 11);

    EXPECT_EQ(std::hash<ManifestId>{}(id("nsA", 1, 1, 1)), std::hash<ManifestId>{}(id("nsA", 1, 1, 1)));
}

TEST(CasManifestId, ManifestAaIsFirstTwoHexOfInstanceId)
{
    /// manifest_instance_id = 0x7f3a...  -> low bytes; u128ToHex is big-endian-ish lowercase hex of
    /// the 128-bit value, so the leading 2 chars reflect the high half. Pin a concrete value.
    ManifestRef r;
    r.manifest_instance_id = (DB::UInt128(0x7f3aULL) << 112);   /// top byte 0x7f, next 0x3a
    EXPECT_EQ(manifestAa(r), "7f");
    /// A small value has leading zeros, so aa = "00".
    ManifestRef z;
    z.manifest_instance_id = DB::UInt128(0xc1);
    EXPECT_EQ(manifestAa(z), "00");
}
```

- [ ] **Step 3: Run the test - must PASS.** (`unit_tests_dbms` must already be built once via Task 8 the first time; for an incremental check after a build exists, rebuild only the target. On the very first run, this step is exercised inside Task 8's sweep. If a `build/unit_tests_dbms` already exists from prior work, run it directly.)

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasManifestId.*' 2>&1 | tail -20
```
Expected (analyze `build/build.log` via a subagent first - it must end with a successful link of `unit_tests_dbms`):
```
[----------] 5 tests from CasManifestId
...
[  PASSED  ] 5 tests.
```

- [ ] **Step 4: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h \
        src/Disks/tests/gtest_cas_manifest_id.cpp
git commit -m "CA GC phase1a: ManifestRef/ManifestId identity types + manifestAa + gtest

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 2: `CasFormat` - new `FormatId`s, magics, change points {#task-2-casformat-new-formatids-magics-change-points}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`
- Modify: `src/Disks/tests/gtest_cas_format.cpp`

**Interfaces produced:** `FormatId::PartManifest = 12`, `FormatId::RunFile = 13`, `FormatId::FoldSeal = 14`, `FormatId::CompletionSeal = 15`; their magics and gen-1 change points.

- [ ] **Step 1: Add the four enumerators to `CasFormat.h`.** Edit the `enum class FormatId` block; append after `GcOutcomes = 11`:

```cpp
    GcOutcomes = 11,
    /// Phase 1a (CA GC root-local part-manifest redesign):
    PartManifest = 12,    /// immutable root-local part manifest body; magic "CAPT" (see plan note: "CAPM" is taken by PoolMeta)
    RunFile = 13,         /// dense block-framed sorted binary data-plane run; magic "CARN"
    /// rev. 15 splits the old single generation seal into two write-once phase seals:
    FoldSeal = 14,        /// write-once gc/gen/<gen>/fold_seal (coverage + blob_target/cleanup runs); magic "CAFS"
    CompletionSeal = 15,  /// write-once gc/gen/<gen>/completion_seal (fence/recheck/delete/trim + adoptable); magic "CACS"
```

- [ ] **Step 2: Add magics in `CasFormat.cpp` `magicFor`.** In the `switch`, add four cases (before the trailing `throw`). Compute the little-endian uint32 of each 4-ASCII string:
  - "CAPT" = `'C'=0x43,'A'=0x41,'P'=0x50,'T'=0x54` => `0x54504143u`
  - "CARN" = `'C'=0x43,'A'=0x41,'R'=0x52,'N'=0x4E` => `0x4E524143u`
  - "CAFS" = `'C'=0x43,'A'=0x41,'F'=0x46,'S'=0x53` => `0x53464143u`
  - "CACS" = `'C'=0x43,'A'=0x41,'C'=0x43,'S'=0x53` => `0x53434143u`

```cpp
        case FormatId::GcOutcomes:     return 0x4F474143u; /// "CAGO"
        case FormatId::PartManifest:   return 0x54504143u; /// "CAPT" (NOT "CAPM"; that is PoolMeta)
        case FormatId::RunFile:        return 0x4E524143u; /// "CARN"
        case FormatId::FoldSeal:       return 0x53464143u; /// "CAFS"
        case FormatId::CompletionSeal: return 0x53434143u; /// "CACS"
```

- [ ] **Step 3: Add the four to `changePoints` in `CasFormat.cpp`.** Append the new enumerators to the fall-through list that returns `BASELINE`:

```cpp
        case FormatId::RootsRegistry:
        case FormatId::GcOutcomes:
        case FormatId::PartManifest:
        case FormatId::RunFile:
        case FormatId::FoldSeal:
        case FormatId::CompletionSeal:
            return BASELINE;
```

- [ ] **Step 4: Extend `gtest_cas_format.cpp`.** (a) add the four ids to the `ChangePointsExistForEveryClass` loop; (b) add four `EXPECT_EQ(le32toStr(...), ...)` lines to `MagicForEachMutableObjectClass`; (c) add the four magics to the `MagicsAreDistinct` array. Apply these three edits:

In `ChangePointsExistForEveryClass`, extend the initializer list:
```cpp
    for (auto id : {FormatId::Blob, FormatId::Tree, FormatId::Manifest, FormatId::GcSnap,
                    FormatId::GcState, FormatId::RetiredSet, FormatId::Watermark,
                    FormatId::PoolMeta, FormatId::Roster,
                    FormatId::RootsRegistry, FormatId::GcOutcomes,
                    FormatId::PartManifest, FormatId::RunFile,
                    FormatId::FoldSeal, FormatId::CompletionSeal})
```

In `MagicForEachMutableObjectClass`, append after the `GcOutcomes` line:
```cpp
    EXPECT_EQ(le32toStr(magicFor(FormatId::PartManifest)),    "CAPT");
    EXPECT_EQ(le32toStr(magicFor(FormatId::RunFile)),         "CARN");
    EXPECT_EQ(le32toStr(magicFor(FormatId::FoldSeal)),        "CAFS");
    EXPECT_EQ(le32toStr(magicFor(FormatId::CompletionSeal)),  "CACS");
    /// Guard the documented collision: PartManifest must NOT reuse PoolMeta's "CAPM".
    EXPECT_NE(magicFor(FormatId::PartManifest), magicFor(FormatId::PoolMeta));
```

In `MagicsAreDistinct`, append four entries to the `magics[]` array:
```cpp
        magicFor(FormatId::GcOutcomes),
        magicFor(FormatId::PartManifest),
        magicFor(FormatId::RunFile),
        magicFor(FormatId::FoldSeal),
        magicFor(FormatId::CompletionSeal),
```

- [ ] **Step 5: Run the format tests - must PASS.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasFormat.*' 2>&1 | tail -20
```
Expected (analyze `build/build.log` via subagent first):
```
[  PASSED  ] 6 tests.
```

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp \
        src/Disks/tests/gtest_cas_format.cpp
git commit -m "CA GC phase1a: add FormatId PartManifest(12)/RunFile(13)/FoldSeal(14)/CompletionSeal(15) + magics

PartManifest takes magic CAPT, not CAPM: CAPM is already PoolMeta's magic.
rev. 15 splits the generation seal into FoldSeal (CAFS) + CompletionSeal (CACS); the single CAGN is gone.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 3: `CasRunFile` writer/reader - block framing, CRC32C, sparse footer, range-seek {#task-3-casrunfile-writer-reader-block-framing-crc32c-sparse-footer-range-seek}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp`
- Create: `src/Disks/tests/gtest_cas_run_file.cpp`

**Interfaces produced:** `RunKind`, `RunHeader`, `RunFileWriter`, `RunFileReader`. (`RunMerger` is Task 4.)

**On-wire format (OQ5, frozen here):**
```
RunFile
  RunHeader  { magic[4]="CARN", format_version u16, kind u8, key_schema u8, codec u8, block_size u32 }   // fixed-width, LE scalars
  DataBlock* { block_len u32, record_count u32, min_key(len-prefixed), max_key(len-prefixed), crc32c u32, payload }
               payload = repeat record_count of: key_len u32, key bytes, payload_len u32, payload bytes  (keys non-decreasing)
               crc32c = crc32c::Crc32c over the payload bytes only
  RunFooter  { block_count u32, then per block: block_offset u64, min_key(len-prefixed), max_key(len-prefixed);
               total_count u64, footer_crc32c u32 over the block-index bytes, footer_len u32 (trailer, LE) }
```
The reader locates the footer by reading the trailing `footer_len u32`, seeking back `footer_len` bytes. `block_offset` is the byte offset of the `DataBlock` from the start of the file. Determinism: a block is sealed when adding the next record would exceed `block_size` (target 256 KiB) OR when a single record's encoded size would exceed the 1 MiB hard cap (such a record gets its own block). No timestamps, no compression by default => byte-identical output for identical input.

- [ ] **Step 1: Write `CasRunFile.h`** with the contract signatures and the frozen constants.

```cpp
#pragma once
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <base/types.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Dense block-framed sorted binary run (spec §Backpressure And Journal Encoding). This is the hot
/// data-plane format: no per-record protobuf tags, no varints in the framing scalars, fixed-width
/// CRC32C per block, sparse footer index. `payload` is specialized by `kind`; the key schema is fixed
/// per kind (blob_hash; (blob_hash, source_id); (target_shard, blob_hash); etc.). The format is
/// deterministic so a write-once run is byte-reproducible for resume/adoption (OQ5).
enum class RunKind : uint8_t
{
    BlobInDegree = 1,
    BlobDelta = 2,
    SourceEdge = 3,
    ManifestEntries = 4,
    TargetShardDelta = 5,
};

/// Block sizing (OQ5). Target a large sequential block; hard-cap any single block.
constexpr uint32_t kRunTargetBlockSize = 256u * 1024u;
constexpr uint32_t kRunHardCapBlockSize = 1024u * 1024u;
constexpr uint16_t kRunFormatVersion = 1;
/// codec 0 = no compression (the only codec in Phase 1a; hashes are high-entropy).
constexpr uint8_t kRunCodecNone = 0;

struct RunHeader
{
    char magic[4] = {'C', 'A', 'R', 'N'};
    uint16_t format_version = kRunFormatVersion;
    RunKind kind = RunKind::BlobDelta;
    uint8_t key_schema = 0;     /// fixed per kind; meaning owned by the producer
    uint8_t codec = kRunCodecNone;
    uint32_t block_size = kRunTargetBlockSize;
};

/// Streaming writer. Keys must be appended in non-decreasing order (the caller sorts). Memory is
/// bounded by one in-flight block (<= hard cap) plus the footer index. `finish` must be called
/// exactly once; it flushes the last block and writes the footer.
class RunFileWriter
{
public:
    RunFileWriter(WriteBuffer & out_, RunHeader header_);
    void append(std::string_view key, std::string_view payload);
    void finish();

private:
    struct BlockIndexEntry
    {
        uint64_t block_offset = 0;
        String min_key;
        String max_key;
    };

    void flushBlock();

    WriteBuffer & out;
    RunHeader header;
    uint64_t bytes_written = 0;          /// running file offset (header + sealed blocks)
    String block_payload;                /// in-flight DataBlock payload
    uint32_t block_records = 0;
    String block_min_key;
    String block_max_key;
    String prev_key;
    bool have_prev_key = false;
    std::vector<BlockIndexEntry> index;
    uint64_t total_count = 0;
    bool finished = false;
};

/// Streaming reader. `next` yields records in stored (sorted) order. `seek(key)` repositions the
/// cursor to the first record whose key >= `key`, using the sparse footer index to skip whole blocks
/// (one ranged read region per touched block).
class RunFileReader
{
public:
    explicit RunFileReader(ReadBuffer & in_);
    bool next(String & key, String & payload);
    void seek(std::string_view key);
    RunKind kind() const { return header.kind; }
    uint8_t keySchema() const { return header.key_schema; }

private:
    struct BlockIndexEntry
    {
        uint64_t block_offset = 0;
        String min_key;
        String max_key;
    };

    void loadFooter();
    bool loadBlock(size_t block_no);

    ReadBuffer & in;
    RunHeader header;
    std::vector<BlockIndexEntry> index;
    uint64_t total_count = 0;

    /// in-memory cursor over the currently loaded block
    String cur_block;
    size_t cur_block_pos = 0;
    uint32_t cur_block_records = 0;
    uint32_t cur_record_no = 0;
    size_t cur_block_idx = 0;
    bool block_loaded = false;
    bool exhausted = false;
};

}
```

- [ ] **Step 2: Write `CasRunFile.cpp`.** Use `WriteHelpers`/`ReadHelpers` for LE scalars and `ch_contrib::crc32c`. (`#include <crc32c/crc32c.h>` for `crc32c::Crc32c`.) The reader reads the whole `ReadBuffer` into a `String` once (the run is materialized in memory at this layer, mirroring the `ReadBufferFromMemory` assumption in `CasCodecUtil.h`), so `seek` can index blocks directly.

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <Common/Exception.h>
#include <crc32c/crc32c.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

void putLE32(String & s, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putLenPrefixed(String & s, std::string_view bytes)
{
    putLE32(s, static_cast<uint32_t>(bytes.size()));
    s.append(bytes.data(), bytes.size());
}

uint32_t crc32cOf(std::string_view bytes)
{
    return crc32c::Crc32c(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
}

}

RunFileWriter::RunFileWriter(WriteBuffer & out_, RunHeader header_) : out(out_), header(header_)
{
    /// RunHeader: magic[4], format_version u16, kind u8, key_schema u8, codec u8, block_size u32.
    out.write(header.magic, 4);
    writeBinaryLittleEndian(header.format_version, out);
    writeBinaryLittleEndian(static_cast<uint8_t>(header.kind), out);
    writeBinaryLittleEndian(header.key_schema, out);
    writeBinaryLittleEndian(header.codec, out);
    writeBinaryLittleEndian(header.block_size, out);
    bytes_written = 4 + 2 + 1 + 1 + 1 + 4;
}

void RunFileWriter::append(std::string_view key, std::string_view payload)
{
    if (finished)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "RunFileWriter: append after finish");
    if (have_prev_key && key < prev_key)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "RunFileWriter: keys must be non-decreasing");

    /// Encoded record size: key_len u32 + key + payload_len u32 + payload.
    const size_t rec_size = 4 + key.size() + 4 + payload.size();

    /// Seal the current block first if adding this record would exceed the target and the block is
    /// non-empty (deterministic boundary). A single oversize record (> hard cap) still gets its own
    /// block - we never split a record.
    if (block_records > 0 && block_payload.size() + rec_size > header.block_size)
        flushBlock();

    if (block_records == 0)
        block_min_key.assign(key.begin(), key.end());
    block_max_key.assign(key.begin(), key.end());

    putLE32(block_payload, static_cast<uint32_t>(key.size()));
    block_payload.append(key.data(), key.size());
    putLE32(block_payload, static_cast<uint32_t>(payload.size()));
    block_payload.append(payload.data(), payload.size());
    ++block_records;
    ++total_count;

    prev_key.assign(key.begin(), key.end());
    have_prev_key = true;

    if (block_payload.size() >= kRunHardCapBlockSize)
        flushBlock();
}

void RunFileWriter::flushBlock()
{
    if (block_records == 0)
        return;

    BlockIndexEntry idx;
    idx.block_offset = bytes_written;
    idx.min_key = block_min_key;
    idx.max_key = block_max_key;
    index.push_back(std::move(idx));

    /// DataBlock: block_len u32, record_count u32, min_key(len-prefixed), max_key(len-prefixed),
    /// crc32c u32, payload. block_len is the byte length of (record_count..payload) inclusive.
    String block_head;
    putLE32(block_head, block_records);
    putLenPrefixed(block_head, block_min_key);
    putLenPrefixed(block_head, block_max_key);
    putLE32(block_head, crc32cOf(block_payload));
    const uint32_t block_len = static_cast<uint32_t>(block_head.size() + block_payload.size());

    writeBinaryLittleEndian(block_len, out);
    out.write(block_head.data(), block_head.size());
    out.write(block_payload.data(), block_payload.size());
    bytes_written += 4 + block_len;

    block_payload.clear();
    block_records = 0;
    block_min_key.clear();
    block_max_key.clear();
}

void RunFileWriter::finish()
{
    if (finished)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "RunFileWriter: finish twice");
    flushBlock();

    /// RunFooter: block_count u32, per block { block_offset u64, min_key(lp), max_key(lp) },
    /// total_count u64, footer_crc32c u32, footer_len u32 trailer.
    String footer;
    putLE32(footer, static_cast<uint32_t>(index.size()));
    for (const auto & e : index)
    {
        for (int i = 0; i < 8; ++i)
            footer.push_back(static_cast<char>((e.block_offset >> (8 * i)) & 0xFF));
        putLenPrefixed(footer, e.min_key);
        putLenPrefixed(footer, e.max_key);
    }
    for (int i = 0; i < 8; ++i)
        footer.push_back(static_cast<char>((total_count >> (8 * i)) & 0xFF));
    putLE32(footer, crc32cOf(footer));
    const uint32_t footer_len = static_cast<uint32_t>(footer.size() + 4);   /// + the trailer itself

    out.write(footer.data(), footer.size());
    writeBinaryLittleEndian(footer_len, out);
    finished = true;
}

/// ---- reader ----

RunFileReader::RunFileReader(ReadBuffer & in_) : in(in_)
{
    in.read(header.magic, 4);
    if (std::string_view(header.magic, 4) != "CARN")
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad magic");
    readBinaryLittleEndian(header.format_version, in);
    checkCompatibility(header.format_version, "RunFile");
    uint8_t kind_raw = 0;
    readBinaryLittleEndian(kind_raw, in);
    header.kind = static_cast<RunKind>(kind_raw);
    readBinaryLittleEndian(header.key_schema, in);
    readBinaryLittleEndian(header.codec, in);
    readBinaryLittleEndian(header.block_size, in);
    if (header.codec != kRunCodecNone)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: unsupported codec {}", header.codec);
    loadFooter();
}

void RunFileReader::loadFooter()
{
    /// Materialize the whole run (this layer reads from in-memory backends). Then locate the footer
    /// via the trailing footer_len u32.
    String all;
    {
        WriteBufferFromOwnString tmp;
        copyData(in, tmp);
        all = tmp.str();
    }
    /// Re-prime a memory reader over the full bytes so next()/seek() can index blocks directly.
    full = std::move(all);
    if (full.size() < 4)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: truncated");

    auto le32at = [&](size_t off) -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };
    auto le64at = [&](size_t off) -> uint64_t
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };

    const uint32_t footer_len = le32at(full.size() - 4);
    if (footer_len < 4 || footer_len > full.size())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: bad footer_len");
    size_t pos = full.size() - footer_len;             /// start of footer body
    const size_t footer_body_end = full.size() - 4;    /// excludes the trailer u32

    const uint32_t block_count = le32at(pos); pos += 4;
    index.clear();
    index.reserve(block_count);
    for (uint32_t b = 0; b < block_count; ++b)
    {
        BlockIndexEntry e;
        e.block_offset = le64at(pos); pos += 8;
        uint32_t mn = le32at(pos); pos += 4; e.min_key = full.substr(pos, mn); pos += mn;
        uint32_t mx = le32at(pos); pos += 4; e.max_key = full.substr(pos, mx); pos += mx;
        index.push_back(std::move(e));
    }
    total_count = le64at(pos); pos += 8;
    /// crc check over the footer body (everything before the trailing crc + footer_len).
    const uint32_t want_crc = le32at(pos);
    const std::string_view footer_body(full.data() + (full.size() - footer_len), (footer_body_end - 4) - (full.size() - footer_len));
    if (crc32cOf(footer_body) != want_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: footer crc mismatch");
}

bool RunFileReader::loadBlock(size_t block_no)
{
    if (block_no >= index.size())
    {
        exhausted = true;
        return false;
    }
    auto le32at = [&](size_t off) -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(full[off + i])) << (8 * i);
        return v;
    };

    size_t off = index[block_no].block_offset;
    const uint32_t block_len = le32at(off); off += 4;
    const size_t block_end = off + block_len;
    const uint32_t rec_count = le32at(off); off += 4;
    uint32_t mn = le32at(off); off += 4; off += mn;        /// skip min_key
    uint32_t mx = le32at(off); off += 4; off += mx;        /// skip max_key
    const uint32_t stored_crc = le32at(off); off += 4;
    const std::string_view payload(full.data() + off, block_end - off);
    if (crc32cOf(payload) != stored_crc)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "RunFile: block crc mismatch");

    cur_block.assign(payload.data(), payload.size());
    cur_block_pos = 0;
    cur_block_records = rec_count;
    cur_record_no = 0;
    cur_block_idx = block_no;
    block_loaded = true;
    return true;
}

bool RunFileReader::next(String & key, String & payload)
{
    if (exhausted)
        return false;
    if (!block_loaded && !loadBlock(0))
        return false;
    while (cur_record_no >= cur_block_records)
    {
        if (!loadBlock(cur_block_idx + 1))
            return false;
    }
    auto le32at = [&](const String & s, size_t off) -> uint32_t
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(static_cast<uint8_t>(s[off + i])) << (8 * i);
        return v;
    };
    uint32_t klen = le32at(cur_block, cur_block_pos); cur_block_pos += 4;
    key = cur_block.substr(cur_block_pos, klen); cur_block_pos += klen;
    uint32_t plen = le32at(cur_block, cur_block_pos); cur_block_pos += 4;
    payload = cur_block.substr(cur_block_pos, plen); cur_block_pos += plen;
    ++cur_record_no;
    return true;
}

void RunFileReader::seek(std::string_view key)
{
    exhausted = false;
    /// Find the last block whose min_key <= key (sparse index); start scanning there.
    size_t target = 0;
    for (size_t b = 0; b < index.size(); ++b)
    {
        if (index[b].min_key <= key)
            target = b;
        else
            break;
    }
    if (index.empty())
    {
        exhausted = true;
        return;
    }
    loadBlock(target);
    /// Advance within the block to the first record with stored_key >= key.
    String k, p;
    size_t save_pos = cur_block_pos;
    uint32_t save_rec = cur_record_no;
    while (cur_record_no < cur_block_records)
    {
        save_pos = cur_block_pos;
        save_rec = cur_record_no;
        if (!next(k, p))
            return;
        if (k >= key)
        {
            /// rewind one record so the next next() re-yields it
            cur_block_pos = save_pos;
            cur_record_no = save_rec;
            return;
        }
    }
}

}
```

Add the two members `String full;` to `RunFileReader`'s private section in `CasRunFile.h` (the reader materializes the whole run). Edit the header to add:
```cpp
    String full;   /// the materialized run bytes (this layer reads in-memory backends)
```
and add `#include <IO/WriteBufferFromString.h>` and `#include <IO/copyData.h>` to `CasRunFile.cpp`.

- [ ] **Step 3: Write `gtest_cas_run_file.cpp`** - round-trip, byte-determinism, range-seek, CRC corruption fail-closed.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromMemory.h>
#include <Common/Exception.h>
#include <vector>
#include <string>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; extern const int LOGICAL_ERROR; }

using namespace DB::Cas;

namespace
{

/// Encode a vector of (key,payload) into a RunFile and return the bytes.
String writeRun(const std::vector<std::pair<String, String>> & recs, uint32_t block_size = kRunTargetBlockSize)
{
    DB::WriteBufferFromOwnString out;
    RunHeader h;
    h.kind = RunKind::BlobDelta;
    h.key_schema = 0;
    h.block_size = block_size;
    RunFileWriter w(out, h);
    for (const auto & [k, p] : recs)
        w.append(k, p);
    w.finish();
    return out.str();
}

std::vector<std::pair<String, String>> readRun(const String & bytes)
{
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    RunFileReader r(in);
    std::vector<std::pair<String, String>> out;
    String k, p;
    while (r.next(k, p))
        out.emplace_back(k, p);
    return out;
}

}

TEST(CasRunFile, RoundTripSingleBlock)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "1"}, {"bb", "22"}, {"cc", "333"}};
    const String bytes = writeRun(recs);
    EXPECT_EQ(readRun(bytes), recs);
}

TEST(CasRunFile, RoundTripManyBlocks)
{
    /// Force many small blocks (block_size = 32 bytes) so the footer index has > 1 entry.
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < 200; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "k%05d", i);
        recs.emplace_back(String(k), String("v") + std::to_string(i));
    }
    const String bytes = writeRun(recs, /*block_size*/ 32);
    EXPECT_EQ(readRun(bytes), recs);
}

TEST(CasRunFile, ByteDeterminism)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "x"}, {"bb", "y"}, {"cc", "z"}};
    EXPECT_EQ(writeRun(recs, 32), writeRun(recs, 32));   /// encode twice -> identical bytes
}

TEST(CasRunFile, KeysMustBeNonDecreasing)
{
    DB::WriteBufferFromOwnString out;
    RunFileWriter w(out, RunHeader{});
    w.append("bb", "1");
    try
    {
        w.append("aa", "2");   /// out of order
        FAIL() << "expected LOGICAL_ERROR";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::LOGICAL_ERROR);
    }
}

TEST(CasRunFile, SeekToKeyRange)
{
    std::vector<std::pair<String, String>> recs;
    for (int i = 0; i < 100; ++i)
    {
        char k[8];
        std::snprintf(k, sizeof(k), "k%03d", i);
        recs.emplace_back(String(k), std::to_string(i));
    }
    const String bytes = writeRun(recs, /*block_size*/ 24);
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    RunFileReader r(in);
    r.seek("k050");
    String k, p;
    ASSERT_TRUE(r.next(k, p));
    EXPECT_EQ(k, "k050");      /// first key >= "k050"
    EXPECT_EQ(p, "50");
    /// Seeking to a key between two stored keys lands on the next-greater.
    DB::ReadBufferFromMemory in2(bytes.data(), bytes.size());
    RunFileReader r2(in2);
    r2.seek("k0509");          /// no exact match; next is k051
    ASSERT_TRUE(r2.next(k, p));
    EXPECT_EQ(k, "k051");
}

TEST(CasRunFile, CorruptedPayloadFailsClosed)
{
    std::vector<std::pair<String, String>> recs = {{"aa", "payload-bytes"}, {"bb", "more"}};
    String bytes = writeRun(recs);
    /// Flip a byte inside the first block payload (well past the header) -> crc mismatch on read.
    bytes[bytes.size() / 2] ^= 0xFF;
    DB::ReadBufferFromMemory in(bytes.data(), bytes.size());
    try
    {
        RunFileReader r(in);
        String k, p;
        while (r.next(k, p)) {}
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}
```

- [ ] **Step 4: Run the run-file tests - must PASS.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasRunFile.*' 2>&1 | tail -25
```
Expected (analyze `build/build.log` via subagent; the new `.cpp` must link - if undefined symbols appear, add `CasRunFile.cpp` to the CAS-core source list and rebuild):
```
[  PASSED  ] 6 tests.
```

- [ ] **Step 5: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp \
        src/Disks/tests/gtest_cas_run_file.cpp
git commit -m "CA GC phase1a: RunFile writer/reader (block framing, CRC32C, sparse footer, range-seek)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 4: `RunMerger` - k-way merge over sorted runs, bounded memory {#task-4-runmerger-k-way-merge-over-sorted-runs-bounded-memory}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h` (add `RunMerger`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp` (implement `RunMerger`)
- Modify: `src/Disks/tests/gtest_cas_run_file.cpp` (add merge tests)

**Interfaces produced:** `RunMerger`.

- [ ] **Step 1: Add `RunMerger` to `CasRunFile.h`** (after `RunFileReader`).

```cpp
/// K-way merge over several sorted `RunFileReader`s. `next` advances all readers positioned at the
/// smallest key and returns that key together with EVERY payload stored for it (across all inputs and
/// across duplicate-key records within one input). Memory is O(inputs * block_size): only the current
/// front record of each reader plus one loaded block per reader is resident. Inputs must share a key
/// ordering (they do: keys are byte-compared).
class RunMerger
{
public:
    explicit RunMerger(std::vector<std::unique_ptr<RunFileReader>> readers_);
    bool next(String & key, std::vector<String> & payloads_for_key);

private:
    struct Front
    {
        size_t reader_idx = 0;
        String key;
        String payload;
        bool valid = false;
    };

    void pull(size_t reader_idx);

    std::vector<std::unique_ptr<RunFileReader>> readers;
    std::vector<Front> fronts;   /// one current front record per reader
};
```

- [ ] **Step 2: Implement `RunMerger` in `CasRunFile.cpp`** (append before the closing `namespace` brace).

```cpp
RunMerger::RunMerger(std::vector<std::unique_ptr<RunFileReader>> readers_) : readers(std::move(readers_))
{
    fronts.resize(readers.size());
    for (size_t i = 0; i < readers.size(); ++i)
        pull(i);
}

void RunMerger::pull(size_t reader_idx)
{
    Front & f = fronts[reader_idx];
    f.reader_idx = reader_idx;
    f.valid = readers[reader_idx]->next(f.key, f.payload);
}

bool RunMerger::next(String & key, std::vector<String> & payloads_for_key)
{
    /// Find the smallest valid front key.
    bool any = false;
    String min_key;
    for (const auto & f : fronts)
    {
        if (!f.valid)
            continue;
        if (!any || f.key < min_key)
        {
            min_key = f.key;
            any = true;
        }
    }
    if (!any)
        return false;

    key = min_key;
    payloads_for_key.clear();
    /// Drain every front (and every consecutive duplicate-key record per reader) equal to min_key.
    for (size_t i = 0; i < fronts.size(); ++i)
    {
        while (fronts[i].valid && fronts[i].key == min_key)
        {
            payloads_for_key.push_back(fronts[i].payload);
            pull(i);
        }
    }
    return true;
}
```

Add `#include <memory>` and `#include <vector>` to `CasRunFile.cpp` if not already present (they are pulled transitively via the header, but add explicitly).

- [ ] **Step 3: Add merge tests to `gtest_cas_run_file.cpp`.**

```cpp
TEST(CasRunFile, MergeTwoDisjointRuns)
{
    const String a = writeRun({{"a", "1"}, {"c", "3"}, {"e", "5"}});
    const String b = writeRun({{"b", "2"}, {"d", "4"}, {"f", "6"}});
    DB::ReadBufferFromMemory ia(a.data(), a.size());
    DB::ReadBufferFromMemory ib(b.data(), b.size());
    std::vector<std::unique_ptr<RunFileReader>> rs;
    rs.push_back(std::make_unique<RunFileReader>(ia));
    rs.push_back(std::make_unique<RunFileReader>(ib));
    RunMerger m(std::move(rs));
    String k;
    std::vector<String> vs;
    std::vector<String> seen;
    while (m.next(k, vs))
    {
        EXPECT_EQ(vs.size(), 1u);
        seen.push_back(k);
    }
    EXPECT_EQ(seen, (std::vector<String>{"a", "b", "c", "d", "e", "f"}));
}

TEST(CasRunFile, MergeCoalescesSameKeyAcrossRuns)
{
    /// Same key "k" present in both runs -> one merged key with both payloads.
    const String a = writeRun({{"k", "from-a"}, {"z", "9"}});
    const String b = writeRun({{"k", "from-b"}});
    DB::ReadBufferFromMemory ia(a.data(), a.size());
    DB::ReadBufferFromMemory ib(b.data(), b.size());
    std::vector<std::unique_ptr<RunFileReader>> rs;
    rs.push_back(std::make_unique<RunFileReader>(ia));
    rs.push_back(std::make_unique<RunFileReader>(ib));
    RunMerger m(std::move(rs));
    String k;
    std::vector<String> vs;
    ASSERT_TRUE(m.next(k, vs));
    EXPECT_EQ(k, "k");
    ASSERT_EQ(vs.size(), 2u);
    /// Payloads come in reader order: run a first, then run b.
    EXPECT_EQ(vs[0], "from-a");
    EXPECT_EQ(vs[1], "from-b");
    ASSERT_TRUE(m.next(k, vs));
    EXPECT_EQ(k, "z");
    EXPECT_FALSE(m.next(k, vs));
}

TEST(CasRunFile, MergeEmptyInput)
{
    std::vector<std::unique_ptr<RunFileReader>> rs;   /// no readers
    RunMerger m(std::move(rs));
    String k;
    std::vector<String> vs;
    EXPECT_FALSE(m.next(k, vs));
}
```

- [ ] **Step 4: Run the merge tests - must PASS.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasRunFile.Merge*' 2>&1 | tail -20
```
Expected:
```
[  PASSED  ] 3 tests.
```

- [ ] **Step 5: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp \
        src/Disks/tests/gtest_cas_run_file.cpp
git commit -m "CA GC phase1a: RunMerger k-way merge over sorted runs (bounded memory) + gtest

Memory is O(inputs * block_size): one loaded block + one front record per reader.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 5: `CasManifestCodec` - `PartManifest`/`ManifestEntry` deterministic encode/decode {#task-5-casmanifestcodec-partmanifest-manifestentry-deterministic-encode-decode}

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp`
- Create: `src/Disks/tests/gtest_cas_manifest_codec.cpp`

**Interfaces produced:** `EntryPlacement`, `ManifestEntry`, `PartManifest`, `encodePartManifest`, `decodePartManifest`, `computePayloadDigest`. (`refMatchesBody`/`manifestNamespaceMatches` are declared here but tested in Task 6.)

**On-wire format (OQ1, frozen here):** header (magic "CAPT" + `format_version u16` + `writer_version u16`), then ref (`writer_instance_id` len-prefixed bytes, `build_sequence` u64 LE, `manifest_instance_id` 16B LE), then `root_namespace_id` (len-prefixed bytes), then `payload_digest` 16B LE, then the entries as a single embedded `RunFile` of `kind = ManifestEntries` (so large manifests are block-framed, not one protobuf per entry). The entry `RunFile` key is the entry `path`; the entry `RunFile` payload is `placement u8, blob_hash 16B LE, blob_size u64 LE, inline_len u32, inline_bytes`. The codec sorts entries by `path` before writing and rejects a duplicate path with `CORRUPTED_DATA`.

- [ ] **Step 1: Write `CasManifestCodec.h`.**

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Where a manifest entry's file bytes live (spec §Object Identity And Ownership). No `Subtree`:
/// there are no nested tree objects in this design; a directory is a path prefix, not a placement.
enum class EntryPlacement : uint8_t
{
    Inline = 1,   /// bytes embedded in `inline_bytes`
    Blob = 2,     /// bytes stored as a content-addressed blob at blobKey(blob_hash)
};

/// One file entry inside a part manifest. `blob_hash`/`blob_size` are meaningful only for `Blob`;
/// `inline_bytes` only for `Inline`.
struct ManifestEntry
{
    String path;
    EntryPlacement placement = EntryPlacement::Inline;
    UInt128 blob_hash{};
    uint64_t blob_size = 0;
    String inline_bytes;
    bool operator==(const ManifestEntry &) const = default;
};

/// The immutable body of a single root-local part manifest (spec §Part Manifest Reference And
/// Identity, OQ1). It repeats its own `ref` and `root_namespace_id` for fail-closed validation only -
/// never as a second identity. `payload_digest` is integrity/debug only: NEVER a key, NEVER dedup,
/// NEVER in-degree. No mutable per-ref payload here (that stays in the root RefRecord). No directory
/// index in Phase 1a (OQ3-deferred).
struct PartManifest
{
    ManifestRef ref;
    RootNamespace root_namespace_id;
    UInt128 payload_digest{};
    std::vector<ManifestEntry> entries;
    bool operator==(const PartManifest &) const = default;
};

/// Deterministic, streaming-capable encode. Entries are written in canonical path order (the encoder
/// sorts them); a duplicate path throws CORRUPTED_DATA. Byte output is reproducible for identical
/// input (no timestamps, no nondeterministic compression).
String encodePartManifest(const PartManifest & m);

/// Decode. Throws CORRUPTED_DATA on bad magic, future format, unknown placement, duplicate path, or a
/// truncated buffer.
PartManifest decodePartManifest(std::string_view data);

/// BLAKE3 digest of the canonical encoded body. Callers (Phase 1b `stageManifest`) set
/// `PartManifest.payload_digest` from this. It is integrity/debug ONLY - never a key, never dedup,
/// never in-degree. Stable for identical bodies; changes when any byte of the canonical encoding does.
UInt128 computePayloadDigest(const PartManifest & m);

/// Fail-closed read/fold checks (spec §Object Identity And Ownership). Tested in Task 6.
/// The journal `ManifestRef` must equal the `ref` inside the decoded body.
bool refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body);
/// The body `root_namespace_id` must equal the owning root namespace.
bool manifestNamespaceMatches(const RootNamespace & owning, const PartManifest & body);

}
```

- [ ] **Step 2: Write `CasManifestCodec.cpp`.** Reuse `RunFile` for the entries and `CasCodecUtil`/`writeU128LE` for the scalars.

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/ReadHelpers.h>
#include <Common/Exception.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{

constexpr uint16_t kPartManifestFormatVersion = 1;

/// One entry's RunFile payload: placement u8, blob_hash 16B LE, blob_size u64 LE, inline_len u32, inline.
String encodeEntryPayload(const ManifestEntry & e)
{
    WriteBufferFromOwnString buf;
    writeBinaryLittleEndian(static_cast<uint8_t>(e.placement), buf);
    writeU128LE(buf, e.blob_hash);
    writeBinaryLittleEndian(e.blob_size, buf);
    writeBinaryLittleEndian(static_cast<uint32_t>(e.inline_bytes.size()), buf);
    buf.write(e.inline_bytes.data(), e.inline_bytes.size());
    return buf.str();
}

ManifestEntry decodeEntryPayload(const String & path, std::string_view payload)
{
    ReadBufferFromMemory in(payload.data(), payload.size());
    return decodeGuarded("PartManifest entry", [&]
    {
        ManifestEntry e;
        e.path = path;
        uint8_t placement_raw = 0;
        readBinaryLittleEndian(placement_raw, in);
        if (placement_raw != static_cast<uint8_t>(EntryPlacement::Inline)
            && placement_raw != static_cast<uint8_t>(EntryPlacement::Blob))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: unknown placement {}", placement_raw);
        e.placement = static_cast<EntryPlacement>(placement_raw);
        e.blob_hash = readU128LE(in);
        readBinaryLittleEndian(e.blob_size, in);
        uint32_t inline_len = 0;
        readBinaryLittleEndian(inline_len, in);
        e.inline_bytes = readFixedBytes(in, inline_len);
        return e;
    });
}

}

String encodePartManifest(const PartManifest & m)
{
    /// Canonical path order + duplicate-path rejection.
    std::vector<const ManifestEntry *> sorted;
    sorted.reserve(m.entries.size());
    for (const auto & e : m.entries)
        sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(),
              [](const ManifestEntry * a, const ManifestEntry * b) { return a->path < b->path; });
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i]->path == sorted[i - 1]->path)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: duplicate path '{}'", sorted[i]->path);

    WriteBufferFromOwnString out;
    /// header: magic "CAPT", format_version u16, writer_version u16
    const uint32_t magic = magicFor(FormatId::PartManifest);
    writeBinaryLittleEndian(magic, out);
    writeBinaryLittleEndian(kPartManifestFormatVersion, out);
    writeBinaryLittleEndian(static_cast<uint16_t>(currentWriterVersion()), out);
    /// ref: writer_instance_id is a String token ("<server_id_hex>:<process_epoch>"), len-prefixed
    writeBinaryLittleEndian(static_cast<uint32_t>(m.ref.writer_instance_id.size()), out);
    out.write(m.ref.writer_instance_id.data(), m.ref.writer_instance_id.size());
    writeBinaryLittleEndian(m.ref.build_sequence, out);
    writeU128LE(out, m.ref.manifest_instance_id);
    /// root_namespace_id (len-prefixed)
    writeBinaryLittleEndian(static_cast<uint32_t>(m.root_namespace_id.string().size()), out);
    out.write(m.root_namespace_id.string().data(), m.root_namespace_id.string().size());
    /// payload_digest
    writeU128LE(out, m.payload_digest);
    /// entries as an embedded RunFile (kind = ManifestEntries), key = path
    WriteBufferFromOwnString entries_buf;
    {
        RunHeader h;
        h.kind = RunKind::ManifestEntries;
        h.key_schema = 0;
        RunFileWriter w(entries_buf, h);
        for (const auto * e : sorted)
            w.append(e->path, encodeEntryPayload(*e));
        w.finish();
    }
    const String entries_bytes = entries_buf.str();
    writeBinaryLittleEndian(static_cast<uint64_t>(entries_bytes.size()), out);
    out.write(entries_bytes.data(), entries_bytes.size());
    return out.str();
}

PartManifest decodePartManifest(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    return decodeGuarded("PartManifest", [&]
    {
        uint32_t magic = 0;
        readBinaryLittleEndian(magic, in);
        if (magic != magicFor(FormatId::PartManifest))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: bad magic");
        uint16_t format_version = 0;
        readBinaryLittleEndian(format_version, in);
        checkCompatibility(format_version, "PartManifest");
        uint16_t writer_version = 0;
        readBinaryLittleEndian(writer_version, in);

        PartManifest m;
        uint32_t writer_len = 0;
        readBinaryLittleEndian(writer_len, in);
        m.ref.writer_instance_id = readFixedBytes(in, writer_len);
        readBinaryLittleEndian(m.ref.build_sequence, in);
        m.ref.manifest_instance_id = readU128LE(in);
        uint32_t ns_len = 0;
        readBinaryLittleEndian(ns_len, in);
        m.root_namespace_id = RootNamespace(readFixedBytes(in, ns_len));
        m.payload_digest = readU128LE(in);

        uint64_t entries_len = 0;
        readBinaryLittleEndian(entries_len, in);
        const String entries_bytes = readFixedBytes(in, entries_len);
        ReadBufferFromMemory entries_in(entries_bytes.data(), entries_bytes.size());
        RunFileReader r(entries_in);
        String path, payload;
        String prev_path;
        bool have_prev = false;
        while (r.next(path, payload))
        {
            if (have_prev && path == prev_path)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "PartManifest: duplicate path '{}'", path);
            m.entries.push_back(decodeEntryPayload(path, payload));
            prev_path = path;
            have_prev = true;
        }
        return m;
    });
}

UInt128 computePayloadDigest(const PartManifest & m)
{
    /// Digest the canonical encoding with payload_digest zeroed, so the digest does not depend on
    /// itself (it would be circular otherwise) and is stable for identical bodies, changing whenever
    /// ref / namespace / entries change. BLAKE3 over the deterministic encodePartManifest bytes.
    PartManifest probe = m;
    probe.payload_digest = UInt128{};
    const String bytes = encodePartManifest(probe);
    return blake3OfBytes(bytes);   /// CasIds.h BLAKE3 helper (the one blobKey hashing already uses)
}

bool refMatchesBody(const ManifestRef & journal_ref, const PartManifest & body)
{
    return journal_ref == body.ref;
}

bool manifestNamespaceMatches(const RootNamespace & owning, const PartManifest & body)
{
    return owning == body.root_namespace_id;
}

}
```
(Use the project's existing BLAKE3-of-bytes helper - the same one blob content hashing uses; if it is spelled differently in `CasIds.h`/`CasCodecUtil.h`, call that name. Do NOT introduce a second hash primitive.)

- [ ] **Step 3: Write `gtest_cas_manifest_codec.cpp`** - round-trip, byte-determinism, canonical path order, duplicate-path rejection, inline-vs-blob entries.

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <Common/Exception.h>

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

using namespace DB::Cas;

namespace
{

PartManifest sample()
{
    PartManifest m;
    m.ref = ManifestRef{"srv-a:7", 1042, DB::UInt128(0x7f3a)};
    m.root_namespace_id = RootNamespace("srv-a/uuid@cas@");
    m.payload_digest = DB::UInt128(0xDEAD);
    ManifestEntry blob;
    blob.path = "columns/data.bin";
    blob.placement = EntryPlacement::Blob;
    blob.blob_hash = (DB::UInt128(1) << 64) | DB::UInt128(2);
    blob.blob_size = 4096;
    ManifestEntry inl;
    inl.path = "checksums.txt";
    inl.placement = EntryPlacement::Inline;
    inl.inline_bytes = "hello-inline";
    m.entries = {blob, inl};   /// deliberately NOT path-sorted on input
    return m;
}

}

TEST(CasManifestCodec, RoundTrip)
{
    const PartManifest m = sample();
    const String bytes = encodePartManifest(m);
    const PartManifest got = decodePartManifest(bytes);
    EXPECT_EQ(got.ref, m.ref);
    EXPECT_EQ(got.root_namespace_id, m.root_namespace_id);
    EXPECT_EQ(got.payload_digest, m.payload_digest);
    ASSERT_EQ(got.entries.size(), 2u);
}

TEST(CasManifestCodec, EntriesInCanonicalPathOrder)
{
    const PartManifest m = sample();
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    /// "checksums.txt" < "columns/data.bin" byte-wise -> inline entry comes first after sorting.
    EXPECT_EQ(got.entries[0].path, "checksums.txt");
    EXPECT_EQ(got.entries[0].placement, EntryPlacement::Inline);
    EXPECT_EQ(got.entries[0].inline_bytes, "hello-inline");
    EXPECT_EQ(got.entries[1].path, "columns/data.bin");
    EXPECT_EQ(got.entries[1].placement, EntryPlacement::Blob);
    EXPECT_EQ(got.entries[1].blob_size, 4096u);
    EXPECT_EQ(got.entries[1].blob_hash, (DB::UInt128(1) << 64) | DB::UInt128(2));
}

TEST(CasManifestCodec, ByteDeterminism)
{
    const PartManifest m = sample();
    /// Encode twice -> identical bytes. Also encode a copy with entries pre-shuffled into the other
    /// order -> still identical, because the encoder sorts canonically.
    PartManifest m2 = m;
    std::swap(m2.entries[0], m2.entries[1]);
    EXPECT_EQ(encodePartManifest(m), encodePartManifest(m));
    EXPECT_EQ(encodePartManifest(m), encodePartManifest(m2));
}

TEST(CasManifestCodec, DuplicatePathRejectedOnEncode)
{
    PartManifest m = sample();
    m.entries.push_back(m.entries[0]);   /// duplicate path
    try
    {
        encodePartManifest(m);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasManifestCodec, BadMagicFailsClosed)
{
    String bytes = encodePartManifest(sample());
    bytes[0] ^= 0xFF;   /// corrupt the magic
    try
    {
        decodePartManifest(bytes);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasManifestCodec, TruncatedFailsClosed)
{
    const String bytes = encodePartManifest(sample());
    const String truncated = bytes.substr(0, bytes.size() / 2);
    try
    {
        decodePartManifest(truncated);
        FAIL() << "expected CORRUPTED_DATA";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::CORRUPTED_DATA);
    }
}

TEST(CasManifestCodec, EmptyEntriesRoundTrips)
{
    PartManifest m = sample();
    m.entries.clear();
    const PartManifest got = decodePartManifest(encodePartManifest(m));
    EXPECT_TRUE(got.entries.empty());
    EXPECT_EQ(got.ref, m.ref);
}

TEST(CasManifestCodec, PayloadDigestStableAndContentSensitive)
{
    const PartManifest m = sample();
    /// Stable for identical bodies, and independent of the payload_digest field itself.
    PartManifest with_digest = m;
    with_digest.payload_digest = DB::UInt128(0x1234);
    EXPECT_EQ(computePayloadDigest(m), computePayloadDigest(m));
    EXPECT_EQ(computePayloadDigest(m), computePayloadDigest(with_digest));
    /// Changing real content (an entry's blob size) changes the digest.
    PartManifest changed = m;
    changed.entries[0].blob_size += 1;
    EXPECT_NE(computePayloadDigest(m), computePayloadDigest(changed));
}
```

- [ ] **Step 4: Run the codec tests - must PASS.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasManifestCodec.*' 2>&1 | tail -25
```
Expected (analyze `build/build.log` via subagent; add `CasManifestCodec.cpp` to the CAS-core source list if the link fails):
```
[  PASSED  ] 8 tests.
```

- [ ] **Step 5: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp \
        src/Disks/tests/gtest_cas_manifest_codec.cpp
git commit -m "CA GC phase1a: PartManifest/ManifestEntry codec (deterministic, block-framed entries)

Entries embed a RunFile of kind ManifestEntries so large manifests are block-framed, not one
length-prefixed record per entry. Canonical path order; duplicate path => CORRUPTED_DATA.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 6: `refMatchesBody` / `manifestNamespaceMatches` - validation oracles {#task-6-refmatchesbody-manifestnamespacematches-validation-oracles}

**Files:**
- Modify: `src/Disks/tests/gtest_cas_manifest_codec.cpp` (add validation tests)

**Interfaces produced:** (tests for the two helpers declared in Task 5).

- [ ] **Step 1: Add validation tests to `gtest_cas_manifest_codec.cpp`.** Cover the match case and each mismatch case for both helpers.

```cpp
TEST(CasManifestCodec, RefMatchesBodyAcceptsExactRef)
{
    const PartManifest m = sample();
    /// The journal ref equals the body ref -> true.
    EXPECT_TRUE(refMatchesBody(m.ref, m));
}

TEST(CasManifestCodec, RefMatchesBodyRejectsEachFieldMismatch)
{
    const PartManifest m = sample();
    ManifestRef wrong_writer = m.ref; wrong_writer.writer_instance_id = m.ref.writer_instance_id + "x";
    ManifestRef wrong_seq = m.ref;    wrong_seq.build_sequence = m.ref.build_sequence + 1;
    ManifestRef wrong_inst = m.ref;   wrong_inst.manifest_instance_id = m.ref.manifest_instance_id + DB::UInt128(1);
    EXPECT_FALSE(refMatchesBody(wrong_writer, m));
    EXPECT_FALSE(refMatchesBody(wrong_seq, m));
    EXPECT_FALSE(refMatchesBody(wrong_inst, m));
}

TEST(CasManifestCodec, ManifestNamespaceMatchesAcceptsOwningNs)
{
    const PartManifest m = sample();
    EXPECT_TRUE(manifestNamespaceMatches(m.root_namespace_id, m));
}

TEST(CasManifestCodec, ManifestNamespaceMatchesRejectsForeignNs)
{
    const PartManifest m = sample();
    EXPECT_FALSE(manifestNamespaceMatches(RootNamespace("srv-b/other@cas@"), m));
    /// A namespace that is a prefix but not equal is still a mismatch (no loose comparison).
    EXPECT_FALSE(manifestNamespaceMatches(RootNamespace("srv-a/uuid"), m));
}
```

- [ ] **Step 2: Run the validation tests - must PASS.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasManifestCodec.RefMatchesBody*:CasManifestCodec.ManifestNamespaceMatches*' 2>&1 | tail -20
```
Expected:
```
[  PASSED  ] 4 tests.
```

- [ ] **Step 3: Commit.**

```bash
git add src/Disks/tests/gtest_cas_manifest_codec.cpp
git commit -m "CA GC phase1a: refMatchesBody/manifestNamespaceMatches gtests (match + each mismatch)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 7: `CasLayout::manifestKey` + `_manifests` reservation {#task-7-caslayout-manifestkey-manifests-reservation}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h`
- Modify: `src/Disks/tests/gtest_cas_layout.cpp`

**Interfaces produced:** `Layout::manifestKey`; `_manifests` rejection in `checkNamespace`.

- [ ] **Step 1: Add `manifestKey` to `CasLayout.h`.** Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>` at the top, and add the method in the public section (e.g. right after `namespaceFilesPrefix`).

```cpp
    /// Root-local part manifest body key (spec §S3 Layout):
    ///   <prefix>/roots/<ns>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto
    /// `<writer_instance_id>` is the ManifestRef's String token ("<server_id_hex>:<process_epoch>"),
    /// used verbatim - NOT hex-rendered. `<manifest_instance_id>` is 32-char lowercase hex of the
    /// 128-bit field; `<aa>` = first 2 hex chars of `manifest_instance_id`. `root_namespace_id` comes
    /// from the owning context (the `ManifestId`), never from the journal ref.
    String manifestKey(const ManifestId & id) const
    {
        checkNamespace(id.root_namespace);
        const String inst_hex = u128ToHex(id.ref.manifest_instance_id);
        return prefix + "/roots/" + id.root_namespace.string() + "/_manifests/"
             + id.ref.writer_instance_id + "/" + std::to_string(id.ref.build_sequence) + "/"
             + manifestAa(id.ref) + "/" + inst_hex + ".proto";
    }
```

- [ ] **Step 2: Reserve `_manifests` in `checkNamespace`.** In the segment loop, add a rejection mirroring the `_files` one (place it right after the `_files` check):

```cpp
            if (segment == "_manifests")
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_manifests'", s);
```

- [ ] **Step 3: Add tests to `gtest_cas_layout.cpp`.** Append a `manifestKey` shape test and a `_manifests` rejection test. Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>` to the test file's includes.

```cpp
TEST(CasLayout, ManifestKeyShape)
{
    Layout l("p");
    ManifestId id;
    id.root_namespace = RootNamespace("srv-a/3f2e-uuid@cas@");
    id.ref.writer_instance_id = "ab:7";                    /// String token, used verbatim
    id.ref.build_sequence = 1042;
    id.ref.manifest_instance_id = (DB::UInt128(0x7f3aULL) << 112);   /// top bytes 7f 3a -> aa = "7f"
    const String key = l.manifestKey(id);
    /// <prefix>/roots/<ns>/_manifests/<writer_instance_id>/<build_seq>/<aa>/<inst_hex>.proto
    EXPECT_EQ(key,
        "p/roots/srv-a/3f2e-uuid@cas@/_manifests/"
        "ab:7/1042/7f/"
        "7f3a0000000000000000000000000000.proto");
}

TEST(CasLayout, ManifestsSegmentReserved)
{
    Layout l("p");
    ManifestId bad;
    bad.root_namespace = RootNamespace("srv-a/_manifests/x");
    EXPECT_THROW(l.manifestKey(bad), DB::Exception);
    /// Also rejected as a generic namespace segment via rootShardKey.
    EXPECT_THROW(l.rootShardKey(RootNamespace{"srv-a/_manifests/tbl"}, 0), DB::Exception);
    /// A segment that merely CONTAINS "_manifests" as a substring is still legal (no false positive).
    EXPECT_NO_THROW(l.rootShardKey(RootNamespace{"my_manifests/tbl"}, 0));
}
```

- [ ] **Step 4: Run the layout tests - must PASS.**

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; \
build/src/unit_tests_dbms --gtest_filter='CasLayout.*' 2>&1 | tail -25
```
Expected:
```
[  PASSED  ] 8 tests.
```
(6 pre-existing `CasLayout.*` tests + the 2 new ones; the exact count is whatever the suite reports - the requirement is `[  PASSED  ]` with no failures.)

- [ ] **Step 5: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/tests/gtest_cas_layout.cpp
git commit -m "CA GC phase1a: CasLayout::manifestKey + reserve _manifests in checkNamespace

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

### Task 8: Full build + `Cas*`/`Ca*` gtest sweep (phase exit) {#task-8-full-build-cas-ca-gtest-sweep-phase-exit}

**Files:** none (verification + commit only).

- [ ] **Step 1: Clean-ish full build of `unit_tests_dbms`.** Redirect to the build log; analyze via subagent.

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && ninja -C build unit_tests_dbms > build/build.log 2>&1; echo "ninja exit=$?"
```
Then **dispatch a subagent** (general-purpose) to read `build/build.log` and report only: did the link of `unit_tests_dbms` succeed, and any errors/warnings touching `CasManifest*`, `CasRunFile`, `CasFormat`, or `CasLayout`. Expected subagent summary: build succeeded, `unit_tests_dbms` linked, no CAS errors. (If a `CasManifestCodec`/`CasRunFile` symbol is undefined at link, add the two `.cpp` files to the CAS-core source list in the appropriate `CMakeLists.txt`, rebuild, and re-analyze.)

- [ ] **Step 2: Run the entire CA gtest sweep.** Redirect to a unique log; analyze via subagent.

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && \
build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/test_phase1a.log 2>&1; echo "tests exit=$?"
```
Then **dispatch a subagent** to read `build/test_phase1a.log` and report only: the final `[  PASSED  ]` / `[  FAILED  ]` line, the total test count, and the names of any failed tests. Expected subagent summary: all `Cas*`/`Ca*` tests pass (`[  PASSED  ]`), including the new `CasManifestId.*`, `CasRunFile.*`, `CasManifestCodec.*`, the extended `CasFormat.*`, and `CasLayout.*`. No failures.

- [ ] **Step 3: Confirm no placeholders shipped.** Grep the new/modified files for accidental stubs:

Run:
```bash
cd /home/mfilimonov/workspace/ClickHouse/master && \
grep -rnE 'TODO|FIXME|placeholder|NotImplemented|throw .*not implemented' \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.cpp || echo "no placeholders"
```
Expected: `no placeholders`.

- [ ] **Step 4: Commit the phase-exit marker** (empty if nothing changed, otherwise any source-list CMake edit made in Step 1).

```bash
git add -A
git commit -m "CA GC phase1a: build green + full Cas*/Ca* gtest sweep green (phase exit)

Phase 1a foundation complete: ManifestRef/ManifestId, PartManifest codec, RunFile/RunMerger,
CasLayout::manifestKey + _manifests reservation, FormatId 12/13/14. Gates Phases 1b/1c/1d.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Self-Review {#self-review}

- **Spec coverage - §Object Identity And Ownership:** `ManifestRef` (writer_instance_id, build_sequence, manifest_instance_id) and namespace-qualified `ManifestId` are Task 1; the body's self-described `ref`/`root_namespace_id` and `payload_digest` (integrity/debug only) are the `PartManifest` of Task 5; `refMatchesBody`/`manifestNamespaceMatches` (the two fail-closed read/fold checks) are Task 5 + Task 6; canonical-path-order entries and duplicate-path-is-corruption are Task 5. `NoManifestIdReuse`/`SingleManifestOwner` are protocol/owner-transition invariants enforced in Phases 1b/1d, not format-layer, so they are out of scope here (correctly). ✓
- **Spec coverage - §S3 Layout:** `manifestKey` shape `<prefix>/roots/<ns>/_manifests/<writer_instance_id>/<build_sequence>/<aa>/<manifest_instance_id>.proto` with `<aa>` from `manifest_instance_id` is Task 7; `_manifests` rejected by `checkNamespace` is Task 7. Blob key is unchanged (not touched). ✓
- **Spec coverage - §Backpressure And Journal Encoding:** the `RunFile`/`DataBlock`/`RunFooter` dense block-framed sorted run (RunHeader{magic,version,kind,key_schema,codec,block_size}; per-block CRC32C; sparse footer index; deterministic bytes; bounded `O(inputs*block_size)` merge) is Tasks 3-4; `PartManifest` `entries` reuse the same block-framed run (`kind = ManifestEntries`) rather than one length-prefixed protobuf per entry, per the spec's explicit instruction, in Task 5. The backpressure *thresholds* (OQ7) are enforced at publish time in Phase 1b, not here. ✓
- **Resolved Open Questions honored:** OQ1 - `PartManifest` carries exactly ref/root_namespace_id/payload_digest/entries, no extra debug fields. OQ3 - no `DirectoryIndex` type defined (deferred). OQ5 - 256 KiB target / 1 MiB cap / CRC32C / sparse footer / compression off / fixed-width hashes / deterministic, all in `CasRunFile`. ✓
- **Type-name consistency with the contract:** `ManifestRef`, `ManifestId`, `manifestAa`, `EntryPlacement{Inline=1,Blob=2}`, `ManifestEntry`, `PartManifest`, `encodePartManifest`/`decodePartManifest`, `refMatchesBody`/`manifestNamespaceMatches`, `RunKind{BlobInDegree=1,BlobDelta=2,SourceEdge=3,ManifestEntries=4,TargetShardDelta=5}`, `RunHeader`, `RunFileWriter`/`RunFileReader`/`RunMerger`, `Layout::manifestKey`, `FormatId::PartManifest=12`/`RunFile=13`/`FoldSeal=14`/`CompletionSeal=15` - all match verbatim. The only deviation is the `PartManifest` **magic** ("CAPT" not "CAPM"), which is forced by a real collision and is flagged in [Magic Collision](#magic-collision); the **type name and enum value** are unchanged. ✓
- **Placeholder scan:** every code step shows full code; every run step shows the exact command and the expected `[  PASSED  ]` (or `no placeholders`); Task 8 Step 3 greps for stubs. ✓
- **Allman braces:** all shown C++ uses opening brace on its own line. ✓

**Could-not-turn-into-a-task / requires a decision:** the contract's `FormatId::PartManifest` magic "CAPM" collides with the existing `FormatId::PoolMeta` magic "CAPM" (asserted in `gtest_cas_format.cpp` and documented in `cas_format.proto`). This plan keeps the enum value (`= 12`) and assigns the non-colliding magic "CAPT", flagged for veto in [Magic Collision](#magic-collision). Everything else in the contract maps to a concrete task.
