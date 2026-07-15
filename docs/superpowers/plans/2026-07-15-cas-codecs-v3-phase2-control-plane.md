---
description: 'Implementation plan for CAS codecs v3 phase 2: converting the eight control-plane objects (pool meta, GC state + heartbeat, outcomes, fold seal, owner/epoch/mount-lease) from protobuf to the phase-1 text file shape, then deleting the protobuf graveyard; plus the phases 3-8 pipelining DAG.'
sidebar_label: 'CAS codecs v3 phase 2 plan'
sidebar_position: 62
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase2-control-plane
title: 'CAS Codecs V3 — Phase 2: Control-Plane Text Cutover'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 2: Control-Plane Text Cutover Implementation Plan {#cas-codecs-v3-phase2}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert every **control-plane** CAS object from protobuf to the phase-1 text file shape, one object per commit, each independently green: `cas_pool_meta`, `cas_gc_state` + `cas_gc_hb` (the last unversioned object dies), `cas_gc_outcomes`, `cas_fold_seal`, and the three server-root singletons `cas_owner` / `cas_epoch` / `cas_mount_lease`. Wire structs move **with** their codecs into `Core/Formats/`; the protobuf message for each object is deleted as its text codec lands; every new format gains a `cas_format_test_battery.h` row. The protobuf **build wiring** (`Proto/`, `clickhouse_cas_proto`, `protobuf_generate_cpp`, the `libprotoc` link) is NOT removed here — that is phase 8, after every phase-3–7 object has also left protobuf.

**Architecture:** Each control object becomes `header line {"type":"cas_<x>","v":N}` + a materialized body, wrapped by the phase-1 helpers (`writeHeaderLine` / `expectHeaderLine` / `JsonObjectReader` / `sealObject` / `openObject`). Codecs are key-mapping + invariants on top of `CasTextFormat` — they never touch `CasBackend`/`CasStore` (physical layering, phase 1). Shared value sub-objects (`Token`, `BlobRef`, and the small enums) get one canonical JSON rendering in a shared wire-value vocabulary so outcomes (this phase) and refsnaplog / blob-meta (phases 3–4) don't each reinvent it. Spec: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md` §migration-order step 2; reference: `docs/superpowers/cas/codecs_proposal_v3.md` §control-plane. Phase-1 foundation (authoritative): `Core/Formats/CasFormat.{h,cpp}`, `Core/Formats/CasTextFormat.{h,cpp}`, `src/Disks/tests/cas_format_test_battery.h`.

**Tech Stack:** C++ (ClickHouse `dbms`), the phase-1 `CasTextFormat` helpers, `ReadHelpers`/`WriteHelpers` JSON primitives, libzstd one-shot (already wired), gtest (`unit_tests_dbms`, auto-globbed).

## Global Constraints {#global-constraints}

- **Allman braces** everywhere (CI style check).
- **Layering (physical, phase-1 rule):** files in `Core/Formats/` may include only other `Formats/` headers, the identifier vocabulary (`Core/CasIds.h`, `Core/CasToken.h`, `Core/CasBlobRef.h`, `Core/CasBlobDigest.h`, `Core/CasBlobHasher.h`, `Core/CasRefIds.h`, `Core/CasManifestId.h`, `Core/CasEnvelope.h` for `ObjectKind`/`ProvenanceOp`), `base/`, `src/IO/`, `src/Common/`, `src/Formats/FormatSettings.h`, `<zstd.h>`. NEVER `CasBackend.h`, `CasStore.h`, `CasLayout.h`, or any subsystem header. A codec that wants a backend must not compile. Protocol logic that needs a `Backend`/`Layout` (`PoolMeta::createOrValidate`, `claimMount`, `allocateWriterEpoch`, the `MountLeaseKeeper`) STAYS in `Core/` and includes the new `Formats/` header; the `Formats/` header only **forward-declares** `class Backend; class Layout;`.
- **Pre-release, hard cutover per object:** no persisted data exists (`feedback` 2026-06-24) — each object flips from protobuf to text in ONE commit with NO dual-read arm. A half-migrated tree is fine because every object is CAS-by-token or write-once and cuts over atomically. Do NOT add a "try proto then text" fallback (violates the fail-close doctrine and the no-compat-scaffolding rule).
- **Error taxonomy (phase-1):** malformed / truncated / over-cap / wrong-type / duplicate-key / bad-hex / whitespace / cap-hit → `CORRUPTED_DATA`; future `v` / unknown `!`-prefixed key → `UNKNOWN_FORMAT_VERSION`. No other codes on decode paths. `JsonObjectReader` already narrows the `ReadHelpers` parse codes to `CORRUPTED_DATA`; per-object decoders throw `CORRUPTED_DATA` directly for their own invariant violations. Pool-meta's mount gates keep throwing `UNKNOWN_FORMAT_VERSION` (forward + backward floor) — those are the ONE decoder that deliberately emits it beyond the header gate.
- **`v` stamping stays at `G_BUILD` = 3.** Phase 2 introduces no breaking format generation, so `writeHeaderLine` stamps 3 and there is NO `G_BUILD` 3→4 bump and NO `changePoints` append. (A future breaking change to any of these text formats is what earns the bump; not this cutover.)
- **Pinned JSON write settings (Phase-1 review finding — HARD requirement).** Phase-1's `CasTextFormat.cpp::writeStringValue` calls `writeJSONString(s, out, jsonWriteSettings())` with a **default-constructed** static `FormatSettings`, and `writeJSONString` consults `FormatSettings::JSON::escape_forward_slashes` (default `true`, `src/Formats/FormatSettings.h:270`) — so whether a `/` in a string value is emitted as `/` or `\/` is tied to a ClickHouse global default that Phase 1 does not own. CAS text values are dense with `/` (ref-paths like `t-1/all_1_2_0`, fold-seal map keys `ns/shard`, run keys `gc/.../run0`). This makes `cas_fold_seal` byte-determinism (the `putDeterministicArtifact` retry-equality contract) and every golden string in this plan hostage to a setting CAS does not control. **Task 1 Step 0 pins the specific `FormatSettings::JSON` fields as CAS-owned constants** (`escape_forward_slashes = false` — also the more readable form for the `jq`/`less` design goal) and adds a test that goes RED if the global default ever leaks back in. Every golden in this plan assumes the pinned form (`/` unescaped).
- **Canonical text only** (phase-1): writers emit no whitespace outside JSON strings; every line ends `\n`. Keys 2–5 chars; hashes/ids = lowercase fixed-width hex strings; unbounded `u64` = decimal strings; structurally-bounded ints (counts, lengths, ms timestamps, round/generation/epoch/seq counters that are protocol-bounded well under 2^53) = JSON numbers. **Exception:** values that are genuinely full-range `u64` sentinels — `min_active`/`oldest_nonpending_condemn_round` use `UINT64_MAX` as "none" — are decimal STRINGS (they reach 2^64-1).
- **Determinism (`cas_fold_seal`):** `FoldSeal` is `PinnedRaw` + `Strict` (byte-adoption via `putDeterministicArtifact`). Its writer MUST be byte-deterministic: fixed field order, every collection emitted in sorted order (sort inside the encoder — never trust caller vector order), strict keys, no compression. A golden **byte** snapshot pins it. `cas_run` (phase 5) is the only other deterministic format; it is out of scope here.
- **Battery is a hard gate:** every new format registers exactly one `FormatBatteryCase` row (`src/Disks/tests/gtest_cas_format_battery.cpp` or a per-object gtest that calls `runFormatBattery`). A format without a battery row fails review. The phase-1 **toy** proving instance (`FormatId::PoolMeta` toy in `gtest_cas_format_battery.cpp`) is REPLACED by the real `cas_pool_meta` case in Task 3 — do not leave both.
- **Build:** `flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p2t<N>.log 2>&1; echo "NINJA_EXIT=$?"` — foreground only, no `-j`, no `nproc`, redirect to a per-task log in the build dir, read back `NINJA_EXIT=`. Substitute the real configured build dir (`ls -d build*`; examples use `build_debug`). Use a subagent to analyze any build log and return only a summary.
- **Commits:** commit after every task; never rebase/amend; branch `cas-gc-rebuild`; explicit-path `git add` (never `git add -A` on this shared worktree — other agents' files must not be swept in), and verify `git log -1 --stat` names only your files before moving on. Commit trailer on every commit:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## Interfaces consumed from phase 1 {#phase1-interfaces}

The real signatures in `Core/Formats/CasTextFormat.h` (do not re-declare — include it):

```cpp
namespace DB::Cas
{
/// write-side JSON micro-vocabulary
void writeKey(WriteBuffer & out, std::string_view key, bool & first);
void writeStringValue(WriteBuffer & out, std::string_view s);
void writeHex128Value(WriteBuffer & out, const UInt128 & v);
void writeU64StringValue(WriteBuffer & out, uint64_t v);
void closeObject(WriteBuffer & out, bool & first);

/// read-side pull cursor over ONE canonical JSON object (ctor consumes '{')
class JsonObjectReader
{
public:
    JsonObjectReader(ReadBuffer & in_, KeyStrictness strictness_, std::string_view what_);
    bool nextKey(String & key);        /// false when '}' consumed; duplicate key -> CORRUPTED_DATA
    String readString();
    UInt128 readHex128();              /// exactly 32 lowercase hex or CORRUPTED_DATA
    uint64_t readU64String();          /// decimal u64 in a JSON string
    uint64_t readU64Number();          /// bare JSON number
    bool readBool();
    void skipUnknown(const String & key); /// '!'-key -> UNKNOWN_FORMAT_VERSION; Strict -> CORRUPTED_DATA; else skip
};

/// header / trailer / raw line
struct TextHeader { String type; uint32_t v = 0; };
void writeHeaderLine(WriteBuffer & out, FormatId id);            /// {"type":"…","v":3}\n
void writeTrailerLine(WriteBuffer & out, uint64_t n);            /// {"n":N}\n
TextHeader expectHeaderLine(ReadBuffer & in, FormatId id);       /// type check + v gate, first
std::optional<TextHeader> sniffHeaderLine(std::string_view bytes);
String readLine(ReadBuffer & in, uint64_t line_cap, std::string_view what); /// excl. '\n'

/// zstd arm (per-type policy; Always -> one frame, else identity)
bool looksZstd(std::string_view bytes);
String sealObject(FormatId id, String text);
String openObject(FormatId id, std::string_view stored);
}
```

And the registry (`Core/Formats/CasFormat.h`), all present and frozen from phase 1: `enum class FormatId` with `PoolMeta=8, GcOutcomes=11, FoldSeal=14, Owner=16, ServerEpoch=17, MountLease=18, RefLog=19, RefSnapshot=20, BlobMeta=21, GcHeartbeat=22`; `enum class TextFamily / KeyStrictness / CompressionPolicy`; `struct FormatTraits`; `const FormatTraits & traitsFor(FormatId)`; `const FormatTraits * traitsForType(std::string_view)`; `std::string_view storedSuffix(FormatId)`; `currentCompatibilityVersion()`; `checkCompatibility(uint32_t, std::string_view)`; the phase-1 `TRAITS` table already carries the family/strictness/compression/caps for every phase-2 object. **Phase 2 does not edit the `TRAITS` table** (values already correct) except where a task note calls it out.

The battery (`src/Disks/tests/cas_format_test_battery.h`, phase 1):

```cpp
struct FormatBatteryCase
{
    DB::Cas::FormatId id;
    std::function<String()> encode;               /// full STORED object bytes (post-sealObject)
    std::function<void(std::string_view)> decode; /// must succeed on encode() output
    String golden;                                /// pinned canonical TEXT ("" = skip golden)
};
void runFormatBattery(const FormatBatteryCase & c);
```

## Object-to-codec-file map {#object-file-map}

Per the spec §code-placement, and grounded in the current code (the wire struct + `encodeX`/`decodeX` move **with** each codec; protocol logic that needs a `Backend`/`Layout` STAYS in `Core/`):

| Object | New `Formats/` file | Struct(s) moved from | Protocol logic that STAYS in `Core/` |
|---|---|---|---|
| `cas_pool_meta` | `CasPoolMetaFormat.{h,cpp}` | `Core/CasPoolMeta.h` (`PoolMeta`) | `PoolMeta::createOrValidate` + `admitOrValidate` → `Core/CasPoolMeta.cpp` |
| `cas_owner` / `cas_epoch` / `cas_mount_lease` | `CasServerRootFormats.{h,cpp}` | `Core/CasServerRoot.h` (`OwnerObject`, `ServerEpoch`, `MountLease`) | `claimOwnerOrThrow`, `allocateWriterEpoch`, `claimMount*`, `MountLeaseKeeper`, `computeHeartbeatFloor`, `listMounts` → stay in `Core/CasServerRoot.{h,cpp}` |
| `cas_gc_state` + `cas_gc_hb` | `CasGcStateFormat.{h,cpp}` | `Core/CasGcFormats.h` (`GcLease`, `GcState`, `GcHeartbeat`) | GC round/lease logic in `Core/CasGc.cpp`; `RetiredEntry` (in-memory only) relocates to `Core/CasBlobInDegree.h` |
| `cas_gc_outcomes` | `CasGcOutcomesFormat.{h,cpp}` | `Core/CasGcOutcomes.h` (`OutcomeKind`, `OutcomeEntry`, `OutcomeLog`) | outcome recheck/verify in `Core/CasGc.cpp` |
| `cas_fold_seal` | `CasFoldSealFormat.{h,cpp}` | `Core/CasGenerationSeal.h` (`RunRef`, `ShardCoverage`, `CondemnedSummary`, `RefNsCleanupItem`, `RefNsCleanupState`, `CasFoldSeal`) | fold/seal logic in `Core/CasGc.cpp` |

Every `encodeX`/`decodeX` keeps its EXACT current signature (`String encodeX(const X &)` / `X decodeX(std::string_view)`) so all call sites compile unchanged — only the bytes on the wire and the codec's own file change. The mechanical include-rewrite after each struct move is: `grep -rl 'ContentAddressed/Core/<OldHeader>.h' src/ | xargs sed -i 's|ContentAddressed/Core/<OldHeader>.h|ContentAddressed/Core/Formats/<NewHeader>.h|g'` (with `RetiredEntry`'s new home added by hand where `CasGcFormats.h` was included only for it).

**Field-to-JSON-key policy for phase 2** (registry rows land with each task):

- Enum values are **full words** (spec universal convention): token type `etag`/`generation`/`emulated`; blob-hash algo `ch128`/`xxh3`/`sha256` (the existing `blobHashAlgoName` path names); object kind `blob`; outcome `deleted`/`absent`/`replaced`/`spared`; ref-ns-cleanup state `pending`/`completed`. The shared word-maps live in `Formats/CasWireVocab` (Task 1).
- **Hashes / uuids** (`pool_id`, `lease.owner`, `server_uuid`, heartbeat `by`) → 32-char lowercase hex strings (`writeHex128Value`/`readHex128`).
- **Genuinely-unbounded `u64` counters** (epochs, ref-sequences, `round`, `generation`, `parent_generation`, `seq`, `hb_seq`, `snap_generation`, `snap_pruned_through`, `snap_attempt`, `next_writer_epoch`) → decimal **strings** (`writeU64StringValue`/`readU64String`), per the proto3-JSON convention.
- **Structurally-bounded ints** (`blob_header_len ∈ [96,16384]`, `min_reader_generation` a small generation, `gc_shards ≥ 1`, `pid`, `started_at_ms`/`expires_at_ms` ms-timestamps < 2^53, `classification` 0/1/2/4, `shard`, condemned/pending totals) → JSON **numbers** (`writeIntText`/`readU64Number`).
- **`UINT64_MAX`-sentinel fields** (`min_active`, `oldest_nonpending_condemn_round`) → decimal **strings** (they reach 2^64-1).
- **`BlobRef`** → two sibling keys `ha` (algo word) + `h` (hex at the algo's width, via `codecFor(algo).toHex`). **`Token`** → two sibling keys `tt` (type word) + `tv` (raw string). **`RefTxnId`** → two sibling keys (epoch + seq decimal strings) because `{0,0}` is a legal "nothing folded yet" value and `renderRefTxnId` rejects zero. Phase 2 uses **no nested JSON objects** — every record is a flat object, so the phase-1 flat `JsonObjectReader` needs no array/nesting extension. Lists (`algos_used`) are comma-joined word strings (tiny, ≤3 entries), split on decode.

---

### Task 1: Shared wire-value vocabulary (`Formats/CasWireVocab`) {#task1}

**Files:**
- Create: `Core/Formats/CasWireVocab.h`, `Core/Formats/CasWireVocab.cpp`
- Test: `src/Disks/tests/gtest_cas_wire_vocab.cpp` (new)

**Interfaces:**
- Consumes: `CasToken.h` (`Token`, `TokenType`), `CasBlobRef.h` (`BlobRef`, `codecFor`, `blobHashAlgoName`), `CasBlobHasher.h` (`BlobHashAlgo`), `CasEnvelope.h` (`ObjectKind`), phase-1 `CasTextFormat.h` (`writeKey`, `writeStringValue`, `JsonObjectReader`).
- Produces (frozen interface phases 3/4/5/7 draft against):

```cpp
namespace DB::Cas
{
/// enum <-> canonical word (fail-closed reverse maps: unknown word -> CORRUPTED_DATA naming `what`)
std::string_view tokenTypeToWord(TokenType t);
TokenType        tokenTypeFromWord(std::string_view w, std::string_view what);
BlobHashAlgo     blobHashAlgoFromWord(std::string_view w, std::string_view what); /// write side reuses blobHashAlgoName
std::string_view objectKindToWord(ObjectKind k);
ObjectKind       objectKindFromWord(std::string_view w, std::string_view what);

/// sibling-key writers (append ,"tt":"..","tv":".." etc. to an in-progress object)
void writeTokenFields(WriteBuffer & out, bool & first, const Token & t);      /// keys: tt, tv
void writeBlobRefFields(WriteBuffer & out, bool & first, const BlobRef & r);   /// keys: ha, h
}
```

- [ ] **Step 0: Pin CAS JSON write settings (Phase-1 review finding — do this FIRST).** Phase-1's `Core/Formats/CasTextFormat.cpp` has:

```cpp
const FormatSettings & jsonWriteSettings()
{
    static const FormatSettings settings;   /// <-- default-constructed: escaping tied to a global default
    return settings;
}
```

`writeJSONString` reads `settings.json.escape_forward_slashes` (default `true`, `src/Formats/FormatSettings.h:270`), so `/` in a CAS string value would be emitted as `\/` — hostage to a ClickHouse-wide default. Replace it with a CAS-owned pin (own the fields `writeJSONString` consults; `escape_forward_slashes = false` gives the readable, `/`-unescaped form every golden in this plan assumes):

```cpp
const FormatSettings & jsonWriteSettings()
{
    /// CAS owns the JSON escaping alphabet for its text codecs — NOT the global default. cas_fold_seal
    /// byte-determinism (putDeterministicArtifact retry-equality) and every golden text file depend on
    /// this being independent of FormatSettings::JSON's ClickHouse-wide defaults (Phase-1 review finding).
    static const FormatSettings settings = []
    {
        FormatSettings s;
        s.json.escape_forward_slashes = false;   /// '/' stays '/': readable paths + stable bytes
        return s;
    }();
    return settings;
}
```

Add the drift guard to the phase-1 test file `src/Disks/tests/gtest_cas_text_format.cpp`:

```cpp
TEST(CasTextValueEscaping, ForwardSlashPinnedUnescaped)
{
    /// Goes RED if the global escape_forward_slashes default ever leaks back into CAS string values.
    /// CAS values are dense with '/' (ref-paths, fold-seal keys); their bytes must be CAS-owned.
    DB::WriteBufferFromOwnString out;
    writeStringValue(out, "ns/shard/all_1_2_0");
    out.finalize();
    EXPECT_EQ(out.str(), "\"ns/shard/all_1_2_0\"");
}
```

Build + run `unit_tests_dbms --gtest_filter='CasTextValueEscaping*'` → green. Commit this pin as its own small commit (`cas: formats v3 phase 2 — pin CAS JSON write settings (Phase-1 review finding)` + trailer) BEFORE the vocab work below, since every codec's `writeStringValue`/`writeBlobRefFields` depends on it and Task 1's own golden (`"ha":"ch128"`, etc.) is only correct under the pin.

- [ ] **Step 1: Failing test** — create `src/Disks/tests/gtest_cas_wire_vocab.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>

using namespace DB::Cas;

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

TEST(CasWireVocab, EnumWordsRoundTrip)
{
    for (TokenType t : {TokenType::ETag, TokenType::Generation, TokenType::Emulated})
        EXPECT_EQ(tokenTypeFromWord(tokenTypeToWord(t), "t"), t);
    for (BlobHashAlgo a : {BlobHashAlgo::CityHash128, BlobHashAlgo::XXH3_128, BlobHashAlgo::Sha256})
        EXPECT_EQ(blobHashAlgoFromWord(blobHashAlgoName(a), "a"), a);
    EXPECT_EQ(objectKindFromWord(objectKindToWord(ObjectKind::Blob), "k"), ObjectKind::Blob);
    EXPECT_THROW(tokenTypeFromWord("nope", "t"), DB::Exception);
    EXPECT_THROW(blobHashAlgoFromWord("nope", "a"), DB::Exception);
}

TEST(CasWireVocab, SiblingFieldsWriteAndReadBack)
{
    DB::WriteBufferFromOwnString out;
    bool first = true;
    writeTokenFields(out, first, Token{"etag-abc\"x", TokenType::ETag});
    const BlobRef ref{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hexToU128("00112233445566778899aabbccddeeff"))};
    writeBlobRefFields(out, first, ref);
    closeObject(out, first);
    out.finalize();
    EXPECT_EQ(out.str(),
        R"({"tt":"etag","tv":"etag-abc\"x","ha":"ch128","h":"00112233445566778899aabbccddeeff"})");

    DB::ReadBufferFromMemory in(out.str().data(), out.str().size());
    JsonObjectReader r(in, KeyStrictness::Tolerant, "t");
    String key, tv, ha, h; TokenType tt{};
    while (r.nextKey(key))
    {
        if (key == "tt") tt = tokenTypeFromWord(r.readString(), "t");
        else if (key == "tv") tv = r.readString();
        else if (key == "ha") ha = r.readString();
        else if (key == "h") h = r.readString();
        else r.skipUnknown(key);
    }
    EXPECT_EQ(tt, TokenType::ETag);
    EXPECT_EQ(tv, "etag-abc\"x");
    const BlobRef back{blobHashAlgoFromWord(ha, "a"), codecFor(blobHashAlgoFromWord(ha, "a")).fromHex(h)};
    EXPECT_EQ(back, ref);
}
```

- [ ] **Step 2: Register + verify compile failure** — add `add_headers_and_sources` already covers `Core/Formats` (phase-1 Task 1). Build; expect `NINJA_EXIT=1` (`CasWireVocab.h` missing).

- [ ] **Step 3: Implement** — `Core/Formats/CasWireVocab.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <IO/WriteBuffer.h>
#include <string_view>

namespace DB::Cas
{

/// Shared JSON rendering of the value sub-types every text codec embeds (spec §object-dispositions:
/// "subformat wire shapes become JSON sub-objects of their parents under the same key-naming
/// policy"). Enum values render as full WORDS (spec universal convention); the reverse maps are
/// fail-closed. Consumed by cas_gc_outcomes here and by refsnaplog / runs / blob envelope later.

std::string_view tokenTypeToWord(TokenType t);
TokenType tokenTypeFromWord(std::string_view w, std::string_view what);

/// Write side reuses blobHashAlgoName (CasBlobHasher.h) directly; this is the fail-closed inverse.
BlobHashAlgo blobHashAlgoFromWord(std::string_view w, std::string_view what);

std::string_view objectKindToWord(ObjectKind k);
ObjectKind objectKindFromWord(std::string_view w, std::string_view what);

/// Append `,"tt":"<type-word>","tv":"<value>"` to an in-progress object (the caller owns `first`).
void writeTokenFields(WriteBuffer & out, bool & first, const Token & t);
/// Append `,"ha":"<algo-word>","h":"<hex-at-algo-width>"`.
void writeBlobRefFields(WriteBuffer & out, bool & first, const BlobRef & r);

}
```

`Core/Formats/CasWireVocab.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

std::string_view tokenTypeToWord(TokenType t)
{
    switch (t)
    {
        case TokenType::ETag:       return "etag";
        case TokenType::Generation: return "generation";
        case TokenType::Emulated:   return "emulated";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS wire: unknown TokenType {}", static_cast<int>(t));
}

TokenType tokenTypeFromWord(std::string_view w, std::string_view what)
{
    if (w == "etag")       return TokenType::ETag;
    if (w == "generation") return TokenType::Generation;
    if (w == "emulated")   return TokenType::Emulated;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown token type '{}'", what, w);
}

BlobHashAlgo blobHashAlgoFromWord(std::string_view w, std::string_view what)
{
    if (w == "ch128")  return BlobHashAlgo::CityHash128;
    if (w == "xxh3")   return BlobHashAlgo::XXH3_128;
    if (w == "sha256") return BlobHashAlgo::Sha256;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown blob hash algo '{}'", what, w);
}

std::string_view objectKindToWord(ObjectKind k)
{
    switch (k)
    {
        case ObjectKind::Blob: return "blob";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS wire: unknown ObjectKind {}", static_cast<int>(k));
}

ObjectKind objectKindFromWord(std::string_view w, std::string_view what)
{
    if (w == "blob") return ObjectKind::Blob;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: unknown object kind '{}'", what, w);
}

void writeTokenFields(WriteBuffer & out, bool & first, const Token & t)
{
    writeKey(out, "tt", first);
    writeStringValue(out, tokenTypeToWord(t.type));
    writeKey(out, "tv", first);
    writeStringValue(out, t.value);
}

void writeBlobRefFields(WriteBuffer & out, bool & first, const BlobRef & r)
{
    writeKey(out, "ha", first);
    writeStringValue(out, blobHashAlgoName(r.algo));
    writeKey(out, "h", first);
    writeStringValue(out, codecFor(r.algo).toHex(r.digest));
}

}
```

- [ ] **Step 4: Verify PASS** — build; `unit_tests_dbms --gtest_filter='CasWireVocab*'` → both green.

- [ ] **Step 5: Commit** — `git add` the two `Formats/CasWireVocab.*` files + `gtest_cas_wire_vocab.cpp`:

```bash
git commit -m "cas: formats v3 phase 2 — shared wire-value vocabulary (Token/BlobRef/enum words)

Frozen sub-object JSON rendering consumed by cas_gc_outcomes (this phase) and
refsnaplog/runs/blob-envelope (phases 3/5/7), per the codecs-v3 design.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27"
```

---

### Task 2: `cas_pool_meta` cutover (retires the phase-1 toy) {#task2}

**Files:**
- Create: `Core/Formats/CasPoolMetaFormat.{h,cpp}`
- Delete: `Core/CasPoolMeta.h` (contents split); Modify: `Core/CasPoolMeta.cpp` (keep protocol logic, drop proto)
- Modify: `src/Disks/tests/gtest_cas_format_battery.cpp` (replace the toy with the real case); migrate `CasPoolMeta.*` / `CasHeaderGolden.PoolMeta*` tests from `gtest_cas_gc_formats.cpp`
- Include-rewrite: every includer of `Core/CasPoolMeta.h`

**Interfaces:**
- Consumes: phase-1 `CasTextFormat`; `CasIds.h` (`u128ToHex`), `CasBlobHasher.h` (`BlobHashAlgo`, `blobHashAlgoName`).
- Produces: `Formats/CasPoolMetaFormat.h` with `struct PoolMeta` (+ its `createOrValidate` static, defined in `Core/`), `String encodePoolMeta(const PoolMeta &)`, `PoolMeta decodePoolMeta(std::string_view)`. Golden text:

```
{"type":"cas_pool_meta","v":3}
{"pid":"00112233445566778899aabbccddeeff","hln":256,"mrg":3,"alg":"ch128"}
```

- [ ] **Step 1: Failing test** — replace the toy in `gtest_cas_format_battery.cpp` with the real codec case (this is the phase-1 toy's designated replacement):

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

using namespace DB::Cas;

TEST(CasFormatBattery, PoolMeta)
{
    PoolMeta pm;
    pm.pool_id = hexToU128("00112233445566778899aabbccddeeff");
    pm.blob_header_len = 256;
    pm.min_reader_generation = 3;
    pm.algos_used = {static_cast<uint8_t>(BlobHashAlgo::CityHash128)};
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::PoolMeta,
        .encode = [&] { return sealObject(FormatId::PoolMeta, encodePoolMeta(pm)); },
        .decode = [](std::string_view s) { decodePoolMeta(std::string(openObject(FormatId::PoolMeta, s))); },
        .golden = "{\"type\":\"cas_pool_meta\",\"v\":3}\n"
                  "{\"pid\":\"00112233445566778899aabbccddeeff\",\"hln\":256,\"mrg\":3,\"alg\":\"ch128\"}\n"});
}
```

(`encodePoolMeta` already returns the STORED bytes for a `Never` format — `sealObject` is identity there — but wrapping it keeps every battery `encode` uniform: "produce stored bytes". `decode` runs `openObject` first, uniform for the compressed formats too.)

- [ ] **Step 2: Verify compile failure** (`CasPoolMetaFormat.h` missing).

- [ ] **Step 3: Implement** — `Core/Formats/CasPoolMetaFormat.h` (struct moves here; `Backend`/`Layout` forward-declared so the static stays legal without a subsystem include):

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobHasher.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DB::Cas
{

class Backend;
class Layout;

/// `_pool_meta` — pool identity + the pool-wide constants every build/read agrees on (spec §4).
/// v3 text form: header line + one JSON body object
/// {"pid":"<32hex>","hln":<blob_header_len>,"mrg":<min_reader_generation>,"alg":"<algo-words>"}.
/// The POOL is authoritative on reopen: constants come FROM this object; `createOrValidate`'s config
/// args apply only at first creation. `pool_id` doubles as the envelope domain_id — stable for life.
struct PoolMeta
{
    UInt128 pool_id{};
    uint64_t blob_header_len = 0;
    uint64_t min_reader_generation = 0;
    /// Every hash algo ever admitted, as static_cast<uint8_t>(BlobHashAlgo), sorted + append-only.
    std::vector<uint8_t> algos_used;

    static PoolMeta createOrValidate(
        Backend &, const Layout &, uint64_t blob_header_len,
        BlobHashAlgo blob_hash_algo = BlobHashAlgo::CityHash128, bool allow_new = false);
};

String encodePoolMeta(const PoolMeta &);
PoolMeta decodePoolMeta(std::string_view);

}
```

`Core/Formats/CasPoolMetaFormat.cpp` (the codec + the two pure validators moved from `CasPoolMeta.cpp`):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasPoolMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int UNKNOWN_FORMAT_VERSION;
}
}

