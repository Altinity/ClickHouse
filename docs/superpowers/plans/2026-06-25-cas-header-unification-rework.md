# CAS Header Unification Rework — Implementation Plan (2026-06-25)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** Replace the binary framing-header (the 8-byte `[magic][writer][min_reader]` prefix added to the protobuf objects in 3a/3c) with the **converged header model** (spec Part II, "CONVERGED HEADER MODEL 2026-06-25"): every object carries `magic + writer_version + compatibility_version`, mutable objects as a **`CasHeader` protobuf message (field 1)** — pure protobuf, no binary prefix — and hashed objects as the trio in the **binary envelope core** (rename `min_reader_version` → `compatibility_version`). Add the `pool-meta` `min_reader_generation` startup gate. Remove the `CasFormat` binary framing helpers.

**Architecture:** Mutable codecs stop prepending a binary framing header and instead set/read a `CasHeader { fixed32 magic; uint32 writer_version; uint32 compatibility_version; }` embedded as **field 1** of each object message → the on-disk object is *pure protobuf*, `protoc`-decodable with no prefix. The hashed envelope keeps its binary header but renames the second version slot to `compatibility_version` and documents the write-down-to-floor discipline. Reader rule everywhere: check `magic` (→`CORRUPTED_DATA`), then `compatibility_version > G_build` (→`UNKNOWN_FORMAT_VERSION`, a cheap post-parse check for protobuf), then interpret. Pre-release: `compatibility_version = G_BUILD` (current generation), no branching yet; the floor/roster stays deferred.

**Tech Stack:** C++ (ClickHouse), protobuf, `CasFormat`, gtest, ninja. Allman, `-Werror`. Branch `cas-vfs-path-mapping` (no master/amend/rebase). Pre-release — no migration; field renumbering is free (the no-compat-scaffolding rule).

**Build & test:** `cd build && cmake . && ninja unit_tests_dbms > cas_hdr_build.log 2>&1` (no `-j`); full sweep `--gtest_filter='Cas*:Ca*'`; only tolerated red `CaWiringOps.FreezeViaHardLinksIntoShadow`.

**Scope:** the proto, all 8 mutable codecs, `CasFormat`, `CasEnvelope` (rename only), `CasPoolMeta` + the open/validate path, and tests. Does NOT add any real compatibility_version *branch* (none exists pre-release — just stamp + check), nor build the roster. Out of scope: B92, B8/B64/B1, B164b, B194.

---

### Task 1: Proto — `CasHeader` + magic constants + embed as field 1

**Files:** `Core/Proto/cas_root_shard.proto`

