# CAS Mutable JSON Objects → Protobuf; Delete the JSON Codec Family — Implementation Plan (Plan 3c)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** Realize the "two encodings / abandon JSON" decision: convert the four remaining JSON objects —
**`pool-meta`, `watermark`, `gc-state`, `retired-set`** — to protobuf using the same framing-header
pattern proven in 3a, then **delete the entire JSON codec family** (`CasCodecUtil.h`'s `JsonObjectWriter`,
`require*`/`requireU64`/`checkNoUnknownKeys`/`parseJsonDocument`/`decodeJsonGuarded`), the monotone
`checkVersion`, and the now-dead `tolerateUnknownKeys` (Plan 1). After this, CA has exactly two
encodings: canonical binary (hashed: blob/tree) and protobuf (mutable), per the spec.

**Architecture:** Each object gets a protobuf message + a 4-byte magic, and its `encodeX`/`decodeX`
switches to: `writeFramingHeader(out, MAGIC_X, currentWriterVersion(FormatId::X))` + serialized message
(encode); `readFramingHeader(in, MAGIC_X, "x")` (gates `min_reader`) + `ParseFromArray` (decode) — exactly
the 3a shape. The C++ structs are unchanged; only the codec bodies change. Magics: `pool-meta`=`CAPM`,
`watermark`=`CAWM`, `gc-state`=`CAGT`, `retired-set`=`CART`. `FormatId::{PoolMeta,Watermark,GcState,
RetiredSet}` already exist (Plan 1). Pre-release — no migration. Proto messages added to the existing
`cas_root_shard.proto` for now (Plan 3d consolidates/renames to `cas_format.proto`). gc-snap is NOT
touched (already binary; B176 deferred).

**Tech Stack:** C++ (ClickHouse), protobuf, `CasFormat`, gtest, ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only).

**Scope guards:** the four JSON codecs + their proto messages + the `CasCodecUtil`/`CasFormat` deletions
+ tests. Does NOT touch gc-snap, the tree/envelope/manifest codecs (done), the part-writer, or the
proto rename (3d). Out of scope: B92, Part IV, B164b.

**Build & test:**
- Build: `cd build && cmake . && ninja unit_tests_dbms > cas_3c_build.log 2>&1` — no `-j`/`nproc` (proto regen needs the cmake re-run once).
- Per-object tests: `--gtest_filter='CasPoolMeta*:CasWatermark*:CasGc*'`.
- Full sweep (final): `--gtest_filter='Cas*:Ca*'`. Only tolerated red: baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

**General procedure for EACH object task (1-4):** read the object's current codec (`encodeX`/`decodeX`)
+ its JSON shape to enumerate its fields; add a matching protobuf message to `cas_root_shard.proto`
(one field per struct field, fresh numbers from 1, `uint64`/`string`/`bytes`/`repeated` as fits; raw
16-byte big-endian `bytes` for any UInt128, reusing the `u128ToBytesBE`/`u128FromBytesBE` helpers);
rewrite `encodeX`/`decodeX` to the framing-header + protobuf pattern; keep the SAME C++ struct and the
SAME validation invariants the JSON decode enforced (e.g. `PoolMeta::decode` validates root_shards≥1,
blob_header_len 8-aligned ∈[96,16Ki] — keep those as post-parse checks throwing the same codes); update
its golden/round-trip test to round-trip through the new codec + add a future-`min_reader` fail-closed
test. Commit per object (message: `CA: <object> JSON -> protobuf (framing header)`).

---

### Task 1: `pool-meta` → protobuf
**Files:** `Core/Proto/cas_root_shard.proto`, `CasPoolMeta.cpp` (+ its gtest). Also add the deferred 3a nit here: `reserved "codec_version";` line next to `reserved 1;` in `RootShardManifest`.
- Fields (from `CasPoolMeta.h`): `pool_id` (UInt128 → `bytes` BE), `root_shards` (uint64), `blob_header_len` (uint64). Message `PoolMetaProto`. Magic `CAPM`, `FormatId::PoolMeta`. Keep `createOrValidate`/`decodePoolMeta`'s constant-invariant checks (root_shards≥1; blob_header_len 8-aligned ∈ [96,16384]) as post-parse validation with the same error codes.
- TDD: failing round-trip test → proto+codec → pass → commit.

### Task 2: `watermark` → protobuf
**Files:** `Core/Proto/cas_root_shard.proto`, `CasWatermark.cpp` (+ gtest). Read `CasWatermark.h` for fields (`ServerWatermark{server_id, epoch, min_active, seq}` per the codebase notes; confirm against the header). Message `WatermarkProto`, magic `CAWM`, `FormatId::Watermark`. Preserve any conditional/optional field semantics (e.g. `min_active` presence) the JSON had — use proto3 optional or a has-bit if the JSON distinguished absent.