namespace DB::Cas
{

namespace
{

/// `blob_header_len` must be 8-aligned and within [96, 16 KiB]. Error code differs by context:
/// BAD_ARGUMENTS at creation (caller config), CORRUPTED_DATA on decode (persisted corruption).
void validateBlobHeaderLen(uint64_t blob_header_len, int error_code, std::string_view what)
{
    if (blob_header_len < 96)
        throw Exception(error_code, "CAS {}: blob_header_len must be >= 96, got {}", what, blob_header_len);
    if (blob_header_len % 8 != 0)
        throw Exception(error_code, "CAS {}: blob_header_len must be a multiple of 8, got {}", what, blob_header_len);
    if (blob_header_len > 16 * 1024)
        throw Exception(error_code, "CAS {}: blob_header_len must be <= 16384, got {}", what, blob_header_len);
}

/// `algos_used`: non-empty, strictly increasing (=> no dups), every entry a real BlobHashAlgo.
void validateAlgosUsed(const std::vector<uint8_t> & algos_used, int error_code, std::string_view what)
{
    if (algos_used.empty())
        throw Exception(error_code, "CAS {}: algos_used must be non-empty", what);
    for (size_t i = 0; i < algos_used.size(); ++i)
    {
        try
        {
            blobHashAlgoName(static_cast<BlobHashAlgo>(algos_used[i]));
        }
        catch (const Exception &)
        {
            throw Exception(error_code, "CAS {}: algos_used contains an unknown algo {}", what, algos_used[i]);
        }
        if (i > 0 && algos_used[i] <= algos_used[i - 1])
            throw Exception(error_code,
                "CAS {}: algos_used must be strictly sorted with no duplicates, got {} at index {} not after {}",
                what, algos_used[i], i, algos_used[i - 1]);
    }
}

}

String encodePoolMeta(const PoolMeta & pm)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::PoolMeta);

    bool first = true;
    writeKey(out, "pid", first);
    writeHex128Value(out, pm.pool_id);
    writeKey(out, "hln", first);
    writeIntText(pm.blob_header_len, out);
    writeKey(out, "mrg", first);
    writeIntText(pm.min_reader_generation, out);
    writeKey(out, "alg", first);
    {
        /// Comma-joined algo words (tiny list, <=3): "ch128" or "ch128,sha256".
        String joined;
        for (size_t i = 0; i < pm.algos_used.size(); ++i)
        {
            if (i != 0)
                joined += ',';
            joined += blobHashAlgoName(static_cast<BlobHashAlgo>(pm.algos_used[i]));
        }
        writeStringValue(out, joined);
    }
    closeObject(out, first);
    writeChar('\n', out);

    out.finalize();
    return out.str();
}

