# CA RootShard manifest — Protobuf codec (B164a)

**Status:** design, awaiting review · **Date:** 2026-06-16 · **Branch:** `cas-mergetree-poc`
**Backlog:** B164 (this = part **a**, the binary codec). Journal *size* is governed by GC throughput (B160/B163), not this change. The journal/refs object split (B164c) is explicitly out of scope (it would double PUTs on the hot write path — rejected by the user).

## Problem

Live profiling of soak #7 (2026-06-16) found the dominant CPU bottleneck on the CA write path: the `RootShard` manifest is encoded as **strict JSON** (`CasRootShardCodec.{h,cpp}`, the §4 "non-hashed metadata = JSON" decision of 2026-06-11), and **every** ref mutation read-modify-writes the whole object. Measured: one manifest = **287,601 bytes**, of which `refs` is 6.5 KB (38 refs) and `journal` is **300 KB (2430 records, 98%)**; `shard_version=22222`. The CPU profiler's #1 leaf was `DB::WriteBuffer::write(char)` (char-by-char `writeJSONString`); the hot stacks were all `encodeRootShard`/`decodeRootShard`/`Cas::checkNoUnknownKeys` from `Store::mutateShard` ← `Build::publish` (INSERT) and ← `dropRef`/`republishRef` ← part removal. ProfileEvents confirmed the read-modify-write: `S3PutObject` 7.73M ≈ `S3GetObject` 7.70M.

Each ref op pays: GET ~280 KB → JSON parse (Poco `Var` map + validate every key) → modify 38 refs → re-serialize ~280 KB → PUT. JSON is the constant-factor multiplier on the hottest path in the system.

## Goal

Replace the JSON manifest codec with **Protobuf** (chosen for compactness + encode/decode performance; `google-protobuf` is already vendored). Targets:
- Eliminate the JSON CPU (no Poco `Var`, no char-by-char `writeJSONString`, no `checkNoUnknownKeys`).
- ~2× fewer bytes per manifest (binary varints; 16-byte raw hashes instead of 32 hex chars; positional fields instead of repeated key strings).
- Preserve fail-closed decode semantics and add introspectability via a decode tool.
- **No JSON back-compat** (user decision, 2026-06-16): fresh pools are always protobuf; the JSON reader is removed entirely. No dual-path, no migration.

Non-goals: reducing the journal *record count* (that is B160/B163 — GC must fold/trim to keep pace); splitting journal from refs (B164c, rejected — extra hot-path PUT).

## Format

New schema `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Proto/cas_root_shard.proto`:

```proto
syntax = "proto3";
package DB.Cas.Proto;

message RefPayload {
  bytes  tree_id = 1;                 // raw 16 bytes (UInt128, big-endian)
  uint64 tree_size = 2;
  map<string, string> mutable_files = 3;
}

enum JournalOp {
  JOURNAL_OP_UNSPECIFIED = 0;         // decode fails closed on 0 (matches today's "bad enum")
  JOURNAL_OP_ADD = 1;
  JOURNAL_OP_REMOVE = 2;
}

message JournalRecord {
  JournalOp op = 1;
  string    ref_name = 2;
  bytes     tree_id = 3;              // raw 16 bytes
  uint64    at_version = 4;
}

message RootShardManifest {
  uint32 codec_version = 1;           // = 1 (protobuf is the only format); 0 -> CORRUPTED, > current -> NOT_IMPLEMENTED
  uint64 shard_version = 2;
  uint64 fence_round = 3;
  map<string, RefPayload> refs = 4;   // decode is order-independent; see Determinism
  repeated JournalRecord  journal = 5;// insertion order preserved (repeated fields keep order)
}
```

`UInt128 tree_id` <-> 16 raw bytes via a fixed big-endian convention (`writeBinaryBigEndian`-equivalent already used elsewhere in the codebase); a `RefPayload.tree_id` / `JournalRecord.tree_id` whose length != 16 on decode -> `CORRUPTED_DATA`.

### Determinism

The manifest is **CAS-by-token (ETag), not content-addressed** — byte-determinism is NOT a correctness requirement. We nonetheless serialize **deterministically** (protobuf deterministic mode: `CodedOutputStream::SetSerializationDeterministic(true)`, which sorts `map<>` entries by key; `repeated` keeps insertion order) so golden tests are stable: refs and mutable_files are name-sorted (map keys), the journal stays in insertion order. Decode is order-independent regardless.