### Task 3: `gc-state` → protobuf
**Files:** `Core/Proto/cas_root_shard.proto`, the gc-state codec (find it: `grep -rln "gc/state\|GcState\|encodeGcState\|cas_gc_state" Core/`; likely `CasGcFormats.cpp`). Enumerate its JSON fields; message `GcStateProto`, magic `CAGT`, `FormatId::GcState`. Keep any ordering/version semantics.

### Task 4: `retired-set` → protobuf
**Files:** `Core/Proto/cas_root_shard.proto`, the retired-set codec (find it similarly). It is a SET that can grow — model as a `repeated` entry message; preserve deterministic order (sort the entries before serializing, as the JSON encode did via ordered iteration). Message `RetiredSetProto`, magic `CART`, `FormatId::RetiredSet`.

### Task 5: Delete the JSON codec family + monotone `checkVersion` + `tolerateUnknownKeys`
**Files:** `CasCodecUtil.h`, `CasFormat.h`, `CasFormat.cpp`, `gtest_cas_format.cpp`, `gtest_cas_codecs.cpp`/`gtest_cas_gc_formats.cpp` (the JSON-golden tests).
- [ ] **Step 1:** Confirm no remaining callers of the JSON helpers: `grep -rn "JsonObjectWriter\|parseJsonDocument\|requireObject\|requireString\|requireU64\|requireHash\|requireStringMap\|checkNoUnknownKeys\|decodeJsonGuarded\|requireArray\|requireObjectAt\|writeJsonString\|writeJsonKey\|checkVersion\|tolerateUnknownKeys" src/Disks/ | grep -v tests`. Expected after Tasks 1-4: nothing in non-test code (every JSON object now protobuf; `checkVersion` had only the JSON `parseJsonDocument` caller + any binary codec callers already migrated to `gateOnRead` in 2b/3a — verify the manifest/envelope/tree no longer call it).
- [ ] **Step 2:** Delete from `CasCodecUtil.h`: `writeJsonString`, `writeJsonKey`, `JsonObjectWriter`, `decodeJsonGuarded`, `requireObject` (both overloads), `requireKey`, `requireString`, `requireU64Var`, `requireU64`, `requireArray`, `requireObjectAt`, `requireStringMap`, `requireHash`, `checkNoUnknownKeys`, `parseJsonDocument`, and the monotone `checkVersion`. KEEP the binary helpers (`writeU128LE`/`readU128LE`/`u128ToBytesBE`/`u128FromBytesBE`/`readFixedBytes`/`decodeGuarded`) and the Poco/JSON includes only if still needed (drop the now-unused `<Poco/JSON/...>` includes). Update the file's top doc comment (it currently describes the binary+JSON split → now binary + "protobuf elsewhere").
- [ ] **Step 3:** Delete `tolerateUnknownKeys` from `CasFormat.h`/`CasFormat.cpp` and its test `TolerateUnknownKeysOnlyForFutureWriter` in `gtest_cas_format.cpp`.
- [ ] **Step 4:** Delete the JSON-golden tests that pinned the old JSON bytes (e.g. `CasJsonGolden` in `gtest_cas_gc_formats.cpp` / any `parseJsonDocument` tests) — the formats they pinned no longer exist. Do NOT delete tests that now round-trip through the protobuf codecs.
- [ ] **Step 5:** Build clean; full sweep green (baseline only). Commit: `CA: delete the JSON codec family + monotone checkVersion + tolerateUnknownKeys (two encodings)`.

---

### Task 6: Full regression sweep
- [ ] Build + `--gtest_filter='Cas*:Ca*'`; confirm only the baseline red. Confirm the grep from Task 5 Step 1 returns clean (no JSON-helper or checkVersion references remain outside tests). (no commit)

---

## Self-Review (inline)
**Spec coverage (3c slice):** pool-meta/watermark/gc-state/retired-set → protobuf (framing header) → Tasks 1-4; delete JSON family + checkVersion + tolerateUnknownKeys → Task 5; achieves "two encodings" (Part I) + the JSON-abandon note + Part V's CasCodecUtil deletion. Uses Plan 1 framing + FormatId. NOT here: gc-snap (deferred), proto rename (3d). ✓
**Placeholder scan:** the per-object procedure is concrete (read the codec → message → framing pattern → keep invariants → test); the deletion task lists exact symbols + a grep gate. The per-object field lists for watermark/gc-state/retired-set are "read the header/codec to enumerate" — a genuine adapt-to-code step (the structs are small/simple), with the message-shaping rules (UInt128→bytes BE, sets→sorted repeated, keep invariant checks) fixed. ✓
**Type consistency:** framing helpers + `FormatId::{PoolMeta,Watermark,GcState,RetiredSet}` (Plan 1); magics 4 bytes; UInt128 via the existing BE helpers. ✓
**Risk:** keep each decode's invariant checks (pool-meta constants; journal/version ordering) — protobuf parse does not enforce them, so they must remain as explicit post-parse throws with the same error codes. The reviews + soak are the backstop.