PoolMeta decodePoolMeta(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    const TextHeader header = expectHeaderLine(in, FormatId::PoolMeta);

    /// Backward floor (Task 12): a pool written with the removed pre-generation-3 mutable ref-shard
    /// format holds refs this build cannot read — fail closed. (v is always >= 3 on write; the gate
    /// exists for a hypothetical older object.) The forward floor is expectHeaderLine's checkCompatibility.
    if (header.v < kRefSnapshotLogGeneration)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool meta: pool was written with the removed pre-generation-3 ref-shard format "
            "(generation {}); this build reads only the snapshot+log ref format (generation {}+). "
            "CAS is pre-release — recreate the pool.", header.v, kRefSnapshotLogGeneration);

    const String body = readLine(in, traitsFor(FormatId::PoolMeta).line_cap, "pool meta");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "pool meta");

    PoolMeta pm;
    bool saw_pid = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "pid") { pm.pool_id = r.readHex128(); saw_pid = true; }
        else if (key == "hln") pm.blob_header_len = r.readU64Number();
        else if (key == "mrg") pm.min_reader_generation = r.readU64Number();
        else if (key == "alg")
        {
            const String joined = r.readString();
            size_t start = 0;
            while (start <= joined.size())
            {
                const size_t comma = joined.find(',', start);
                const String word = joined.substr(start, comma == String::npos ? String::npos : comma - start);
                if (word.empty())
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: empty algo word in '{}'", joined);
                pm.algos_used.push_back(static_cast<uint8_t>(blobHashAlgoFromWord(word, "pool meta algo")));
                if (comma == String::npos)
                    break;
                start = comma + 1;
            }
        }
        else r.skipUnknown(key);
    }
    if (!saw_pid)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: missing pid");
    if (!body_in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: junk after body object");
    if (!in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS pool meta: trailing bytes after body line");

    validateBlobHeaderLen(pm.blob_header_len, ErrorCodes::CORRUPTED_DATA, "pool meta");
    validateAlgosUsed(pm.algos_used, ErrorCodes::CORRUPTED_DATA, "pool meta");

    if (G_BUILD < pm.min_reader_generation)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS pool meta: pool requires reader generation {} but this build supports at most {}",
            pm.min_reader_generation, G_BUILD);

    return pm;
}

}
```

Then **split `Core/CasPoolMeta.cpp`**: delete `Core/CasPoolMeta.h`; make `Core/CasPoolMeta.cpp` `#include "Formats/CasPoolMetaFormat.h"` + `CasBackend.h` + `CasLayout.h`, DROP `#include <cas_format.pb.h>`, and keep ONLY `mintPoolId`, `isAlgoAdmittedIn`, `joinAlgoNames`, `throwNotAdmitted`, `admitOrValidate`, and `PoolMeta::createOrValidate` (all unchanged — they call `encodePoolMeta`/`decodePoolMeta` by their stable signatures). Rewrite includers:

```bash
grep -rl 'ContentAddressed/Core/CasPoolMeta\.h' src/ | xargs sed -i \
  's|ContentAddressed/Core/CasPoolMeta\.h|ContentAddressed/Core/Formats/CasPoolMetaFormat.h|g'
```

Migrate the pool-meta unit tests out of `gtest_cas_gc_formats.cpp` (`CasPoolMeta.ConstantInvariantsPostParse`, `CasPoolMeta.MinReaderGenerationGate`, `CasHeaderGolden.PoolMeta*`) into a new `gtest_cas_pool_meta_format.cpp` re-pointed at the text codec (round-trip + the two floors + a corrupt-body case). Drop the now-obsolete `CasHeaderGolden.PoolMetaCasHeaderRoundTrips` proto-header assertion (there is no `CasHeader` anymore); replace with a golden-text assertion.

- [ ] **Step 4: Verify PASS** — build; `unit_tests_dbms --gtest_filter='CasFormatBattery.PoolMeta:CasPoolMeta*'` green; full `Cas*` slice green.

- [ ] **Step 5: Commit** — `git add` `Formats/CasPoolMetaFormat.*`, `Core/CasPoolMeta.cpp`, deleted `Core/CasPoolMeta.h`, the rewired includers, `gtest_cas_format_battery.cpp`, `gtest_cas_pool_meta_format.cpp`, edited `gtest_cas_gc_formats.cpp`. Message `cas: formats v3 phase 2 — cas_pool_meta text cutover` + trailer.

