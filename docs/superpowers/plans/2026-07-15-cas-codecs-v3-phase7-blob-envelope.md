---
description: 'Implementation plan for CAS codecs v3 phase 7: converting the 70-byte binary blob envelope core + TLV to a 256-byte JSON header line padded with spaces to byte 255 + newline, with an exact ref-truncation budget, exact pad-zone verification, and CityHash64 leaving the envelope. Independent of the control-plane cutover; layers on current mainline.'
sidebar_label: 'CAS codecs v3 phase 7 plan'
sidebar_position: 67
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase7-blob-envelope
title: 'CAS Codecs V3 — Phase 7: Blob-Envelope Text Cutover'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 7: Blob-Envelope Text Cutover Implementation Plan {#cas-codecs-v3-phase7}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Base assumption (explicit):** this plan layers on **current mainline HEAD** — NOT on the phase-2 or phase-4 drafts. The blob envelope shares no code with the control-plane codecs (it is the one hot ranged-read object, `PayloadHybrid` family), so its only dependency is the phase-1 write vocabulary in `Core/Formats/CasTextFormat` (`writeKey`, `writeStringValue`, `writeHex128Value`, `closeObject`, `JsonObjectReader`, `checkCompatibility`, `traitsFor`, `FormatId::Blob`), all landed on mainline. **Phase 7's patch is independent and can integrate before OR after phase 2/4.** When drafting: branch `tmp/worktrees/draft-codecs7` from mainline HEAD, implement on top, document independence in DRAFT.md.

**No dependency on the phase-2 `escape_forward_slashes=false` pin (design decision — see [§ref-escaper](#ref-escaper)):** the envelope `ref` value is written by a LOCAL fixed escaper that never escapes `/`, so the 256-byte budget arithmetic and the golden bytes are deterministic regardless of the global `FormatSettings::JSON` default. This is what makes phase 7 truly mainline-independent. The other string fields (`type` = `cas_blob`, `op` word) contain no `/`, so the global setting does not affect them.

**Goal:** convert the blob envelope from the 70-byte binary core + TLV (`CasEnvelope`) to a fixed-length **256-byte JSON header line**: a single JSON object at bytes `[0, json_len)`, ASCII spaces at `[json_len, blob_header_len-1)`, and `\n` at byte `blob_header_len-1`. Payload stays at the constant offset `blob_header_len` (256, a `PoolMeta` parameter). Drop `hash_algo` / `domain_id` / `header_hash` / `writer_version`; TLV extensions become `!`-critical JSON keys; the header is still built BEFORE the payload streams (S3-native staging). `CityHash64` leaves the envelope entirely (the `header_hash` recipe is deleted, no consumer). The struct + codec move to `Core/Formats/CasBlobEnvelopeFormat`; `CasBuild` (writer) and `CasInspect` (introspection) are rewired; golden tests are re-pinned.

**Architecture:** `cas_blob` is `PayloadHybrid` (`Never` compression; the payload identity is the raw bytes — no zstd on the object). Unlike the control-plane codecs it does NOT use `writeHeaderLine`/`readLine` — it builds one JSON object then a fixed-width space+newline pad, and parses the object directly from byte 0 then verifies the pad zone. `blobKey` and its parser are UNCHANGED (the envelope is the blob body; its key is the content address). Never was protobuf.

**Tech Stack:** C++ (ClickHouse `dbms`), the phase-1 `CasTextFormat` write vocabulary + `JsonObjectReader`, `ReadHelpers`/`WriteHelpers`, gtest (`unit_tests_dbms`).

## Global Constraints {#global-constraints}

- **Allman braces** everywhere.
- **Layering (physical):** `Core/Formats/CasBlobEnvelopeFormat.h` may include only `Formats/CasFormat.h`, `Formats/CasTextFormat.h`, `Core/CasEnvelope.h`-relocated types (`ObjectKind`, `ProvenanceOp`, `Provenance` move WITH the codec — see below), `base/`, IO primitives. NEVER `CasBackend.h`/`CasStore.h`/`CasBuild.h`. The writer logic that builds a header from a `Store`/`PoolMeta` STAYS in `Core/CasBuild.cpp`.
- **Pre-release, hard cutover, no dual-read:** the binary envelope codec is deleted and replaced by text in one commit. No "sniff binary vs JSON".
- **Error taxonomy:** bad `type` / truncated object / pad-zone violation / duplicate key / over-256 → `CORRUPTED_DATA`; future `v` / unknown `!`-key → `UNKNOWN_FORMAT_VERSION`.
- **`v` stamping stays at `G_BUILD` = 3** (`currentCompatibilityVersion`); the header `v` IS the former core `compatibility_version` slot. No `changePoints` append.
- **JSON value conventions:** `tag`/`bld`/`by` → 32-char lowercase hex; `ts` (unix ms) and `ch` (`VERSION_INTEGER`) → JSON **numbers** (both structurally < 2^53); `op` → full word; `ref` → JSON string via the local escaper, truncated to fit (see below). `blob_header_len` (256) is a pool-creation parameter, NOT serialized (derived on read from the `\n` position).
- **`blobKey` UNCHANGED** — the envelope is the blob body at the content-address key; no `.zst`, no key or parser change.
- **`Never` compression** — the object is never zstd-wrapped (payload identity = raw bytes). The battery `encode`/`decode` still route through `sealObject`/`openObject` (identity for `Never`) for harness uniformity, but the write/read paths do NOT.
- **Header built BEFORE payload** (S3-native staging): `encodeEnvelopeHeader` returns the fixed 256-byte header with no payload dependency — preserved (the current core was already made payload-independent when `logical_size`/`logical_hash` were dropped 2026-07-11).
- **Golden re-pin is MANDATORY** (byte layout changes completely). `CasByteOrderGolden.EnvelopeLittleEndian` (binary) is deleted; a v3 text golden replaces it.
- **Battery is a hard gate:** `cas_blob` gains exactly one `FormatBatteryCase` row.
- **Build/commit discipline** (as prior phases): `flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p7t<N>.log 2>&1; echo "NINJA_EXIT=$?"`, foreground only, subagent-analyze, commit per task, explicit-path `git add`, trailer:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## The 256-byte layout and the exact budget {#budget}

`blob_header_len` = `L` (256 for blob pools, a `PoolMeta` field; the code treats it as a parameter). The header is EXACTLY `L` bytes:

```
byte 0 .............. json_len-1   the JSON object  {"type":"cas_blob","v":3,"tag":..,"ref":"<escaped>"}
byte json_len ....... L-2          ASCII space (0x20) pad
byte L-1                            '\n' (0x0A)
```

Invariant: `json_len <= L-1` (byte `L-1` is reserved for `\n`; when `json_len == L-1` the pad zone is empty).

**Encode truncation (exact, deterministic):**

1. Build `PREFIX` = the JSON object text up to and including the opening quote of `ref` — i.e. `{"type":"cas_blob","v":<v>,"tag":"<32hex>","bld":"<32hex>","ts":<ts>,"by":"<32hex>","op":"<word>","ch":<ch>,"ref":"` — using the ACTUAL field values (numbers `ts`/`ch` are variable width; `op` is a variable word). Plus any `!`-critical extension keys go BEFORE `ref` (so `ref` is always last and is the only truncated field).
2. `SUFFIX` = `"}` (2 bytes: closing quote + `}`).
3. `budget = (L - 1) - PREFIX.size() - SUFFIX.size()` = the max ESCAPED-`ref` byte count. If `budget < 0` → `LOGICAL_ERROR` (the non-`ref` fields overflow `L-1`; impossible with bounded fields — `3*32` hex + a ~13-digit `ts` + ~8-digit `ch` + a ≤8-char `op` + the fixed skeleton is ≈193 bytes, leaving ≈54 for `ref` at `L`=256; the throw asserts the invariant rather than silently overflowing).
4. Escape `ref` incrementally: for each raw char, compute its escaped form (`"`→`\"`, `\`→`\\`, a byte `< 0x20`→`\uXXXX` (6 bytes), everything else including `/` and printable ASCII → 1 byte); append only if the running escaped length stays `<= budget`; STOP at the first char that would overflow (never split an escape or a UTF-8 continuation — the `< 0x20` rule keeps multibyte UTF-8 lead/continuation bytes as-is, so a truncation never lands mid-escape; a truncation between UTF-8 continuation bytes is acceptable for a diagnostic string and never produces invalid JSON because no byte `>= 0x20` is ever escape-expanded).
5. `json = PREFIX + escaped_ref + SUFFIX`; `json_len = json.size() <= L-1`.
6. Emit `json`, then `(L-1) - json_len` spaces, then `\n`. Total = `L` bytes exactly.

**Decode + pad-verify (exact):** parse the JSON object from byte 0 with `JsonObjectReader` (it consumes through the matching `}`, leaving the cursor at `json_len`). Then consume the pad: every byte in `[json_len, ?)` must be `0x20` until the terminating `0x0A`; the first non-space must be `\n`. `header_len = (index of that \n) + 1` — DERIVED, not read from a field (so `blob_header_len` need not be passed). Any non-space non-`\n` byte in the pad zone, or no `\n` before the buffer end → `CORRUPTED_DATA` ("no smuggling"). `payloadOffset = header_len`.

## The local `ref` escaper {#ref-escaper}

`writeEnvelopeRefField(out, budget, raw_ref)` is LOCAL to `CasBlobEnvelopeFormat.cpp`. It escapes only `"`, `\`, and control chars (`< 0x20`, as `\uXXXX`), passing `/` and all other bytes verbatim, and truncates by measured escaped length to `budget`. Rationale: (a) makes the budget arithmetic and golden deterministic **independent of `FormatSettings::JSON::escape_forward_slashes`** (so no dependency on the phase-2 pin — phase 7 stays mainline-independent); (b) keeps the `ref` human-readable (`/` unescaped) which is the whole point of the header (`head -c 256 blob | jq .ref`). The reader side is `JsonObjectReader::readString` (standard un-escaping — it already handles `\/` if some other writer produced it, and plain `/`).

## Field mapping and dropped fields {#fields}

| v3 key | From `EnvelopeHeader` | Type | Note |
|---|---|---|---|
| `type` | (magic `CABL`) | `cas_blob` | the magic becomes the type string |
| `v` | `compatibility_version` | number | read gate (`checkCompatibility`) |
| `tag` | `incarnation_tag` | 32 hex | the exact-token delete primitive (W-FRESH-TAG) |
| `bld` | `build_id` | 32 hex | newborn-debris watermark attribution, B170 |
| `ts` | `provenance.created_at_ms` | number (unix ms) | |
| `by` | `provenance.creator_server_id` | 32 hex | |
| `op` | `provenance.op` | word | `insert`/`merge`/`mutation`/`attach`/`repack`/`other` |
| `ch` | `provenance.ch_version` | number | set to the real `VERSION_INTEGER` (was hardcoded `0` in `CasBuild`; diagnostic-only, no decision reads it) |
| `ref` | `intended_ref` | string (truncated) | diagnostic; last field; local-escaper-truncated |

**Dropped** (each with its reason, spec §blob-envelope): `hash_algo` (identity lives in the key + manifest ref); `domain_id` (written, never validated — YAGNI); `header_hash` (no consumer — `tag` compares are storage-vs-storage, provider checksums cover S3; **CityHash64 leaves the envelope**); `writer_version` (forensics are `ch` + `bld`). The TLV critical-flag mechanism becomes the `!`-key convention; `provenance`/`intended_ref` TLVs become inline keys.

## Interfaces consumed {#interfaces}

From `Core/Formats/CasTextFormat.h`: `writeKey`, `writeStringValue`, `writeHex128Value`, `closeObject`, `class JsonObjectReader` (`nextKey`/`readString`/`readHex128`/`readU64Number`/`skipUnknown`). From `Core/Formats/CasFormat.h`: `FormatId::Blob`, `currentCompatibilityVersion`, `checkCompatibility`, `traitsFor`. From `IO/WriteHelpers.h`: `writeChar`, `writeIntText`. NOT `writeHeaderLine`/`writeTrailerLine`/`readLine` (the envelope has its own header shape — object-then-pad, not a `\n`-terminated line at `json_len`).

---

### Task 1: `Formats/CasBlobEnvelopeFormat` — struct + text codec + exact-budget/pad tests {#task1}

**Files:**
- Create: `Core/Formats/CasBlobEnvelopeFormat.h`, `Core/Formats/CasBlobEnvelopeFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_blob_envelope_format.cpp` (new)

The wire-vocabulary types `ObjectKind`, `ProvenanceOp`, `Provenance` move from `Core/CasEnvelope.h` into `Core/Formats/CasBlobEnvelopeFormat.h` (they are the envelope's protocol vocabulary). The slim v3 `EnvelopeHeader` moves too. `Core/CasEnvelope.{h,cpp}` are DELETED (their entire content is the binary codec, now replaced). Consumers (`CasBuild`, `CasInspect`, tests) that used `ObjectKind`/`ProvenanceOp` via `CasEnvelope.h` are rewired to the new header (Task 2 + an include sweep).

**Interfaces:**
- Produces: `enum class ObjectKind`, `enum class ProvenanceOp`, `struct Provenance`, slim `struct EnvelopeHeader`, `String encodeEnvelopeHeader(EnvelopeHeader &, uint32_t blob_header_len)`, `EnvelopeHeader decodeEnvelopeHeader(std::string_view head_bytes, uint64_t object_size, ObjectKind expected_kind)`, `inline uint64_t payloadOffset(const EnvelopeHeader &)`.

  NOTE the encode signature GAINS `uint32_t blob_header_len` (the pad target `L`) — the current code carried it as `header.pad_to_header_len`; making it an explicit parameter is cleaner and matches the "header built to a pool-fixed length" contract. Document this as a deviation.

Slim `EnvelopeHeader`:

```cpp
struct EnvelopeHeader
{
    ObjectKind kind = ObjectKind::Blob;
    uint32_t compatibility_version = 0;   /// set by decode (the header `v`); encode uses currentCompatibilityVersion()
    UInt128 incarnation_tag{};             /// tag
    UInt128 build_id{};                    /// bld
    std::optional<Provenance> provenance;  /// ts/by/op/ch
    std::optional<String> intended_ref;    /// ref (truncated on encode)
    uint32_t header_len = 0;               /// set by encode/decode = blob_header_len (payload offset)
    /// Test-only knob to drive the critical-`!`-key fail-closed path.
    bool emit_unknown_critical_key = false;
};
```

- [ ] **Step 1: Failing tests** — `src/Disks/tests/gtest_cas_blob_envelope_format.cpp`. This is where the exact byte-count budget + pad-verify RED tests live:

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobEnvelopeFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

using namespace DB::Cas;
namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; extern const int UNKNOWN_FORMAT_VERSION; }

namespace
{
EnvelopeHeader sampleHeader(const String & ref)
{
    EnvelopeHeader h;
    h.kind = ObjectKind::Blob;
    h.incarnation_tag = hexToU128("0102030405060708090a0b0c0d0e0f10");
    h.build_id = hexToU128("1112131415161718191a1b1c1d1e1f20");
    h.provenance = Provenance{1752537600123ULL, hexToU128("2122232425262728292a2b2c2d2e2f30"), 26006001u, ProvenanceOp::Merge};
    h.intended_ref = ref;
    return h;
}
constexpr uint32_t L = 256;
}

TEST(CasBlobEnvelopeFormat, FixedLengthAndPadZone)
{
    EnvelopeHeader h = sampleHeader("t-abc/all_1_2_0");
    const String head = encodeEnvelopeHeader(h, L);
    ASSERT_EQ(head.size(), L);                       /// exactly blob_header_len
    EXPECT_EQ(head[L - 1], '\n');                     /// terminator at byte 255
    const String json = "{\"type\":\"cas_blob\",\"v\":3,\"tag\":\"0102030405060708090a0b0c0d0e0f10\","
                        "\"bld\":\"1112131415161718191a1b1c1d1e1f20\",\"ts\":1752537600123,"
                        "\"by\":\"2122232425262728292a2b2c2d2e2f30\",\"op\":\"merge\",\"ch\":26006001,"
                        "\"ref\":\"t-abc/all_1_2_0\"}";
    ASSERT_LT(json.size(), L);
    EXPECT_EQ(head.substr(0, json.size()), json);                        /// '/' UNescaped (local escaper)
    EXPECT_EQ(head.substr(json.size(), (L - 1) - json.size()), String((L - 1) - json.size(), ' ')); /// pad = spaces
    /// round-trip
    const EnvelopeHeader back = decodeEnvelopeHeader(head, head.size(), ObjectKind::Blob);
    EXPECT_EQ(back.incarnation_tag, h.incarnation_tag);
    EXPECT_EQ(back.build_id, h.build_id);
    ASSERT_TRUE(back.provenance.has_value());
    EXPECT_EQ(back.provenance->created_at_ms, 1752537600123ULL);
    EXPECT_EQ(back.provenance->ch_version, 26006001u);
    EXPECT_EQ(back.provenance->op, ProvenanceOp::Merge);
    ASSERT_TRUE(back.intended_ref.has_value());
    EXPECT_EQ(*back.intended_ref, "t-abc/all_1_2_0");
    EXPECT_EQ(back.header_len, L);
    EXPECT_EQ(payloadOffset(back), L);
}

TEST(CasBlobEnvelopeFormat, RefTruncatedToExactBudget)
{
    /// A 200-char ref cannot fit; it is truncated so the header is EXACTLY 256 bytes and the pad holds.
    EnvelopeHeader h = sampleHeader(String(200, 'a'));
    const String head = encodeEnvelopeHeader(h, L);
    ASSERT_EQ(head.size(), L);
    EXPECT_EQ(head[L - 1], '\n');
    const EnvelopeHeader back = decodeEnvelopeHeader(head, head.size(), ObjectKind::Blob);
    ASSERT_TRUE(back.intended_ref.has_value());
    /// Budget is deterministic: json_len == 255 (pad zone empty) at the truncation boundary, so the
    /// decoded ref is exactly (255 - prefix - 2) 'a's. Compute the same way the encoder does:
    EnvelopeHeader probe = sampleHeader("");
    const String empty_ref_head = encodeEnvelopeHeader(probe, L);
    // json with empty ref, minus the two chars of `""`, gives the prefix+suffix length.
    const size_t json_len_empty = std::string_view(empty_ref_head).find('\n') == String::npos
        ? 0 : empty_ref_head.find_last_not_of(' ', empty_ref_head.find('\n') - 1) + 1;
    const size_t budget = (L - 1) - (json_len_empty - 0);   // 'a' is 1 escaped byte each
    EXPECT_EQ(back.intended_ref->size(), budget) << "ref truncated to the exact byte budget";
    for (char c : *back.intended_ref) EXPECT_EQ(c, 'a');
}

TEST(CasBlobEnvelopeFormat, PadZoneSmugglingFailsClosed)
{
    EnvelopeHeader h = sampleHeader("r");
    String head = encodeEnvelopeHeader(h, L);
    const size_t json_len = head.find_last_not_of(' ', (L - 1) - 1) + 1; /// first pad byte index = json_len
    ASSERT_LT(json_len, L - 1);
    /// A non-space byte smuggled into the pad zone -> CORRUPTED_DATA.
    String smuggled = head;
    smuggled[json_len + 1] = 'x';
    EXPECT_THROW(decodeEnvelopeHeader(smuggled, smuggled.size(), ObjectKind::Blob), DB::Exception);
    /// Byte 255 not '\n' -> CORRUPTED_DATA.
    String no_nl = head;
    no_nl[L - 1] = ' ';
    EXPECT_THROW(decodeEnvelopeHeader(no_nl, no_nl.size(), ObjectKind::Blob), DB::Exception);
}

TEST(CasBlobEnvelopeFormat, GatesAndCriticalKey)
{
    /// wrong type -> CORRUPTED_DATA; future v -> UNKNOWN_FORMAT_VERSION.
    EnvelopeHeader h = sampleHeader("r");
    String head = encodeEnvelopeHeader(h, L);
    String wrong_type = head;
    wrong_type.replace(wrong_type.find("cas_blob"), 8, "cas_xxxx");
    EXPECT_THROW(decodeEnvelopeHeader(wrong_type, wrong_type.size(), ObjectKind::Blob), DB::Exception);
    String future = head;
    future.replace(future.find("\"v\":3"), 5, "\"v\":4");
    EXPECT_THROW(decodeEnvelopeHeader(future, future.size(), ObjectKind::Blob), DB::Exception);
    /// an unknown `!`-critical key fails closed.
    EnvelopeHeader hc = sampleHeader("r");
    hc.emit_unknown_critical_key = true;
    const String crit = encodeEnvelopeHeader(hc, L);
    EXPECT_THROW(decodeEnvelopeHeader(crit, crit.size(), ObjectKind::Blob), DB::Exception);
}

TEST(CasBlobEnvelopeFormat, RefEscaperAlphabetPinned)
{
    /// Pins the LOCAL escaper's alphabet (§ref-escaper): " and \ escape, control chars -> \uXXXX,
    /// '/' passes VERBATIM. Goes RED if anyone "unifies" this with writeStringValue/FormatSettings —
    /// the 256-byte budget arithmetic depends on this alphabet being codec-owned and frozen.
    EnvelopeHeader h = sampleHeader(String("a/b\"c\\d") + '\x01' + "e");
    const String head = encodeEnvelopeHeader(h, L);
    const String expected_ref_json = "\"a/b\\\"c\\\\d\\u0001e\"";
    EXPECT_NE(head.find("\"ref\":" + expected_ref_json), String::npos)
        << "escaper alphabet drifted: '/' must be verbatim, quote/backslash escaped, control -> \\uXXXX";
}

TEST(CasFormatBattery, BlobEnvelope)
{
    /// The golden is CONSTRUCTED from the hand-pinned json literal (same one FixedLengthAndPadZone
    /// asserts) + the derived pad — NOT self-computed via encodeEnvelopeHeader, which would compare
    /// the encoder to itself and pin nothing.
    const String json = "{\"type\":\"cas_blob\",\"v\":3,\"tag\":\"0102030405060708090a0b0c0d0e0f10\","
                        "\"bld\":\"1112131415161718191a1b1c1d1e1f20\",\"ts\":1752537600123,"
                        "\"by\":\"2122232425262728292a2b2c2d2e2f30\",\"op\":\"merge\",\"ch\":26006001,"
                        "\"ref\":\"t-abc/all_1_2_0\"}";
    const String golden = json + String((L - 1) - json.size(), ' ') + '\n';
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::Blob,
        .encode = [&] { EnvelopeHeader e = sampleHeader("t-abc/all_1_2_0"); return sealObject(FormatId::Blob, encodeEnvelopeHeader(e, L)); },
        .decode = [](std::string_view s) { decodeEnvelopeHeader(String(openObject(FormatId::Blob, s)), s.size(), ObjectKind::Blob); },
        .golden = golden});
}
```

Note (implementer): the battery header line for `cas_blob` is the 256-byte block; `runFormatBattery` asserts `text.starts_with("{\"type\":\"")` (holds) and, for the truncation-at-line-boundary sweep, the only `\n` is at byte 255 — the battery's cut-at-`\n` and cut-inside-line-1 loops both exercise the object. Confirm the battery's wrong-type substitution finds `cas_blob`; if the generic battery's "another registered type" swap picks a type longer/shorter than `cas_blob` it changes `json_len` — pad-derivation still finds `\n`, so decode fails on the type check first (`CORRUPTED_DATA`), which is what the battery expects.

- [ ] **Step 2: Verify compile failure** (`CasBlobEnvelopeFormat.h` missing).

- [ ] **Step 3: Implement** — `Core/Formats/CasBlobEnvelopeFormat.h` (move `ObjectKind`/`ProvenanceOp`/`Provenance` here verbatim with their doc comments; add the slim `EnvelopeHeader` + the three function decls + `payloadOffset`). `Core/Formats/CasBlobEnvelopeFormat.cpp`: the `opToWord`/`opFromWord` local word maps (all `ProvenanceOp` enumerators, trailing throw, no default); `writeEnvelopeRefField` (the local escaper + measured truncation, §ref-escaper/§budget); `encodeEnvelopeHeader` (build PREFIX with `writeKey`/`writeStringValue`/`writeHex128Value`/`writeIntText`, then `!`-key if `emit_unknown_critical_key`, then `writeEnvelopeRefField`, then `"}`, then the space pad + `\n`; set `header.header_len = blob_header_len`); `decodeEnvelopeHeader` (`JsonObjectReader` over `head_bytes` from byte 0, read `type`==`cas_blob` + `v` gate via `checkCompatibility`, then `tag`/`bld`/`ts`/`by`/`op`/`ch`/`ref` — `skipUnknown` fails closed on `!`-keys — then the pad-verify-to-`\n` deriving `header_len`). Full code is a direct transcription of §budget + §fields; the JIT executor writes it against these exact rules. `payloadOffset` returns `header.header_len`.

- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasBlobEnvelopeFormat*:CasFormatBattery.BlobEnvelope'` green.

- [ ] **Step 5: Commit** — `Formats/CasBlobEnvelopeFormat.*` + the new gtest. Message `cas: formats v3 phase 7 — cas_blob 256-byte JSON envelope header` + trailer.

---

### Task 2: Rewire `CasBuild` + `CasInspect`, delete `CasEnvelope`, re-pin goldens {#task2}

**Files:**
- Delete: `Core/CasEnvelope.{h,cpp}` (content fully replaced by Task 1's Formats file)
- Modify: `Core/CasBuild.cpp` (header construction), `Core/CasInspect.cpp` (`renderEnvelopeHeader`), include-rewrite every `CasEnvelope.h` includer → `Formats/CasBlobEnvelopeFormat.h`
- Modify/replace tests: `src/Disks/tests/gtest_cas_envelope.cpp`, `src/Disks/tests/gtest_cas_codecs.cpp` (the `CasByteOrderGolden.EnvelopeLittleEndian` binary golden)

- [ ] **Step 1: Include-rewrite + delete**

```bash
cd <worktree>
D=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
grep -rl 'ContentAddressed/Core/CasEnvelope\.h' src/ | grep -v 'Core/CasEnvelope\.cpp' | xargs -r sed -i \
  's|ContentAddressed/Core/CasEnvelope\.h|ContentAddressed/Core/Formats/CasBlobEnvelopeFormat.h|g'
git rm $D/Core/CasEnvelope.h $D/Core/CasEnvelope.cpp
```

- [ ] **Step 2: `CasBuild.cpp` header construction** (the `buildHeader` lambda, ~lines 379-405). Remove `header.hash_algo = …` and `header.domain_id = …` (dropped fields). Set `ch_version` to the real version: `header.provenance = Provenance{nowMs(), cfg.server_id, VERSION_INTEGER, info.op};` (add `#include <Common/config_version.h>`). Replace `header.pad_to_header_len = blob_header_len;` + the `try/encode/catch BAD_ARGUMENTS → drop intended_ref → re-encode` with a single call `return encodeEnvelopeHeader(header, static_cast<uint32_t>(meta.blob_header_len));` — the v3 codec truncates `ref` internally, so the drop-and-retry is dead (document the removal). Result:

```cpp
    auto buildHeader = [&]() -> String
    {
        EnvelopeHeader header;
        header.kind = kind;
        header.incarnation_tag = mintU128();
        header.build_id = build_id;
        header.provenance = Provenance{nowMs(), cfg.server_id, VERSION_INTEGER, info.op};
        if (kind == ObjectKind::Blob)
            header.intended_ref = info.intended_ref;
        return encodeEnvelopeHeader(header, static_cast<uint32_t>(meta.blob_header_len));
    };
```

- [ ] **Step 3: `CasInspect.cpp` `renderEnvelopeHeader`** — drop the `.add("hash_algo", …)`, `.add("writer_version", …)`, `.add("domain_id", …)` lines (those fields no longer exist on the slim struct); keep `kind`, `compatibility_version` (render as `v`), `incarnation_tag`, `build_id`, `header_len`, `provenance`, `intended_ref`. (The introspection JSON is `ca-inspect`'s own renderer, independent of the on-disk format; it just needs to compile against the slim struct.)

- [ ] **Step 4: Re-pin the golden tests.** In `gtest_cas_envelope.cpp`, the round-trip / magic / future-version / incarnation-zone tests migrate to the v3 codec (most assert via `encode`/`decode`, but the binary-specific ones — `BadMagicThrows` on raw bytes, the 70-byte-layout `CasByteOrderGolden.EnvelopeLittleEndian` in `gtest_cas_codecs.cpp` — are DELETED; their intent is covered by Task 1's `GatesAndCriticalKey` + `FixedLengthAndPadZone` + the battery golden). Keep `IncarnationZoneDoesNotAffectPayload` re-pointed (the `tag` still varies per incarnation and must not perturb the payload). Add a note that the payload-offset invariant (`payloadOffset == blob_header_len == 256`) is asserted by `FixedLengthAndPadZone`.

- [ ] **Step 5: Grep gate + build + full CAS slice**

```bash
cd <worktree>
D=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
grep -rn 'CasEnvelope\.h\|header_hash\|CityHash64\|computeHeaderHash\|domain_id\|pad_to_header_len' $D/Core/ | grep -viE 'CasBlobInDegree|GcLease|GcHeartbeat'  ; echo "EXPECT: none envelope-related (CityHash64 left the envelope)"
flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p7t2.log 2>&1; echo "NINJA_EXIT=$?"
build_debug/src/unit_tests_dbms --gtest_filter='CasEnvelope*:CasBlobEnvelopeFormat*:CasFormatBattery.BlobEnvelope:CasBuild*:CasInspect*' 2>&1 | tail -6
```
Expected: `NINJA_EXIT=0`; the envelope grep shows no `header_hash`/`CityHash64`/`domain_id`/`pad_to_header_len` remnants (CityHash64 is gone from the envelope; `city.h` include removed); all filtered tests green — in particular the `CasBuild` write path and any e2e that reads a blob header back. Subagent-analyze the log.

- [ ] **Step 6: Commit** — `Core/CasEnvelope.{h,cpp}` (deleted), `CasBuild.cpp`, `CasInspect.cpp`, rewired includers, the two edited test files. Message `cas: formats v3 phase 7 — rewire writer/inspect to the JSON envelope; CityHash64 leaves the envelope` + trailer.

---

## Phase-7 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- `cas_blob` is a fixed 256-byte JSON header (object + space pad + `\n` at byte 255); payload at the constant `blob_header_len`; `payloadOffset` unchanged.
- `ObjectKind`/`ProvenanceOp`/`Provenance`/`EnvelopeHeader` + codec live in `Core/Formats/CasBlobEnvelopeFormat`; `Core/CasEnvelope.{h,cpp}` deleted.
- Dropped: `hash_algo`, `domain_id`, `header_hash`, `writer_version`; **CityHash64 no longer appears in the envelope** (`city.h` include gone).
- Exact-budget truncation + pad-zone verification are covered by RED-able tests with exact byte counts (`FixedLengthAndPadZone`, `RefTruncatedToExactBudget`, `PadZoneSmugglingFailsClosed`); critical `!`-key fails closed (`GatesAndCriticalKey`).
- One `FormatBatteryCase.BlobEnvelope` row; the binary `CasByteOrderGolden.EnvelopeLittleEndian` golden deleted and replaced by the v3 golden.
- Header still built before payload (staging); `CasBuild` no longer needs the drop-and-retry.
- No dependency on the phase-2 `escape_forward_slashes` pin (local `ref` escaper); phase 7's patch integrates independently, before or after phase 2/4.

## Open decision for the gate {#open-decision}

**The local `ref` escaper (§ref-escaper)** is the one judgment call: it keeps phase 7 mainline-independent and the budget deterministic, at the cost of a small self-contained escaper instead of reusing `writeStringValue`. The alternative — depend on the phase-2 pin — would couple phase 7 to phase 2's integration order, which contradicts the "independent, before-or-after" requirement.

**GATE RESOLUTION (approved): the local escaper.** The deeper reason beyond integration order: the envelope's 256-byte budget arithmetic must be OWNED by this codec — even the phase-2 pinned `jsonWriteSettings` is another translation unit's static that could legitimately evolve for the control-plane formats without anyone realizing it changes blob-header byte budgets. Two conditions attach: (1) the escaper's alphabet is RED-guarded by `CasBlobEnvelopeFormat.RefEscaperAlphabetPinned` (Task 1); (2) `writeEnvelopeRefField`'s comment must state it is deliberately NOT `writeStringValue` and must not be "unified" with it — the alphabet is frozen because budget arithmetic and stored blob bytes depend on it.

## Draft packaging note {#draft-note}

Branch `tmp/worktrees/draft-codecs7` from mainline HEAD (NOT on phase-2/4). Implement Tasks 1-2. No-build contract. `DRAFT.patch` = `git add -A && git diff --cached -M --binary` (exclude `DRAFT.md`/`DRAFT.patch`); document in DRAFT.md that this patch is independent of phase 2/4 and can apply before or after them (it touches only the envelope + its consumers, disjoint from the control-plane codecs).