- [ ] Add the header message + a magic enum/constants near the top:
```proto
message CasHeader {
  fixed32 magic = 1;                 // 4 ASCII bytes, e.g. 'CARS' little-endian; per-type constant
  uint32  writer_version = 2;        // forensic: the build/generation that wrote this
  uint32  compatibility_version = 3; // functional: the floor targeted; reader dispatches + fail-closes
}
```
- [ ] Embed `CasHeader header = 1;` as the **first field of every mutable message**: `RootShardManifest`, `PoolMetaProto`, `WatermarkProto`, `GcStateProto`, `RetiredSetProto`, `HeartbeatProto`, `RootsRegistryProto`, `GcOutcomeLogProto`. Renumber each message's existing fields to free field 1 (pre-release; renumber freely; update the `reserved` lines — e.g. `RootShardManifest` currently `reserved 1` (old codec_version) → reclaim it for `header` since no on-disk data exists). Keep field numbers stable *within* this commit (so the C++ accessors match).
- [ ] Define the per-type magic fixed32 values (the `CA__` ASCII as little-endian `fixed32`) in C++ (see Task 3), not the proto (proto can't hold a constant; the proto comment documents them). Magics: blob `CABL`, tree `CATR`, manifest `CARS`, pool-meta `CAPM`, watermark `CAWM`, gc-state `CAGT`, retired-set `CART`, heartbeat `CAHB`, roots-registry `CARR`, gc-outcomes `CAGO`.
- [ ] Build to regenerate (`cmake .` + `ninja`); expect codec compile errors (next task).

### Task 2: `CasFormat` — drop framing helpers, add header helpers

**Files:** `CasFormat.h`, `CasFormat.cpp`, `gtest_cas_format.cpp`

- [ ] **Remove** `writeFramingHeader`, `readFramingHeader`, `FramingHeader`, `FRAMING_HEADER_SIZE` (the binary prefix is gone). Remove the now-unused tests.
- [ ] Add a per-`FormatId` **magic constant** table + accessor `uint32_t magicFor(FormatId)` returning the `CA__` ASCII as `fixed32`-compatible `uint32` (LE), with a `default`-throw for an unexpected id.
- [ ] Add `uint16_t currentCompatibilityVersion()` (== `G_BUILD` pre-roster) and keep/repurpose the read-gate as `void checkCompatibility(uint32_t compatibility_version, std::string_view what)` → throws `UNKNOWN_FORMAT_VERSION` if `> G_BUILD` (the post-parse check). `currentWriterVersion`/the change-point table can stay as forensic generation source or be simplified to `G_BUILD`.
- [ ] Unit tests: `magicFor` per id; `checkCompatibility` pass/fail-closed at the `G_BUILD` boundary.

### Task 3: Mutable codecs — set/check `CasHeader` (all 8)

**Files:** `CasRootShardCodec.cpp`, `CasPoolMeta.cpp`, `CasWatermark.cpp`, `CasGcFormats.cpp` (gc-state + retired-set), `CasHeartbeat.cpp`, `CasRootsRegistry.cpp`, `CasGcOutcomes.cpp` (+ their tests).

Per codec, study the post-3c body and apply the uniform change:
- [ ] **Encode:** remove the `writeFramingHeader(...)` prefix + the `WriteBufferFromOwnString`/`writeString(body)` wrapping; instead populate `msg.mutable_header()` with `magic = magicFor(FormatId::X)`, `writer_version = currentWriterVersion()`, `compatibility_version = currentCompatibilityVersion()`, then `SerializeToString` the whole message (it now starts with field 1 = header) and return that directly (pure protobuf, no prefix).
- [ ] **Decode:** remove the `ReadBufferFromMemory`/`readFramingHeader`/`data.substr(FRAMING_HEADER_SIZE)` dance; `ParseFromArray(data)` directly; then `if (msg.header().magic() != magicFor(FormatId::X)) throw CORRUPTED_DATA("... bad magic")`; `checkCompatibility(msg.header().compatibility_version(), "x")`; then map fields as before, **keeping every existing post-parse invariant** (pool-meta constants, enum-zero rejection, ordering, etc.).
- [ ] **Tests:** update each codec's round-trip to assert the header round-trips (magic, writer, compat); add a "wrong magic → CORRUPTED_DATA" and a "future compatibility_version → UNKNOWN_FORMAT_VERSION" case. Remove tests asserting the old binary framing bytes.
- [ ] Commit per codec (or grouped) with message `CA: <object> uses CasHeader field (drop binary framing)`.

### Task 4: Hashed envelope — rename `min_reader_version` → `compatibility_version`

**Files:** `CasEnvelope.h`, `CasEnvelope.cpp`, envelope tests.
- [ ] Rename the struct field + the encode/decode wire field (same offset/width — the 2b core already has the slot) `min_reader_version` → `compatibility_version`. Reader check stays `compatibility_version > G_build → UNKNOWN_FORMAT_VERSION`.
- [ ] Update the doc comment to describe the write-down-to-floor discipline (for trees; blob payload is raw and doesn't branch). No behavior change (no tree-format-v2 exists yet). Update tests referencing the old name.
- [ ] Commit `CA: envelope version slot is compatibility_version (align with the converged header model)`.

### Task 5: `pool-meta` — `min_reader_generation` startup gate

**Files:** `cas_root_shard.proto` (`PoolMetaProto`), `CasPoolMeta.{h,cpp}`, the pool open/validate path (`CasStore`/`PoolMeta::createOrValidate`), tests.
- [ ] Add `uint64 min_reader_generation = N;` to `PoolMetaProto` + the `PoolMeta` struct (default 0). Set it at pool creation to the current generation's required floor (pre-release: 0/1).
- [ ] In the pool open path (`createOrValidate` / wherever pool-meta is read at startup), after decoding pool-meta: `if (G_BUILD < min_reader_generation) throw Exception(UNKNOWN_FORMAT_VERSION, "CAS pool requires reader generation {} but this build supports {}", min_reader_generation, G_BUILD)`. Clear "upgrade required" message.
- [ ] Test: a pool-meta with `min_reader_generation` above `G_BUILD` makes open fail-closed; at/below it opens.
- [ ] Commit `CA: pool-meta min_reader_generation startup gate`.

### Task 6: Document the ser/de discipline + full sweep

**Files:** `cas_root_shard.proto` header comment; final verification.
- [ ] In the proto header, document the write-down-to-floor branch pattern (safe-additive = new field number, no branch; unsafe/replacing = branch on `compatibility_version` until the floor rises; prune old arms after full upgrade) and the `CasHeader`/magic table.
- [ ] Build + `--gtest_filter='Cas*:Ca*'`; only the baseline red. Confirm no `writeFramingHeader`/`readFramingHeader`/`FRAMING_HEADER_SIZE`/`min_reader_version` references remain: `grep -rn "writeFramingHeader\|readFramingHeader\|FRAMING_HEADER_SIZE\|min_reader_version" src/Disks/ | grep -v tests` → empty.
- [ ] (no commit) rework complete.

---

## Self-Review (inline)
**Spec coverage:** converged header model (Part II box) — `CasHeader` field for mutable (Tasks 1,3), binary trio rename for hashed (Task 4), magic+writer+compatibility everywhere, post-parse magic + `compatibility_version > G_build` gate (Tasks 2,3), pool-meta startup gate (Task 5), framing helpers removed (Task 2), ser/de discipline documented (Task 6), no CRC, roster deferred (compat = G_BUILD). ✓
**Placeholder scan:** per-codec procedure is the proven study-the-body pattern; structural decisions (CasHeader shape, field-1 embedding, magic-as-fixed32, the encode/decode replacement, the envelope rename, the pool gate) are concrete. Field renumbering per message is "free field 1, renumber rest" (pre-release) — explicit. ✓
**Type consistency:** `CasHeader{fixed32 magic, uint32 writer_version, uint32 compatibility_version}`; `magicFor(FormatId)->uint32`; `checkCompatibility(uint32, string_view)`; `currentCompatibilityVersion()`/`currentWriterVersion()`; magics the `CA__` set; envelope field renamed consistently. ✓
**Risk:** keep every post-parse invariant during the encode/decode rewrite (don't drop a check while removing the framing). The reviews + full sweep + (later) soak are the backstop. The magic-as-`fixed32` endianness must be consistent between `magicFor` (write) and the decode compare (both use the same `uint32` constant, so it's self-consistent regardless of byte order).