---

### Task 3: `cas_owner` / `cas_epoch` / `cas_mount_lease` cutover {#task3}

**Files:**
- Create: `Core/Formats/CasServerRootFormats.{h,cpp}`
- Modify: `Core/CasServerRoot.h` (drop the three structs + codec decls, include the new header), `Core/CasServerRoot.cpp` (drop `<cas_format.pb.h>` + the six codec bodies; keep all protocol logic incl. `MountLeaseKeeper::encodeBody`, which calls `encodeMountLease` unchanged)
- Test: `src/Disks/tests/gtest_cas_server_root_format.cpp` (new: three battery rows + field round-trips)

**Interfaces:**
- Produces: `Formats/CasServerRootFormats.h` — `struct OwnerObject`, `struct ServerEpoch`, `struct MountLease` + `encodeOwner`/`decodeOwner`, `encodeServerEpoch`/`decodeServerEpoch`, `encodeMountLease`/`decodeMountLease` (all signatures identical to today). Golden texts:

```
{"type":"cas_owner","v":3}
{"su":"0123456789abcdeffedcba9876543210"}
```
```
{"type":"cas_epoch","v":3}
{"nwe":"7"}
```
```
{"type":"cas_mount_lease","v":3}
{"su":"0123456789abcdeffedcba9876543210","we":"7","hn":"host-1","pid":4242,"sat":1752537600000,"seq":"5","eat":1752537630000,"ma":"9","fen":false}
```

- [ ] **Step 1: Failing test** — `src/Disks/tests/gtest_cas_server_root_format.cpp` registers the three battery cases + a mount-lease field round-trip that covers `min_active == UINT64_MAX` (the farewell sentinel must survive as a decimal string) and `gc_fenced == true`:

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>
#include <limits>

using namespace DB::Cas;

TEST(CasFormatBattery, Owner)
{
    OwnerObject o; o.server_uuid = hexToU128("0123456789abcdeffedcba9876543210");
    runFormatBattery({FormatId::Owner,
        [&]{ return sealObject(FormatId::Owner, encodeOwner(o)); },
        [](std::string_view s){ decodeOwner(std::string(openObject(FormatId::Owner, s))); },
        "{\"type\":\"cas_owner\",\"v\":3}\n{\"su\":\"0123456789abcdeffedcba9876543210\"}\n"});
}

TEST(CasFormatBattery, ServerEpoch)
{
    ServerEpoch e; e.next_writer_epoch = 7;
    runFormatBattery({FormatId::ServerEpoch,
        [&]{ return sealObject(FormatId::ServerEpoch, encodeServerEpoch(e)); },
        [](std::string_view s){ decodeServerEpoch(std::string(openObject(FormatId::ServerEpoch, s))); },
        "{\"type\":\"cas_epoch\",\"v\":3}\n{\"nwe\":\"7\"}\n"});
}

TEST(CasFormatBattery, MountLease)
{
    MountLease m{hexToU128("0123456789abcdeffedcba9876543210"), 7, "host-1", 4242,
                 1752537600000ULL, 5, 1752537630000ULL, 9, false};
    runFormatBattery({FormatId::MountLease,
        [&]{ return sealObject(FormatId::MountLease, encodeMountLease(m)); },
        [](std::string_view s){ decodeMountLease(std::string(openObject(FormatId::MountLease, s))); },
        "{\"type\":\"cas_mount_lease\",\"v\":3}\n"
        "{\"su\":\"0123456789abcdeffedcba9876543210\",\"we\":\"7\",\"hn\":\"host-1\",\"pid\":4242,"
        "\"sat\":1752537600000,\"seq\":\"5\",\"eat\":1752537630000,\"ma\":\"9\",\"fen\":false}\n"});
}

TEST(CasMountLeaseFormat, FarewellSentinelAndFencedSurvive)
{
    MountLease m{hexToU128("0123456789abcdeffedcba9876543210"), 7, "h", 1,
                 1, 5, 2, std::numeric_limits<uint64_t>::max(), true};
    const MountLease back = decodeMountLease(encodeMountLease(m));
    EXPECT_EQ(back.min_active, std::numeric_limits<uint64_t>::max());
    EXPECT_TRUE(back.gc_fenced);
    EXPECT_EQ(back.hostname, "h");
}
```

- [ ] **Step 2: Verify compile failure.**

- [ ] **Step 3: Implement** — `Core/Formats/CasServerRootFormats.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// Per-server-root control singletons under gc/server-roots/<srid>/ (mount safety, Phase 0). v3 text:
/// header line + one JSON body object. Owner binds srid -> server UUID (write-once); epoch is the
/// monotone next writer epoch; mount lease is the current live holder (liveness + merged min_active).

struct OwnerObject
{
    UInt128 server_uuid{};
};

struct ServerEpoch
{
    uint64_t next_writer_epoch = 0;
};

struct MountLease
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    String hostname;
    uint64_t pid = 0;
    uint64_t started_at_ms = 0;
    uint64_t seq = 0;
    uint64_t expires_at_ms = 0;
    uint64_t min_active = 0;   /// UINT64_MAX = retired (farewell)
    bool gc_fenced = false;    /// GC fence-out of an expired lease; terminal
};

String encodeOwner(const OwnerObject & o);
OwnerObject decodeOwner(std::string_view data);

String encodeServerEpoch(const ServerEpoch & e);
ServerEpoch decodeServerEpoch(std::string_view data);

String encodeMountLease(const MountLease & m);
MountLease decodeMountLease(std::string_view data);

}
```

`Core/Formats/CasServerRootFormats.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasServerRootFormats.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

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

/// Read the single body line of a control singleton and hand back a reader over it (fail-closed on a
/// missing/oversized line, and on trailing bytes after the body line).
String readBodyLine(ReadBuffer & in, FormatId id, std::string_view what)
{
    return readLine(in, traitsFor(id).line_cap, what);
}

}

String encodeOwner(const OwnerObject & o)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::Owner);
    bool first = true;
    writeKey(out, "su", first);
    writeHex128Value(out, o.server_uuid);
    closeObject(out, first);
    writeChar('\n', out);
    out.finalize();
    return out.str();
}

OwnerObject decodeOwner(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::Owner);
    const String body = readBodyLine(in, FormatId::Owner, "owner");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "owner");

    OwnerObject o;
    bool saw = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "su") { o.server_uuid = r.readHex128(); saw = true; }
        else r.skipUnknown(key);
    }
    if (!saw)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: missing su");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS owner: trailing bytes");
    return o;
}

String encodeServerEpoch(const ServerEpoch & e)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::ServerEpoch);
    bool first = true;
    writeKey(out, "nwe", first);
    writeU64StringValue(out, e.next_writer_epoch);
    closeObject(out, first);
    writeChar('\n', out);
    out.finalize();
    return out.str();
}

ServerEpoch decodeServerEpoch(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::ServerEpoch);
    const String body = readBodyLine(in, FormatId::ServerEpoch, "server-epoch");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "server-epoch");

    ServerEpoch e;
    bool saw = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "nwe") { e.next_writer_epoch = r.readU64String(); saw = true; }
        else r.skipUnknown(key);
    }
    if (!saw)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: missing nwe");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS server-epoch: trailing bytes");
    return e;
}

String encodeMountLease(const MountLease & m)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::MountLease);
    bool first = true;
    writeKey(out, "su", first);  writeHex128Value(out, m.server_uuid);
    writeKey(out, "we", first);  writeU64StringValue(out, m.writer_epoch);
    writeKey(out, "hn", first);  writeStringValue(out, m.hostname);
    writeKey(out, "pid", first); writeIntText(m.pid, out);
    writeKey(out, "sat", first); writeIntText(m.started_at_ms, out);
    writeKey(out, "seq", first); writeU64StringValue(out, m.seq);
    writeKey(out, "eat", first); writeIntText(m.expires_at_ms, out);
    writeKey(out, "ma", first);  writeU64StringValue(out, m.min_active);
    writeKey(out, "fen", first); writeBoolValue(out, m.gc_fenced); /// new phase-1 helper — see note below
    closeObject(out, first);
    writeChar('\n', out);
    out.finalize();
    return out.str();
}

MountLease decodeMountLease(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::MountLease);
    const String body = readBodyLine(in, FormatId::MountLease, "mount-lease");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "mount-lease");

    MountLease m;
    String key;
    while (r.nextKey(key))
    {
        if (key == "su") m.server_uuid = r.readHex128();
        else if (key == "we") m.writer_epoch = r.readU64String();
        else if (key == "hn") m.hostname = r.readString();
        else if (key == "pid") m.pid = r.readU64Number();
        else if (key == "sat") m.started_at_ms = r.readU64Number();
        else if (key == "seq") m.seq = r.readU64String();
        else if (key == "eat") m.expires_at_ms = r.readU64Number();
        else if (key == "ma") m.min_active = r.readU64String();
        else if (key == "fen") m.gc_fenced = r.readBool();
        else r.skipUnknown(key);
    }
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS mount-lease: trailing bytes");
    return m;
}

}
```

**Note (implementer): `writeBoolValue` is new.** The phase-1 vocabulary has no `writeBoolValue`; add one small helper to `CasTextFormat` (declared in `CasTextFormat.h`, defined in `.cpp`: `void writeBoolValue(WriteBuffer & out, bool v) { writeCString(v ? "true" : "false", out); }`) — the `fen` line above already uses it, and it pairs with the existing `JsonObjectReader::readBool`. Adding `writeBoolValue` is the one intentional phase-1-helper extension in this task (symmetric with `readBool`); note it in the commit. Mount lease is `Never`-compression and NOT byte-adopted (CAS-swapped via `putOverwrite` with a token), so field order is a readability choice, not a determinism constraint.

Then edit `Core/CasServerRoot.h` to `#include "Formats/CasServerRootFormats.h"` and delete the three struct definitions + six codec decls; edit `Core/CasServerRoot.cpp` to drop `#include <cas_format.pb.h>` and the six codec bodies (keep everything else — `MountLeaseKeeper::encodeBody` still calls `encodeMountLease`). No include-rewrite needed elsewhere (`CasServerRoot.h` re-exports the structs via the include).

- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasFormatBattery.Owner:CasFormatBattery.ServerEpoch:CasFormatBattery.MountLease:CasMountLeaseFormat*:CasMount*'` green (the large `gtest_cas_mount.cpp` behavioral suite must still pass — it exercises `claimMount` end-to-end through the new codec).

- [ ] **Step 5: Commit** — `cas: formats v3 phase 2 — cas_owner/cas_epoch/cas_mount_lease text cutover` + trailer.


### Task 4: `cas_gc_state` + `cas_gc_hb` cutover (kills the last unversioned object) {#task4}

**Files:**
- Create: `Core/Formats/CasGcStateFormat.{h,cpp}` (holds BOTH `cas_gc_state` and `cas_gc_hb`, per spec placement)
- Delete: `Core/CasGcFormats.{h,cpp}`; Modify: `Core/CasBlobInDegree.h` (adopt the in-memory-only `RetiredEntry`)
- Include-rewrite: every includer of `Core/CasGcFormats.h`
- Test: migrate `gtest_cas_gc_formats.cpp`'s gc-state / heartbeat cases into `gtest_cas_gc_state_format.cpp` + two battery rows

**Migration stance (spec-explicit):** the spec (§migration-order step 2, §control-plane) says `cas_gc_hb` "kills the unversioned exception" and shows the example `{"type":"cas_gc_hb","v":1,"by":"<32hex>","seq":"1741"}`. So the heartbeat becomes a **full header'd text object** — header line + one JSON body line — NOT a kept raw 24-byte fast path. The advisory-pulse semantics (owner + monotone `hb_seq`) are unchanged; only the encoding gains the header/version gate every other object has. Hard cutover, no dual-read (pre-release).

**Interfaces:**
- Produces: `Formats/CasGcStateFormat.h` with `struct GcLease`, `struct GcState`, `struct GcHeartbeat` + `encodeGcState`/`decodeGcState`, `encodeGcHeartbeat`/`decodeGcHeartbeat` (signatures unchanged). Golden texts:

```
{"type":"cas_gc_state","v":3}
{"rnd":"4","gcs":1,"sg":"9","spt":"7","sa":"3","msc":"","lo":"00000000000000000000000000000001","ls":"12"}
```
```
{"type":"cas_gc_hb","v":3}
{"by":"00000000000000000000000000000001","seq":"1741"}
```