## Codec API

`CasRootShardCodec.{h,cpp}` keeps the public surface unchanged so call sites (`CasStore.cpp:269,412,580`) are untouched:

- `String encodeRootShard(const RootShard &)` — builds a `RootShardManifest`, sets `codec_version=1`, serializes deterministically. Maps the existing `RootShard`/`RefPayload`/`JournalRecord` structs 1:1 onto the proto messages.
- `RootShard decodeRootShard(std::string_view data)` — **protobuf only** (no JSON path):
  - empty `data` -> `CORRUPTED_DATA`.
  - `ParseFromArray` into `RootShardManifest`; on parse failure -> `CORRUPTED_DATA`.
  - `codec_version == 0` (missing/zero — not a conforming manifest) -> `CORRUPTED_DATA`; `codec_version > 1` -> `NOT_IMPLEMENTED`.
  - `op == JOURNAL_OP_UNSPECIFIED` or `tree_id.size() != 16` -> `CORRUPTED_DATA`; the journal `at_version` invariants (≤ shard_version, non-decreasing) -> `CORRUPTED_DATA`.

## Compatibility

**No JSON back-compat** (user decision, 2026-06-16). A fresh CA pool is always protobuf; the legacy JSON encoder AND decoder are removed entirely (no dual path, no first-byte dispatch, no migration). The PoC recreates pools from scratch (`docker compose down -v`), so there are no JSON manifests to read. A pre-B164 binary cannot read a B164 pool and vice-versa — a clean one-way format, not a mixed-version concern.

## Build wiring (precedent: BuzzHouse)

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Proto/CMakeLists.txt`: `protobuf_generate(LANGUAGE cpp OUT_VAR ... PROTOS cas_root_shard.proto)` -> static lib `clickhouse_cas_proto` (mirrors `src/Client/BuzzHouse/Proto/CMakeLists.txt`).
- `src/CMakeLists.txt`: `if (TARGET ch_contrib::protobuf) target_link_libraries(dbms PRIVATE clickhouse_cas_proto)` (mirrors line 860).
- Guarded by `ch_contrib::protobuf` (always on in normal builds; the codec includes a `#if USE_PROTOBUF`-style guard with a clear build error if absent, since the CA disk now hard-depends on it).

## Debuggability

Offline introspection is provided by protobuf itself: `protoc --decode DB.Cas.Proto.RootShardManifest cas_root_shard.proto < manifest.bin` dumps a manifest to text using only the `.proto`. A `clickhouse-disks ca-decode-manifest` convenience command (reading through the disk abstraction) is an optional follow-up, not part of B164a.

## Testing (TDD)

Tests live in the existing `src/Disks/tests/gtest_cas_codecs.cpp` `CasRootShardCodec` suite (a separate file would collide on its symbols):
- **Round-trip**: encode→decode == identity for the refs+journal manifest, the empty manifest, and a 2430-record journal.
- **Deterministic encoding**: `encode(x) == encode(x)` across refs (a map) and journal — golden-stable.
- **Canonical order**: two manifests built in different insertion order encode byte-identically.
- **Journal invariants**: a deliberately-bad struct (descending `at_version`; `at_version` > shard_version) is encoded then rejected by decode (`CORRUPTED_DATA`); equal `at_version`s decode fine.
- **Fail-closed**: empty, non-protobuf garbage, and a parseable blob with `codec_version==0` → `CORRUPTED_DATA`; `codec_version` > current → `NOT_IMPLEMENTED`.

All existing `Cas*`/`CaWiring*` suites must stay green (the codec is transparent to them).

## Risks

1. **Build integration** — de-risked by the BuzzHouse precedent; the generated header is included by basename via the lib's SYSTEM include dir so its reserved identifiers don't trip `-Weverything -Werror`.
2. **protobuf `map<>` serialization order** — handled by deterministic serialization (for golden tests only; correctness is order-independent).
3. **One-way format** — no JSON reader, so a pre-B164 binary cannot read a B164 pool. Acceptable per the no-back-compat decision; the PoC recreates pools from scratch.
4. **proto3 zero-value ambiguity** — handled: a missing/zero `codec_version` is rejected as `CORRUPTED_DATA`; the only other enum-zero case is `JOURNAL_OP_UNSPECIFIED`, an explicit fail-closed sentinel.
