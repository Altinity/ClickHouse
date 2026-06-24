# CAS Manifest Framing Header + `published_at_ms` — Implementation Plan (Plan 3a)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** Move the root-shard manifest onto the `CasFormat` framing header (the `[magic][writer:u16][min_reader:u16]` prefix from Plan 1) in place of its in-message `codec_version`, and replace the stringly `.ca_mtime` magic key in `RefPayload.mutable_files` with a typed `RefPayload.published_at_ms` protobuf field. First step of consolidating every mutable object onto one protobuf scheme.

**Architecture:** The manifest is already protobuf (`cas_root_shard.proto`, `RootShardManifest`). `encodeRootShard` prepends `writeFramingHeader(out, MANIFEST_MAGIC, currentWriterVersion(FormatId::Manifest))` then the serialized message; `decodeRootShard` calls `readFramingHeader(in, MANIFEST_MAGIC, "root shard")` (which `gateOnRead`s `min_reader`) then parses the remainder. The in-message `codec_version` field is removed (`reserved 1`). A typed `published_at_ms` (proto field) carries the publish wall-clock that today lives as `mutable_files[".ca_mtime"]`; the C++ `RefPayload` struct gains a `uint64_t published_at_ms`, and the read/write sites (`ContentAddressedMetadataStorage.cpp:661,1030`, `ContentAddressedTransaction.cpp:157,280`) use it instead of the string key. Pre-release — no migration. The proto file/package is NOT renamed here (that is Plan 3d, after all messages exist).

**Tech Stack:** C++ (ClickHouse), protobuf (proto3), `CasFormat` framing helpers, gtest, ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only, no amend/rebase).

**Scope guards:** changes the manifest codec (`CasRootShardCodec.{h,cpp}`), the proto (`cas_root_shard.proto`), the two getLastModified/publish sites, and tests. Does NOT touch gc-snap (3b), the JSON objects (3c), the proto rename (3d), the tree/envelope/part-writer (Phase 1, done). Out of scope: B92 `tree_size` correctness (separate), Part IV, B164b.