(`gcs` = `gc_shards`, mapped to/from the proto's `snap_shards` field is irrelevant now — the wire is text and the C++ field is `gc_shards`. `rnd`/`sg`/`spt`/`sa`/`ls`/`seq` are unbounded counters → strings; `gcs` is bounded ≥1 → number; `msc` = `manifest_sweep_cursor` string; `lo`/`by` = owner hex.)

- [ ] **Step 1: Failing test** — `gtest_cas_gc_state_format.cpp` with two battery rows + a `gc_shards == 0 → CORRUPTED_DATA` decode case + a heartbeat round-trip:

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasGcStateFormat.h>

using namespace DB::Cas;
namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

TEST(CasFormatBattery, GcState)
{
    GcState s;
    s.round = 4; s.gc_shards = 1; s.snap_generation = 9; s.snap_pruned_through = 7;
    s.snap_attempt = 3; s.manifest_sweep_cursor = ""; s.lease = GcLease{UInt128(1), 12};
    runFormatBattery({FormatId::GcState,
        [&]{ return sealObject(FormatId::GcState, encodeGcState(s)); },
        [](std::string_view d){ decodeGcState(std::string(openObject(FormatId::GcState, d))); },
        "{\"type\":\"cas_gc_state\",\"v\":3}\n"
        "{\"rnd\":\"4\",\"gcs\":1,\"sg\":\"9\",\"spt\":\"7\",\"sa\":\"3\",\"msc\":\"\","
        "\"lo\":\"00000000000000000000000000000001\",\"ls\":\"12\"}\n"});
}

TEST(CasFormatBattery, GcHeartbeat)
{
    GcHeartbeat hb{UInt128(1), 1741};
    runFormatBattery({FormatId::GcHeartbeat,
        [&]{ return sealObject(FormatId::GcHeartbeat, encodeGcHeartbeat(hb)); },
        [](std::string_view d){ decodeGcHeartbeat(std::string(openObject(FormatId::GcHeartbeat, d))); },
        "{\"type\":\"cas_gc_hb\",\"v\":3}\n"
        "{\"by\":\"00000000000000000000000000000001\",\"seq\":\"1741\"}\n"});
}

TEST(CasGcStateFormat, RejectsZeroGcShards)
{
    const String bad = "{\"type\":\"cas_gc_state\",\"v\":3}\n"
                       "{\"rnd\":\"0\",\"gcs\":0,\"sg\":\"0\",\"spt\":\"0\",\"sa\":\"0\",\"msc\":\"\","
                       "\"lo\":\"00000000000000000000000000000000\",\"ls\":\"0\"}\n";
    EXPECT_THROW(decodeGcState(bad), DB::Exception);
}
```

- [ ] **Step 2: Verify compile failure.**

- [ ] **Step 3: Implement** — `Core/Formats/CasGcStateFormat.h` (structs move here; `RetiredEntry` does NOT — see below):

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <base/extended_types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// gc/state control object (spec §GC State): the GC lease, snap config, and cursors. The fold cursor
/// lives in the write-once fold seal, not here. v3 text: header line + one JSON body object.
struct GcLease
{
    UInt128 owner{};
    uint64_t seq = 0;
};

struct GcState
{
    uint64_t round = 0;
    uint64_t gc_shards = 1;         /// immutable; must be >= 1
    uint64_t snap_generation = 0;
    uint64_t snap_pruned_through = 0;
    uint64_t snap_attempt = 0;
    String manifest_sweep_cursor;
    GcLease lease;
};

String encodeGcState(const GcState & state);
GcState decodeGcState(std::string_view data);

/// Advisory GC liveness pulse (B160). v3 text: header line + {"by":"<owner hex>","seq":"<hb_seq>"} —
/// the former 24-byte unversioned binary is gone (the last unversioned object; spec §control-plane).
struct GcHeartbeat
{
    UInt128 owner{};
    uint64_t hb_seq = 0;
};

String encodeGcHeartbeat(const GcHeartbeat & hb);
GcHeartbeat decodeGcHeartbeat(std::string_view data);

}
```

`Core/Formats/CasGcStateFormat.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasGcStateFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <base/defines.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

String encodeGcState(const GcState & state)
{
    chassert(state.gc_shards >= 1);   /// catch a zeroed GC constant at the write site
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::GcState);
    bool first = true;
    writeKey(out, "rnd", first); writeU64StringValue(out, state.round);
    writeKey(out, "gcs", first); writeIntText(state.gc_shards, out);
    writeKey(out, "sg", first);  writeU64StringValue(out, state.snap_generation);
    writeKey(out, "spt", first); writeU64StringValue(out, state.snap_pruned_through);
    writeKey(out, "sa", first);  writeU64StringValue(out, state.snap_attempt);
    writeKey(out, "msc", first); writeStringValue(out, state.manifest_sweep_cursor);
    writeKey(out, "lo", first);  writeHex128Value(out, state.lease.owner);
    writeKey(out, "ls", first);  writeU64StringValue(out, state.lease.seq);
    closeObject(out, first);
    writeChar('\n', out);
    out.finalize();
    return out.str();
}

GcState decodeGcState(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::GcState);
    const String body = readLine(in, traitsFor(FormatId::GcState).line_cap, "gc/state");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "gc/state");

    GcState state;
    String key;
    while (r.nextKey(key))
    {
        if (key == "rnd") state.round = r.readU64String();
        else if (key == "gcs") state.gc_shards = r.readU64Number();
        else if (key == "sg") state.snap_generation = r.readU64String();
        else if (key == "spt") state.snap_pruned_through = r.readU64String();
        else if (key == "sa") state.snap_attempt = r.readU64String();
        else if (key == "msc") state.manifest_sweep_cursor = r.readString();
        else if (key == "lo") state.lease.owner = r.readHex128();
        else if (key == "ls") state.lease.seq = r.readU64String();
        else r.skipUnknown(key);
    }
    if (state.gc_shards == 0)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: gc_shards must be >= 1");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc/state: trailing bytes");
    return state;
}

String encodeGcHeartbeat(const GcHeartbeat & hb)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::GcHeartbeat);
    bool first = true;
    writeKey(out, "by", first);  writeHex128Value(out, hb.owner);
    writeKey(out, "seq", first); writeU64StringValue(out, hb.hb_seq);
    closeObject(out, first);
    writeChar('\n', out);
    out.finalize();
    return out.str();
}

GcHeartbeat decodeGcHeartbeat(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::GcHeartbeat);
    const String body = readLine(in, traitsFor(FormatId::GcHeartbeat).line_cap, "gc heartbeat");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "gc heartbeat");

    GcHeartbeat hb;
    String key;
    while (r.nextKey(key))
    {
        if (key == "by") hb.owner = r.readHex128();
        else if (key == "seq") hb.hb_seq = r.readU64String();
        else r.skipUnknown(key);
    }
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS gc heartbeat: trailing bytes");
    return hb;
}

}
```

**`RetiredEntry` relocation:** `Core/CasGcFormats.h` also declared `struct RetiredEntry`, which is **in-memory only** (its comment: "the durable RetiredSet object family is gone; this struct now lives solely as the element type of `RetiredMergeResult` in `CasBlobInDegree.h`"). It has no codec. Move the `RetiredEntry` struct verbatim into `Core/CasBlobInDegree.h` (next to `RetiredMergeResult`), then delete `Core/CasGcFormats.{h,cpp}`. Rewire includers:

```bash
grep -rl 'ContentAddressed/Core/CasGcFormats\.h' src/ | xargs sed -i \
  's|ContentAddressed/Core/CasGcFormats\.h|ContentAddressed/Core/Formats/CasGcStateFormat.h|g'
# Then, for any file that used RetiredEntry, ensure it includes CasBlobInDegree.h (grep 'RetiredEntry'
# in the rewired set; CasBlobInDegree.h is already included by the GC merge sites).
grep -rn 'RetiredEntry' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -v CasBlobInDegree.h
```

Migrate `gtest_cas_gc_formats.cpp`'s gc-state + heartbeat tests (`GcStateV3RoundTrip`, `GcStateSnapPrunedThroughRoundTrip`, `SnapAttemptRoundTrips`, `ManifestSweepCursorRoundTrips`, `GcHeartbeatRoundTrip`, `GcStateV3Validation`, `GcStateRejectsOldVersionFailClosed`, `GcStateValidation`, `CasHeaderGolden.GcStateCasHeaderRoundTrips`) into `gtest_cas_gc_state_format.cpp`, re-pointed at the text codec; drop the `CasHeaderGolden` proto-header assertions (replace with golden-text). `GcStateV3DefaultsAndEncodingIsBinary` is deleted (the object is no longer binary). Leave the outcomes + fold-seal tests in place for Tasks 5–6.

- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasFormatBattery.GcState:CasFormatBattery.GcHeartbeat:CasGcState*:CasHeartbeat*'` green; the `gtest_cas_heartbeat.cpp` behavioral suite (GC pulse logic) still green.

- [ ] **Step 5: Commit** — `cas: formats v3 phase 2 — cas_gc_state + cas_gc_hb text cutover (last unversioned object dies)` + trailer.

---

### Task 5: `cas_gc_outcomes` cutover (record-line body, `Always`/`.zst`) {#task5}

**Files:**
- Create: `Core/Formats/CasGcOutcomesFormat.{h,cpp}`
- Delete: `Core/CasGcOutcomes.{h,cpp}`; include-rewrite its includers
- Modify: `Core/CasLayout.{h,cpp}` — `outcomesKey` appends `storedSuffix(FormatId::GcOutcomes)` (`.zst`), and any outcomes-key **parser** strips it
- Test: `gtest_cas_gc_outcomes_format.cpp` + a battery row; migrate `OutcomeLogRoundTrip` / `EmptyOutcomeLogRoundTrips` / `OutcomeLogValidation` / `CasHeaderGolden.GcOutcomes*`

**Body shape:** `cas_gc_outcomes` is `Control` in role but carries a repeated entry list, and its traits give it a 64 KiB **line** cap + 256 MiB object cap — so it is **line-structured**, materialized whole: header line + one flat JSON record per `OutcomeEntry` (insertion order, matching today's encoder) + a `{"n":<count>}` trailer. `Always` compression → the stored bytes are one zstd frame (`.zst` key). NOT deterministic (idempotent `putIfAbsent`; on a byte mismatch the replay path decodes and ADOPTS the existing durable object rather than failing — byte equality is never a correctness gate, which zstd would defeat anyway), so insertion order is fine.

**Interfaces:**
- Consumes: Task 1 `CasWireVocab` (`writeBlobRefFields`/`blobHashAlgoFromWord`, `writeTokenFields`/`tokenTypeFromWord`, `objectKindToWord`/`objectKindFromWord`) + `codecFor` for the digest hex.
- Produces: `Formats/CasGcOutcomesFormat.h` with `enum class OutcomeKind`, `struct OutcomeEntry`, `struct OutcomeLog` + `encodeOutcomeLog`/`decodeOutcomeLog`. One record line per entry:

```
{"type":"cas_gc_outcomes","v":3}
{"k":"blob","ha":"ch128","h":"00112233445566778899aabbccddeeff","tt":"etag","tv":"e-1","oc":"deleted"}
{"n":1}
```

- [ ] **Step 1: Failing test** — `gtest_cas_gc_outcomes_format.cpp`:

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

using namespace DB::Cas;

TEST(CasFormatBattery, GcOutcomes)
{
    OutcomeLog log;
    OutcomeEntry e;
    e.kind = ObjectKind::Blob;
    e.ref = BlobRef{BlobHashAlgo::CityHash128, BlobDigest::fromU128(hexToU128("00112233445566778899aabbccddeeff"))};
    e.token = Token{"e-1", TokenType::ETag};
    e.outcome = OutcomeKind::Deleted;
    log.entries.push_back(e);
    runFormatBattery({FormatId::GcOutcomes,
        [&]{ return sealObject(FormatId::GcOutcomes, encodeOutcomeLog(log)); },
        [](std::string_view d){ decodeOutcomeLog(std::string(openObject(FormatId::GcOutcomes, d))); },
        "{\"type\":\"cas_gc_outcomes\",\"v\":3}\n"
        "{\"k\":\"blob\",\"ha\":\"ch128\",\"h\":\"00112233445566778899aabbccddeeff\","
        "\"tt\":\"etag\",\"tv\":\"e-1\",\"oc\":\"deleted\"}\n{\"n\":1}\n"});
}

TEST(CasGcOutcomesFormat, EmptyRoundTrips)
{
    EXPECT_EQ(decodeOutcomeLog(encodeOutcomeLog(OutcomeLog{})).entries.size(), 0u);
}
```

- [ ] **Step 2: Verify compile failure.**

- [ ] **Step 3: Implement** — `Core/Formats/CasGcOutcomesFormat.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h>
#include <base/types.h>
#include <cstdint>
#include <string_view>
#include <vector>

namespace DB::Cas
{

/// Retire outcomes (spec §7 R4): what the recheck decided per retired entry. One object per
/// gc/gen/<g>/attempt/<a>/outcomes/<round>/<shard>, written once. v3 text: header line + one flat
/// JSON record per entry (insertion order) + {"n":count} trailer; Always-compressed (.zst key).
enum class OutcomeKind : uint8_t
{
    Deleted = 1,
    Absent = 2,
    Replaced = 3,
    Spared = 4,
};

struct OutcomeEntry
{
    ObjectKind kind = ObjectKind::Blob;
    BlobRef ref{};
    Token token;
    OutcomeKind outcome = OutcomeKind::Spared;
};

struct OutcomeLog
{
    std::vector<OutcomeEntry> entries;
};

String encodeOutcomeLog(const OutcomeLog & log);
OutcomeLog decodeOutcomeLog(std::string_view data);

}
```

`Core/Formats/CasGcOutcomesFormat.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasGcOutcomesFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>

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

std::string_view outcomeKindToWord(OutcomeKind o)
{
    switch (o)
    {
        case OutcomeKind::Deleted:  return "deleted";
        case OutcomeKind::Absent:   return "absent";
        case OutcomeKind::Replaced: return "replaced";
        case OutcomeKind::Spared:   return "spared";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: unknown OutcomeKind {}", static_cast<int>(o));
}

OutcomeKind outcomeKindFromWord(std::string_view w)
{
    if (w == "deleted")  return OutcomeKind::Deleted;
    if (w == "absent")   return OutcomeKind::Absent;
    if (w == "replaced") return OutcomeKind::Replaced;
    if (w == "spared")   return OutcomeKind::Spared;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: unknown outcome '{}'", w);
}

}

String encodeOutcomeLog(const OutcomeLog & log)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::GcOutcomes);
    for (const OutcomeEntry & e : log.entries)
    {
        bool first = true;
        writeKey(out, "k", first);
        writeStringValue(out, objectKindToWord(e.kind));
        writeBlobRefFields(out, first, e.ref);   /// ha + h
        writeTokenFields(out, first, e.token);   /// tt + tv
        writeKey(out, "oc", first);
        writeStringValue(out, outcomeKindToWord(e.outcome));
        closeObject(out, first);
        writeChar('\n', out);
    }
    writeTrailerLine(out, log.entries.size());
    out.finalize();
    return out.str();
}

OutcomeLog decodeOutcomeLog(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::GcOutcomes);
    const uint64_t line_cap = traitsFor(FormatId::GcOutcomes).line_cap;

    OutcomeLog log;
    while (true)
    {
        const String line = readLine(in, line_cap, "outcome log");
        ReadBufferFromMemory line_in(line.data(), line.size());
        JsonObjectReader r(line_in, KeyStrictness::Tolerant, "outcome log");

        String key;
        /// Peek the first key to distinguish a trailer ("n") from a record ("k").
        if (!r.nextKey(key))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: empty line");
        if (key == "n")
        {
            const uint64_t n = r.readU64Number();
            while (r.nextKey(key))
                r.skipUnknown(key);
            if (!line_in.eof() || !in.eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: bytes after trailer");
            if (n != log.entries.size())
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS outcome log: trailer count {} != {} records", n, log.entries.size());
            return log;
        }

        OutcomeEntry e;
        String ha, hhex, tv;
        bool have_ha = false, have_h = false, have_tt = false;
        TokenType tt{};
        do
        {
            if (key == "k") e.kind = objectKindFromWord(r.readString(), "outcome log");
            else if (key == "ha") { ha = r.readString(); have_ha = true; }
            else if (key == "h") { hhex = r.readString(); have_h = true; }
            else if (key == "tt") { tt = tokenTypeFromWord(r.readString(), "outcome log"); have_tt = true; }
            else if (key == "tv") tv = r.readString();
            else if (key == "oc") e.outcome = outcomeKindFromWord(r.readString());
            else r.skipUnknown(key);
        } while (r.nextKey(key));

        if (!have_ha || !have_h || !have_tt)
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: record missing ha/h/tt");
        const BlobHashAlgo algo = blobHashAlgoFromWord(ha, "outcome log");
        e.ref = BlobRef{algo, codecFor(algo).fromHex(hhex)};
        e.token = Token{tv, tt};
        if (!line_in.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS outcome log: junk after record");
        log.entries.push_back(std::move(e));
    }
}

}
```

Then delete `Core/CasGcOutcomes.{h,cpp}`, rewire includers (`grep -rl 'ContentAddressed/Core/CasGcOutcomes\.h' … | sed …` → `Formats/CasGcOutcomesFormat.h`), and update `CasLayout`:

In `Core/CasLayout.cpp`, `outcomesKey(uint64_t generation, uint64_t attempt, uint64_t round, uint64_t shard)` currently ends with a `return <path expression>;`. Do NOT rewrite the path — append the Always-policy suffix to that existing expression so a constructed key deterministically names the `.zst` object: change the `return <expr>;` to `return <expr> + String(storedSuffix(FormatId::GcOutcomes));` (`storedSuffix(FormatId::GcOutcomes)` is `".zst"`; include `Formats/CasFormat.h` in `CasLayout.cpp` if not already). This is the ONLY phase-2 object whose key changes (`GcOutcomes` is the sole `Always` control object; the other seven are `Never`/`PinnedRaw`, no suffix).

Grep for any code that LISTs `gc/.../outcomes/` and parses the key back — strip the `.zst` suffix there (`grep -rn 'outcomesKey\|/outcomes/' src/…/ContentAddressed/`). Most sites construct+GET/PUT and inherit the suffix for free. Migrate `OutcomeLogRoundTrip`, `EmptyOutcomeLogRoundTrips`, `OutcomeLogValidation`, `CasHeaderGolden.GcOutcomesCasHeaderRoundTrips` into `gtest_cas_gc_outcomes_format.cpp` (drop the CasHeader proto assertion).

- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasFormatBattery.GcOutcomes:CasGcOutcomes*'` green; the GC recheck suites (`gtest_cas_gc_round*`, `gtest_cas_gc_ack_floor`) still green (they write+read outcomes end-to-end).

- [ ] **Step 5: Commit** — `cas: formats v3 phase 2 — cas_gc_outcomes text cutover (.zst key suffix)` + trailer.

---

### Task 6: `cas_fold_seal` cutover (DETERMINISTIC, `PinnedRaw`) {#task6}

**Files:**
- Create: `Core/Formats/CasFoldSealFormat.{h,cpp}`
- Delete: `Core/CasGenerationSeal.{h,cpp}`; include-rewrite its includers
- Test: migrate `gtest_cas_generation_seal.cpp` (KEEP `CasFoldSeal.EncodingIsByteDeterministic`) + a battery row + a text-specific determinism test

**Determinism (hard requirement):** `cas_fold_seal` is `PinnedRaw` + `Strict` and goes through `putDeterministicArtifact` (a retrying leader re-encodes and the backend `casPut` rejects any byte drift as `CORRUPTED_DATA`). The text encoder MUST be byte-reproducible: **fixed field order**, **every collection emitted sorted** — `per_ns_shard` / `condemned_summary` / `ns_cleanup_items` are `std::map` (already key-sorted; emit in iteration order), `blob_target_runs` / `part_manifest_cleanup` are `std::vector` and MUST be sorted by `key` inside the encoder (as `addRuns` does today). No floats exist anywhere in the struct, so `writeIntText`/hex are the only numeric writers — no formatting drift. **String determinism depends on Task 1 Step 0's pinned JSON write settings**: fold-seal string values are dense with `/` (map keys `ns/shard`, run keys `gc/.../run0`, token values) — the `escape_forward_slashes = false` pin is what makes their bytes CAS-owned and reproducible across a retry, rather than hostage to a global default. The existing `CasFoldSeal.EncodingIsByteDeterministic` test (two encodes of the same seal byte-equal) MUST keep passing; add `CasFoldSealFormat.TextIsByteDeterministic` that builds a seal with intentionally out-of-order run vectors and asserts two encodes are byte-equal.

**Body shape:** `Control` in role but with FIVE repeated collections + two scalars, and a 64 KiB line cap → line-structured, materialized whole. Fixed layout: header line, a `meta` line, then tagged record lines in a **fixed section order** (coverage → blob-target-runs → part-manifest-cleanup → condemned-summary → ns-cleanup), then a `{"n":<total record count>}` trailer. Each record carries a `k` discriminator. `PinnedRaw` → stored bytes == text (no compression). (`part_manifest_cleanup` is removed in phase 5; it stays here in phase 2.)

**Interfaces:**
- Produces: `Formats/CasFoldSealFormat.h` with `struct RunRef`, `struct ShardCoverage`, `struct CondemnedSummary`, `enum class RefNsCleanupState`, `struct RefNsCleanupItem`, `struct CasFoldSeal` + `encodeFoldSeal`/`decodeFoldSeal`. Record discriminators: `cov` (coverage), `btr` (blob-target run), `pmc` (part-manifest-cleanup run), `cnd` (condemned summary), `nsc` (ns-cleanup item). Golden sketch (one of each):

```
{"type":"cas_fold_seal","v":3}
{"g":"5","pg":"4"}
{"k":"cov","key":"ns1/0","cls":2,"tt":"etag","tv":"t-1","lfe":"7","lfs":"11"}
{"k":"btr","key":"gc/.../run0","ck":"00...0f","shard":0,"gen":"5"}
{"k":"cnd","shard":0,"ct":3,"pt":1,"ocr":"4"}
{"k":"nsc","ns":"srv/uuid","rte":"7","rts":"9","st":"pending"}
{"n":4}
```

(`g`/`pg`/`lfe`/`lfs`/`gen`/`rte`/`rts` = unbounded counters → strings; `cls`/`shard`/`ct`/`pt` = bounded → numbers; `ocr` = `oldest_nonpending_condemn_round`, `UINT64_MAX` sentinel → string; `ck` = `RunRef.checksum` (UInt128) → 32-hex; token via `tt`/`tv`; state word via `st`.)

- [ ] **Step 1: Failing test** — `gtest_cas_fold_seal_format.cpp` with the battery row (a seal containing one of each record), a re-pointed `EncodingIsByteDeterministic`, and:

```cpp
TEST(CasFoldSealFormat, TextIsByteDeterministic)
{
    CasFoldSeal a;
    a.generation = 5; a.parent_generation = 4;
    a.blob_target_runs = {RunRef{"z", UInt128(2), 1, 5}, RunRef{"a", UInt128(1), 0, 5}};
    CasFoldSeal b = a;
    std::reverse(b.blob_target_runs.begin(), b.blob_target_runs.end());  /// same set, different order
    EXPECT_EQ(encodeFoldSeal(a), encodeFoldSeal(b));   /// encoder must sort runs by key
}
```

- [ ] **Step 2: Verify compile failure.**