**Build & test:**
- Build: `cd build && ninja unit_tests_dbms > cas_3a_build.log 2>&1` — no `-j`/`nproc`. (Proto change triggers a regen; if cmake doesn't auto-rerun, `cd build && cmake .` then rebuild.)
- Tests: `build/src/unit_tests_dbms --gtest_filter='CasRootShard*:CasManifest*' > build/cas_3a_test.log 2>&1`.
- Full sweep (final): `--gtest_filter='Cas*:Ca*'`. Only tolerated red: baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

---

### Task 1: Proto — drop `codec_version`, add `published_at_ms`

**Files:** `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_root_shard.proto`

- [ ] **Step 1: Edit the proto**

In `message RefPayload`, add the typed field (use a fresh number — `4` is already `reserved`, so use `5`):

```proto
message RefPayload {
  bytes tree_id = 1;                      // raw 16 bytes (UInt128, big-endian)
  uint64 tree_size = 2;
  map<string, string> mutable_files = 3;
  reserved 4;                             // was `closure` (B199-S2); moved to JournalRecord.closure
  uint64 published_at_ms = 5;             // publish wall-clock (epoch ms) for getLastModified; replaces the .ca_mtime mutable_files key
}
```

In `message RootShardManifest`, remove `codec_version` and reserve its number (the version now lives in the framing header, not in-message):

```proto
message RootShardManifest {
  reserved 1;                         // was codec_version; version is now in the CasFormat framing header
  uint64 shard_version = 2;
  uint64 fence_round = 3;
  map<string, RefPayload> refs = 4;
  repeated JournalRecord journal = 5; // insertion order preserved
}
```

Update the file's top comment: the `codec_version`/`NOT_IMPLEMENTED` schema-evolution paragraph is replaced by "version + fail-closed gating now live in the CasFormat framing header (`[magic][writer:u16][min_reader:u16]`); see `CasFormat.h`." Keep the additive/breaking and the `reserved`/never-reuse-numbers guidance.

- [ ] **Step 2: Build to regenerate** — `cd build && cmake . && ninja unit_tests_dbms > cas_3a_build.log 2>&1; tail -20 cas_3a_build.log`. Expected: FAILS in `CasRootShardCodec.cpp` (it still sets/reads `codec_version` and won't have `published_at_ms` wired). Proceed to Task 2.

---

### Task 2: Manifest codec — framing header + `published_at_ms`

**Files:** `CasRootShardCodec.h`, `CasRootShardCodec.cpp`

- [ ] **Step 1: C++ `RefPayload` struct gains the field**

In `CasRootShardCodec.h`, add to the `RefPayload` struct:

```cpp
    uint64_t published_at_ms = 0;   /// publish wall-clock (epoch ms); 0 = unset
```

- [ ] **Step 2: Encode/decode the framing header + the field**

In `CasRootShardCodec.cpp`:
- Add `#include <Disks/.../Core/CasFormat.h>`.
- Define `constexpr std::string_view MANIFEST_MAGIC = "CARS";` (CA Root Shard) near the top.
- In `encodeRootShard`: remove `msg.set_codec_version(CODEC_VERSION);`. Map `published_at_ms`: `p.set_published_at_ms(payload.published_at_ms);` where each RefPayload is encoded. Then, instead of returning the bare serialized string, prepend the framing header:
  ```cpp
  String body;
  if (!msg.SerializeToString(&body))
      throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS root shard: protobuf serialization failed");
  WriteBufferFromOwnString out;
  Cas::writeFramingHeader(out, MANIFEST_MAGIC, Cas::currentWriterVersion(Cas::FormatId::Manifest));
  writeString(body, out);
  return std::move(out.str());
  ```
- In `decodeRootShard`: read+gate the framing header first, then parse the remainder:
  ```cpp
  ReadBufferFromMemory in(data.data(), data.size());
  Cas::readFramingHeader(in, MANIFEST_MAGIC, "root shard");   // validates magic + gateOnRead(min_reader)
  const std::string_view body = data.substr(in.count());
  Cas::Proto::RootShardManifest msg;
  if (!msg.ParseFromArray(body.data(), static_cast<int>(body.size())))
      throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf parse failed");
  ```
  Remove the `codec_version() == 0` and `checkVersion(CODEC_VERSION, ...)` checks (the framing header replaces them). Map `payload.published_at_ms = p.published_at_ms();` when decoding each RefPayload. Delete the now-unused `constexpr uint32_t CODEC_VERSION = 1;`.

- [ ] **Step 3: Build + manifest tests**

Run: `cd build && ninja unit_tests_dbms > cas_3a_build.log 2>&1; tail -5 cas_3a_build.log && ./src/unit_tests_dbms --gtest_filter='CasRootShard*' > cas_3a_test.log 2>&1; grep -E 'PASSED|FAILED' cas_3a_test.log | tail -5`
Expected: build clean; manifest round-trip tests pass. Update any test that asserted `codec_version` bytes or the old no-framing layout — to round-trip through `encodeRootShard`/`decodeRootShard` (semantics only; add a `published_at_ms` round-trip assertion and a future-`min_reader` fail-closed test if not already covered by the framing tests).

---

### Task 3: Wire `published_at_ms` at the publish/read sites

**Files:** `ContentAddressedMetadataStorage.cpp`, `ContentAddressedTransaction.cpp`

- [ ] **Step 1: Write the typed field instead of the string key**

- `ContentAddressedMetadataStorage.cpp:1030`: replace
  `payload.mutable_files[".ca_mtime"] = std::to_string(static_cast<uint64_t>(::time(nullptr)));`
  with `payload.published_at_ms = static_cast<uint64_t>(::time(nullptr)) * 1000;` (epoch **ms** — the field is `_ms`; multiply the seconds source by 1000, or switch to a ms clock).
- `ContentAddressedTransaction.cpp:280`: same replacement.
- `ContentAddressedTransaction.cpp:157`: the rename carry-over `payload.mutable_files = resolved->mutable_files;` — also carry the stamp: `payload.published_at_ms = resolved->published_at_ms;` (a rename is not a new part).

- [ ] **Step 2: Read the typed field in getLastModified**

- `ContentAddressedMetadataStorage.cpp:~652-670`: replace the `mutable_files.find(".ca_mtime")` lookup + decimal-seconds parse with `resolved->published_at_ms` (already an integer; convert ms→the unit `getLastModified` returns — it currently returns seconds, so divide by 1000, or return ms consistently with how callers use it — match the existing return contract). Remove the parse-error branch (a typed integer can't be malformed). If `published_at_ms == 0` (unset), preserve the existing fallback behavior the old code used when `.ca_mtime` was absent.

Note: the `Resolved` struct (returned by `resolveRef`/`listRefs`) must also carry `published_at_ms` — add it to `Resolved` (in `CasStore.h`) and populate it where `Resolved` is built from `RefPayload` (`CasStore.cpp` `listRefs` ~551 and the resolve path), mirroring `tree_size`/`mutable_files`.

- [ ] **Step 3: Build + full sweep + commit**

Run: `cd build && ninja unit_tests_dbms > cas_3a_build.log 2>&1; tail -3 cas_3a_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_3a_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_3a_sweep.log | tail -6`
Expected: only the baseline red. The transaction tests that check getLastModified / rename mtime carry-over still pass against the typed field.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_root_shard.proto \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
# plus any updated test files
git commit -m "CA: manifest on the CasFormat framing header; typed RefPayload.published_at_ms

The root-shard manifest now carries its version in the CasFormat framing header
([magic CARS][writer:u16][min_reader:u16]) instead of an in-message
codec_version (reserved 1); decode reads+gateOnReads the header pre-parse. The
.ca_mtime magic mutable_files key becomes a typed RefPayload.published_at_ms
(epoch ms), carried on Resolved and across rename. No JSON, no migration
(pre-release).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review (inline)

**Spec coverage (3a slice):** manifest framing header (Part III.2 + the proto-library note) → Tasks 1-2; `ca_mtime → typed published_at_ms` (Part III.2) → Tasks 1-3; uses Plan 1's `writeFramingHeader`/`readFramingHeader`/`currentWriterVersion(FormatId::Manifest)`. NOT here: proto rename to `cas_format.proto` (3d), gc-snap (3b), other JSON objects (3c). ✓
**Placeholder scan:** proto edits + the encode/decode framing pattern + the wiring sites (by file:line) are concrete; the getLastModified ms-unit reconciliation and the `Resolved.published_at_ms` plumbing are explicit, with the contract to preserve (fallback when unset). Test updates are round-trip-only. ✓
**Type consistency:** `published_at_ms` is `uint64` (proto) / `uint64_t` (C++ struct + Resolved); `MANIFEST_MAGIC="CARS"` 4 bytes; `FormatId::Manifest` (Plan 1); framing helpers signatures per Plan 1 (`writeFramingHeader(WriteBuffer&, string_view, WriterStamp)`, `readFramingHeader(ReadBuffer&, string_view, string_view)`). ✓
**Risk note:** the manifest is the root object — the reviews must confirm the framing round-trips and that `published_at_ms` ms-vs-seconds is consistent end-to-end (getLastModified contract). Flag if `time(nullptr)*1000` loses sub-second precision matters (it doesn't for mtime).