- [ ] **Step 3: Implement** — `Core/Formats/CasFoldSealFormat.h` (structs move verbatim from `CasGenerationSeal.h`; reproduce the full struct set — `RunRef`, `ShardCoverage`, `CondemnedSummary`, `RefNsCleanupState`, `RefNsCleanupItem`, `CasFoldSeal` — with their existing doc comments and `operator==`; includes `CasManifestId.h`, `CasRefIds.h`, `CasToken.h`). `Core/Formats/CasFoldSealFormat.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasWireVocab.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
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

std::string_view nsCleanupStateToWord(RefNsCleanupState s)
{
    switch (s)
    {
        case RefNsCleanupState::Pending:   return "pending";
        case RefNsCleanupState::Completed: return "completed";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown ns-cleanup state {}", static_cast<int>(s));
}

RefNsCleanupState nsCleanupStateFromWord(std::string_view w)
{
    if (w == "pending")   return RefNsCleanupState::Pending;
    if (w == "completed") return RefNsCleanupState::Completed;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown ns-cleanup state '{}'", w);
}

/// Emit one run record (`k` = "btr" or "pmc"); the caller sorts the vector by key first.
void writeRun(WriteBuffer & out, std::string_view kind, const RunRef & r)
{
    bool first = true;
    writeKey(out, "k", first);     writeStringValue(out, kind);
    writeKey(out, "key", first);   writeStringValue(out, r.key);
    writeKey(out, "ck", first);    writeHex128Value(out, r.checksum);
    writeKey(out, "shard", first); writeIntText(r.shard, out);
    writeKey(out, "gen", first);   writeU64StringValue(out, r.generation);
    closeObject(out, first);
    writeChar('\n', out);
}

void writeSortedRuns(WriteBuffer & out, std::string_view kind, std::vector<RunRef> runs)
{
    std::sort(runs.begin(), runs.end(), [](const RunRef & a, const RunRef & b) { return a.key < b.key; });
    for (const RunRef & r : runs)
        writeRun(out, kind, r);
}

}

String encodeFoldSeal(const CasFoldSeal & seal)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::FoldSeal);

    /// meta line
    {
        bool first = true;
        writeKey(out, "g", first);  writeU64StringValue(out, seal.generation);
        writeKey(out, "pg", first); writeU64StringValue(out, seal.parent_generation);
        closeObject(out, first);
        writeChar('\n', out);
    }

    uint64_t n = 0;

    /// coverage (std::map => key-sorted)
    for (const auto & [key, cov] : seal.per_ns_shard)
    {
        bool first = true;
        writeKey(out, "k", first);    writeStringValue(out, "cov");
        writeKey(out, "key", first);  writeStringValue(out, key);
        writeKey(out, "cls", first);  writeIntText(cov.classification, out);
        writeTokenFields(out, first, cov.folded_token);   /// tt + tv
        writeKey(out, "lfe", first);  writeU64StringValue(out, cov.last_folded_ref_id.writer_epoch);
        writeKey(out, "lfs", first);  writeU64StringValue(out, cov.last_folded_ref_id.ref_sequence);
        closeObject(out, first);
        writeChar('\n', out);
        ++n;
    }

    writeSortedRuns(out, "btr", seal.blob_target_runs);   n += seal.blob_target_runs.size();
    writeSortedRuns(out, "pmc", seal.part_manifest_cleanup); n += seal.part_manifest_cleanup.size();

    /// condemned summary (std::map<uint64> => shard-sorted)
    for (const auto & [shard, s] : seal.condemned_summary)
    {
        bool first = true;
        writeKey(out, "k", first);     writeStringValue(out, "cnd");
        writeKey(out, "shard", first); writeIntText(shard, out);
        writeKey(out, "ct", first);    writeIntText(s.condemned_total, out);
        writeKey(out, "pt", first);    writeIntText(s.pending_total, out);
        writeKey(out, "ocr", first);   writeU64StringValue(out, s.oldest_nonpending_condemn_round);
        closeObject(out, first);
        writeChar('\n', out);
        ++n;
    }

    /// ns-cleanup items (std::map<String> => key-sorted)
    for (const auto & [key, item] : seal.ns_cleanup_items)
    {
        bool first = true;
        writeKey(out, "k", first);   writeStringValue(out, "nsc");
        writeKey(out, "ns", first);  writeStringValue(out, item.ns.string());
        writeKey(out, "rte", first); writeU64StringValue(out, item.remove_txn_id.writer_epoch);
        writeKey(out, "rts", first); writeU64StringValue(out, item.remove_txn_id.ref_sequence);
        writeKey(out, "st", first);  writeStringValue(out, nsCleanupStateToWord(item.state));
        closeObject(out, first);
        writeChar('\n', out);
        ++n;
    }

    writeTrailerLine(out, n);
    out.finalize();
    return out.str();
}

CasFoldSeal decodeFoldSeal(std::string_view data)
{
    ReadBufferFromMemory in(data.data(), data.size());
    expectHeaderLine(in, FormatId::FoldSeal);
    const uint64_t line_cap = traitsFor(FormatId::FoldSeal).line_cap;

    CasFoldSeal seal;

    /// meta line
    {
        const String meta = readLine(in, line_cap, "fold seal");
        ReadBufferFromMemory m(meta.data(), meta.size());
        JsonObjectReader r(m, KeyStrictness::Strict, "fold seal");
        String key;
        while (r.nextKey(key))
        {
            if (key == "g") seal.generation = r.readU64String();
            else if (key == "pg") seal.parent_generation = r.readU64String();
            else r.skipUnknown(key);   /// Strict => any unknown key is CORRUPTED_DATA
        }
    }

    uint64_t seen = 0;
    while (true)
    {
        const String line = readLine(in, line_cap, "fold seal");
        ReadBufferFromMemory l(line.data(), line.size());
        JsonObjectReader r(l, KeyStrictness::Strict, "fold seal");
        String key;
        if (!r.nextKey(key))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: empty line");

        if (key == "n")
        {
            const uint64_t n = r.readU64Number();
            if (r.nextKey(key))
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: trailer has extra keys");
            if (!l.eof() || !in.eof())
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: bytes after trailer");
            if (n != seen)
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS fold seal: trailer count {} != {} records", n, seen);
            return seal;
        }
        if (key != "k")
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: record must start with \"k\"");
        const String kind = r.readString();

        if (kind == "cov")
        {
            String map_key, tv; TokenType tt{}; bool have_tt = false;
            ShardCoverage cov;
            while (r.nextKey(key))
            {
                if (key == "key") map_key = r.readString();
                else if (key == "cls") cov.classification = static_cast<uint8_t>(r.readU64Number());
                else if (key == "tt") { tt = tokenTypeFromWord(r.readString(), "fold seal"); have_tt = true; }
                else if (key == "tv") tv = r.readString();
                else if (key == "lfe") cov.last_folded_ref_id.writer_epoch = r.readU64String();
                else if (key == "lfs") cov.last_folded_ref_id.ref_sequence = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown cov key '{}'", key);
            }
            if (!have_tt)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: cov missing tt");
            cov.folded_token = Token{tv, tt};
            seal.per_ns_shard[map_key] = cov;
        }
        else if (kind == "btr" || kind == "pmc")
        {
            RunRef run;
            while (r.nextKey(key))
            {
                if (key == "key") run.key = r.readString();
                else if (key == "ck") run.checksum = r.readHex128();
                else if (key == "shard") run.shard = r.readU64Number();
                else if (key == "gen") run.generation = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown run key '{}'", key);
            }
            (kind == "btr" ? seal.blob_target_runs : seal.part_manifest_cleanup).push_back(run);
        }
        else if (kind == "cnd")
        {
            uint64_t shard = 0; CondemnedSummary s;
            while (r.nextKey(key))
            {
                if (key == "shard") shard = r.readU64Number();
                else if (key == "ct") s.condemned_total = r.readU64Number();
                else if (key == "pt") s.pending_total = r.readU64Number();
                else if (key == "ocr") s.oldest_nonpending_condemn_round = r.readU64String();
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown cnd key '{}'", key);
            }
            seal.condemned_summary[shard] = s;
        }
        else if (kind == "nsc")
        {
            String ns; RefTxnId txn; RefNsCleanupState st = RefNsCleanupState::Pending; bool have_st = false;
            while (r.nextKey(key))
            {
                if (key == "ns") ns = r.readString();
                else if (key == "rte") txn.writer_epoch = r.readU64String();
                else if (key == "rts") txn.ref_sequence = r.readU64String();
                else if (key == "st") { st = nsCleanupStateFromWord(r.readString()); have_st = true; }
                else throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown nsc key '{}'", key);
            }
            if (!have_st)
                throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: nsc missing st");
            const String map_key = ns + "\n" + renderRefTxnId(txn);   /// mirrors the proto decoder's key
            seal.ns_cleanup_items[map_key] = RefNsCleanupItem{RootNamespace{ns}, txn, st};
        }
        else
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: unknown record kind '{}'", kind);

        if (!l.eof())
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS fold seal: junk after record");
        ++seen;
    }
}

}
```

**Note (implementer):** the `nsc` record rebuilds the `std::map` key exactly as the old proto decoder did (`ns + "\n" + renderRefTxnId(remove_txn_id)`); `renderRefTxnId` throws `LOGICAL_ERROR` on a `{0,0}` id, which is correct — an `nsc` item always has a nonzero `remove_txn_id`. `cov`'s `last_folded_ref_id` MAY be `{0,0}` ("nothing folded yet") — that is why it uses the plain `lfe`/`lfs` decimal-string keys, NOT `renderRefTxnId`. Delete `Core/CasGenerationSeal.{h,cpp}`, rewire includers (`sed` → `Formats/CasFoldSealFormat.h`). Migrate all six `gtest_cas_generation_seal.cpp` tests + the `gtest_cas_gc_formats.cpp::FoldSealCondemnedSummaryRoundTrips` test into `gtest_cas_fold_seal_format.cpp`, re-pointed at the text codec; KEEP `EncodingIsByteDeterministic` (it now protects the text encoder's determinism).

- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasFormatBattery.FoldSeal:CasFoldSeal*'` green; the GC fold/resume suites (`gtest_cas_gc_fold`, `gtest_cas_gc_resume`, `gtest_cas_gc_round*`) still green (they seal + re-adopt fold seals through `putDeterministicArtifact`).

- [ ] **Step 5: Commit** — `cas: formats v3 phase 2 — cas_fold_seal text cutover (deterministic record body)` + trailer.

---

### Task 7: Delete the protobuf graveyard + finalize the registry {#task7}

**Files:**
- Delete: `Core/Proto/cas_format.proto`, `Core/Proto/CMakeLists.txt`, and the empty `Core/Proto/` dir
- Modify: `src/CMakeLists.txt` (remove the `clickhouse_cas_proto` wiring), `Core/Formats/README.md` (flip the phase-2 rows from `*`-legacy to done)

**Precondition — grep-proof zero references.** After Tasks 2–6, exactly the five production files that included `<cas_format.pb.h>` (`CasPoolMeta.cpp`, `CasServerRoot.cpp`, `CasGcFormats.cpp`→deleted, `CasGcOutcomes.cpp`→deleted, `CasGenerationSeal.cpp`→deleted) and the tests that included it no longer do. The corrected object inventory confirms these eight control-plane objects were the ONLY protobuf consumers (refsnaplog / runs / part-manifest / blob / blob-meta are all custom-binary, never proto), so proto dies **entirely** in phase 2 — the spec's "phase 8 removes protobuf build wiring" is pulled forward here because there is nothing left in phases 3–7 that references it. Gate the deletion on:

```bash
cd /home/mfilimonov/workspace/ClickHouse/master   # (or the active worktree)
grep -rn 'cas_format.pb.h\|clickhouse::cas::format\|cas_format\.proto' src/ ; echo "EXPECT: no output"
grep -rn 'clickhouse_cas_proto' src/CMakeLists.txt ; echo "(the lines to delete)"
```
If the first grep prints ANY line, STOP — a codec still references proto; that object's task is incomplete. Do not delete the wiring until it is clean (fail-closed).

- [ ] **Step 1: Delete the proto sources**

```bash
git rm src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_format.proto \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/CMakeLists.txt
```

- [ ] **Step 2: Remove the CMake wiring** in `src/CMakeLists.txt`:
  - Delete the `add_subdirectory(.../Core/Proto)` line (grep `Core/Proto` in `src/CMakeLists.txt` — the phase that introduced the proto added it; it may live near the CA source-group registration around line ~134 or a dedicated block).
  - Delete `target_link_libraries(dbms PRIVATE clickhouse_cas_proto)` (≈ line 708).
  - Delete the test-side block `if (TARGET clickhouse_cas_proto) … target_link_libraries(unit_tests_dbms PRIVATE clickhouse_cas_proto) … endif()` (≈ lines 906-908).
  - Verify no other `clickhouse_cas_proto` reference remains: `grep -rn clickhouse_cas_proto src/CMakeLists.txt` → empty.

- [ ] **Step 3: Finalize `Core/Formats/README.md`** — flip the eight phase-2 bucket-map rows from the phase-1 `*` (legacy binary) marker to their real `Formats/` codec, and confirm the codec-table note points at the now-complete control plane. Leave the refsnaplog / runs / manifest / blob / blob-meta rows marked `*` (phases 3–7).

- [ ] **Step 4: Full clean build + the whole CAS slice**

```bash
flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p2t7.log 2>&1; echo "NINJA_EXIT=$?"
build_debug/src/unit_tests_dbms --gtest_filter='Cas*' 2>&1 | tail -5
```
Expected: `NINJA_EXIT=0` (dbms + unit_tests_dbms link WITHOUT the proto library — proves nothing references it); all `Cas*` green. Use a subagent to analyze the build log and return a summary.

- [ ] **Step 5: Commit** — `git add src/CMakeLists.txt`, the deleted proto files (staged by `git rm`), `Core/Formats/README.md`:

```bash
git commit -m "cas: formats v3 phase 2 — remove the protobuf graveyard (clickhouse_cas_proto)

All eight control-plane codecs are text (Tasks 2-6); the corrected object
inventory confirms they were the only protobuf consumers, so cas_format.proto,
the clickhouse_cas_proto target, and its dbms/unit_tests_dbms link edges are
deleted. Grep-gated: zero references to cas_format.pb.h / clickhouse::cas::format
remain. (Pulled forward from the spec's phase 8, which now carries only the
provider-metadata mirror, docs, and CasInspect.)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27"
```

## Phase-2 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green; `dbms` + `unit_tests_dbms` link with NO `clickhouse_cas_proto`.
- Eight control-plane objects are text (`cas_pool_meta`, `cas_owner`, `cas_epoch`, `cas_mount_lease`, `cas_gc_state`, `cas_gc_hb`, `cas_gc_outcomes`, `cas_fold_seal`); each has a `FormatBatteryCase` row; the phase-1 toy battery instance is gone.
- `cas_gc_hb` is header'd text (last unversioned object eliminated); `cas_fold_seal` byte-determinism preserved (both the kept `EncodingIsByteDeterministic` and the new text determinism test pass); `cas_gc_outcomes` stored under `.zst`.
- `Core/Proto/` deleted; `Formats/README.md` control-plane rows finalized.
- Phases 3–8 get their own JIT plans against this foundation, per the DAG below.

## Phases 3–8: dependency map {#phase-dag}

This is a pipelining DAG, NOT detailed plans — each phase gets its own just-in-time writing-plans pass against the then-current tree. "Consumes" lists the concrete artifacts a phase needs from earlier phases; "Parallel-draftable" says whether the plan + code can be written against a **frozen interface** while the predecessor's integration is still in flight. The one interface every downstream phase freezes against is the **shared wire-value vocabulary** introduced in phase-2 Task 1 (`writeToken`/`readToken`, `writeBlobRef`/`readBlobRef`, the enum word-maps) — once its signatures land, phases 3/4/7 no longer wait on the rest of phase 2.

### Phase 3 — Refsnaplog (`cas_ref_log` + `cas_ref_snap`) {#dag-phase3}

**Scope:** convert the two custom-binary refsnaplog codecs (`CasRefLogCodec`, `CasRefSnapshotCodec`) to `Control`-family text under the `Always`/`.zst` policy. Move the codecs to `Formats/CasRefLogFormat` / `Formats/CasRefSnapshotFormat` with their wire structs; re-derive `ref_txn_max_bytes` and the removal-class byte budgets for JSON inflation (deferred to plans by the spec); keep the key↔body binding invariant (decoded `ns`/`txn_id` must equal the key read from) and extend the same idea documented for the manifest. Thread the `.zst` `storedSuffix` through `refLogKey`/`refSnapshotKey` construction and through the key **parsers** (`parseRefObjectKey`, fsck/sweep classifiers strip the suffix).

**Consumes:** phase-1 `CasTextFormat` shape; **phase-2 Task 1** shared wire-value vocabulary (`Token`, `BlobRef`, `ManifestRef`) — refsnaplog embeds `ManifestRef` and `BlobRef` in its records; the `checkManifestRef` / `checkCanonicalRefName` invariants from `CasCodecUtil.h` (relocate them into `Formats/` alongside the codec, or into the shared vocabulary). Independent of every phase-2 CONTROL codec (pool meta, gc, server root).

**Parallel-draftable:** **YES, gated on phase-2 Task 1.** The refsnaplog codecs share no code with the phase-2 control codecs; only the shared wire-value helper signatures must be frozen first. Draft the plan and codecs against those signatures while phase-2 Tasks 3–9 integrate.

**Risk:** highest of 3/4/7 — byte-budget re-derivation is a correctness gate (a JSON blow-up past `ref_txn_max_bytes` changes append behavior); the `.zst` key-suffix touches many key parsers/classifiers across fsck and the staging sweeper; the key↔body binding must survive re-encode. `ManifestRef` sub-object rendering is first introduced here (fold seal in phase 2 uses `RefTxnId`, not `ManifestRef`).

### Phase 4 — Blob meta (`cas_blob_meta`) {#dag-phase4}

**Scope:** convert the fixed 22-byte binary sidecar (`CasBlobMeta`: state / condemn_round / size) to one-line `Control` JSON; move the `BlobMeta` struct + `encodeBlobMeta`/`decodeBlobMeta` to `Formats/CasBlobMetaFormat`, leaving the CAS-lifecycle helpers (dedup/resurrect token semantics) in `Core/CasBlobMeta`. Token/dedup/resurrect semantics are byte-for-byte unchanged — only the encoding moves.

**Consumes:** phase-1 `CasTextFormat` shape only (the sidecar body is 3 scalars + a state enum; it does not embed a `BlobRef` — the ref lives in the key). Independent of phases 2/3 codecs.

**Parallel-draftable:** **YES, fully, right after phase 1.** Smallest object in the whole migration; no shared-vocabulary dependency. Can be drafted and even implemented before or alongside any phase-2 task; sequenced after phase 2 in the spec only for reviewer bandwidth, not for a code dependency.

**Risk:** low. Only subtlety: the `state`/token semantics of the resurrect gate must stay exactly as the CAS-swap logic expects — the codec change must not perturb which byte pattern means "condemned". Golden test pins the one line.

### Phase 5 — Runs (`cas_run`, `RecordStream`) {#dag-phase5}

**Scope:** the data-plane rewrite. Convert `CasRunFile` → `Formats/CasRecordStreamFormat`: sorted NDJSON records + `{"n":…}` trailer, whole-file seal-checksum (`RunRef.checksum`, CityHash128) accumulated on every full read and verified before use, typed opens (`openSourceEdgeRun` validates `type`/`v`/`kind`). DELETE the `CARN` block/footer machinery, `RunFileReader::seek`, `inDegreeInGeneration`, `SourceEdgeKeyCodec::seekPrefix`, the part-manifest-cleanup run, the fold seal's `part_manifest_cleanup` field, and `partManifestCleanupKey`; rewrite the k-way merger line-based. `PinnedRaw` + `Strict` (deterministic, byte-adoption).

**Consumes:** phase-1 `CasTextFormat`; **phase-2 Task 1** wire-value vocabulary (`BlobRef`, `Token`, marker words); **phase-2 `CasFoldSealFormat`** — phase 5 REMOVES the `part_manifest_cleanup` field from the text fold seal that phase 2 wrote, and removes `RunRef` entries it referenced. That field removal must layer on the phase-2 text fold seal, not the proto one.

**Parallel-draftable:** **PARTIAL.** The run codec itself (NDJSON writer/reader/merger + seal-checksum) is independent and draftable in parallel with phase 2 against the wire-value vocabulary. But the coupled deletions — the fold-seal `part_manifest_cleanup` field, `partManifestCleanupKey` — must land AFTER phase-2's `cas_fold_seal` cutover (they edit a phase-2 artifact). Split the phase-5 plan so the run-codec tasks draft early and the fold-seal-field-removal task depends on phase 2.

**Risk:** highest overall — determinism (byte-adoption must survive the encoding change), the k-way merger rewrite, seal-checksum-on-every-read across all live consumers (fold merge, `zeroInDegree`, orphan scan, `fsck`), and the ≈2× byte cost the spec flags for soak measurement (re-binarize `cas_run` only is the localized fallback).

### Phase 6 — Part manifest (`cas_part_manifest`, `PayloadHybrid`) {#dag-phase6}

**Scope:** convert `CasManifestCodec` to a JSON descriptor (one line per entry: `path` + inline `off`/`len` or `blob` ref) followed by a `head -v`-banner raw payload zone, `Always`/`.zst`. Regenerate + verify banners/padding as a deterministic function of the entries (no smuggling). DELETE the embedded `CARN` stream path, `RunKind::ManifestEntries`, and `payload_digest`. Enforce inline caps (1 MiB/entry, 16 MiB total) and the descriptor line cap.

**Consumes:** phase-1 `CasTextFormat` shape; **phase-2 Task 1** wire-value vocabulary (`BlobRef` for large-file entries); **phase-5** — CORRECTED at phase-5 JIT planning (2026-07-15): `RunKind::ManifestEntries` and the CARN embedded-stream framing are NOT deleted by phase 5 — `CasManifestCodec` still consumes them with no private framing, so `Core/CasRunFile.{h,cpp}` stays ALIVE through phase 5 (pruned of `seek`/`RunMerger`/dead kinds) as the phase-6-owned embedded-manifest codec; phase 6 deletes it entirely. See the phase-5 plan, FLAG 1.

**Parallel-draftable:** **NO.** Sequence after phase 5: the embedded-`CARN`-stream deletion and `RunKind::ManifestEntries` removal are shared surface. Drafting phase 6 against a not-yet-refactored run layer would race the phase-5 deletions.

**Risk:** medium — banner/padding byte-regeneration must be exact (a verify mismatch is `CORRUPTED_DATA`); the inline-vs-blob entry split and caps; the in-memory serve path (`PartFolderView`) must keep reading inline bytes from the decoded manifest, not range-read the object.

### Phase 7 — Blob envelope (`cas_blob`, `PayloadHybrid`) {#dag-phase7}

**Scope:** convert the 70-byte binary core + TLV envelope (`CasEnvelope`) to a 256-byte JSON header line (`{type,v,tag,bld,ts,by,op,ch,ref}`) padded with spaces to byte 255 + `\n`, payload at the constant offset `blob_header_len` (stays 256). Drop `hash_algo` / `domain_id` / `header_hash` / `writer_version`; TLV → `!`-critical keys; verify the pad zone is exactly spaces-then-newline. Re-pin golden tests; keep header-built-before-payload (S3-native staging).

**Consumes:** phase-1 `CasTextFormat` write-vocabulary (`writeKey`/`writeStringValue`/`writeU64StringValue`/`writeHex128Value`); the `ProvenanceOp` word-map (add to the phase-2 enum vocabulary if convenient, else local). Independent of phases 3/4/5/6 codecs.

**Parallel-draftable:** **YES, fully, after phase 1.** The envelope shares no code with any other codec; only the phase-1 write-vocabulary is needed. Draft in parallel with any phase. (It is NOT a `Control` object — it is the one hot ranged-read object — so it does not depend on the control-plane cutover at all.)

**Risk:** medium — the 256-byte budget is tight: `ref` must be truncated so the whole line fits byte 255 (the spec computes ~54 chars of headroom); the pad-verify must be exact; the header must still be constructible BEFORE the payload streams (staging). Golden re-pin is mandatory (byte layout changes).

### Phase 8 — Finish (wiring, mirror, docs, `CasInspect`) {#dag-phase8}

**Scope (reduced):** the switch-flips and hygiene that remain after phase 2 already removed the protobuf graveyard (see Task 7 — proto dies in phase 2 because the corrected inventory shows the eight control-plane objects were its only consumers). Phase 8 is therefore: the provider-metadata mirror in the backend PUT path (`Content-Type` per family; `x-amz-meta-cas` = the header-line copy; `application/zstd` for `.zst` objects) — an OPT-IN convenience the protocol never reads; docs finalization (`codecs_proposal_v3.md` dispositions, `05-formats-and-backend.md` envelope + evolution sections, retitle `codecs.md` as the pre-v3 historical audit, final pass over `Formats/README.md`); and gutting `CasInspect` to a thin "decompress + print" or deleting it. NOTE: the `protobuf_generate_cpp` / `clickhouse_cas_proto` / `libprotoc` removal is NOT here — it landed in phase-2 Task 7.

**Consumes:** EVERY phase-2–7 object converted (the backend PUT path can only mirror header lines once every object HAS one). The provider-metadata mirror consumes the settled per-family `Content-Type` map and the header-line copy from `CasTextFormat`.

**Parallel-draftable:** **NO — strictly last.** The mirror + docs finalization summarize the completed state; every prior phase must have landed (the header-line-per-object property the mirror relies on is only universal after phase 7).

**Risk:** low–medium — the provider-metadata mirror is opt-in and fail-close (dropped by copy tools, absent on the local backend — nothing may depend on reading it back). With the build-system surgery already done in phase 2, phase 8 is mostly docs + one backend PUT hook; gate on a clean full `unit_tests_dbms` + a soak that inspects a live object's `x-amz-meta-cas`.

### DAG summary {#dag-summary}

| Phase | Object(s) | Parallel-draftable? | Gate / predecessor |
|---|---|---|---|
| 3 | `cas_ref_log`, `cas_ref_snap` | YES | freeze phase-2 Task 1 wire-value vocabulary |
| 4 | `cas_blob_meta` | YES (fully) | phase 1 only |
| 5 | `cas_run` | PARTIAL | run-codec parallel; fold-seal-field-removal after phase 2 |
| 6 | `cas_part_manifest` | NO | after phase 5 (`RunKind::ManifestEntries`) |
| 7 | `cas_blob` | YES (fully) | phase 1 only |
| 8 | mirror/docs/CasInspect (proto wiring already gone in phase 2) | NO | after ALL of 2–7 |

Critical path: **1 → 2 → 5 → 6 → 8**. Phases 3, 4, 7 hang off the freeze points and fill idle drafting capacity. The tightest real coupling is phase 5 → phase 6 (shared `RunKind`/embedded-stream surface) and everything → phase 8 (proto-wiring removal).

