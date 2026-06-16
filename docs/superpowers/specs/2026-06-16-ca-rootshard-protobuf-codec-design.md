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
- Back-compatible read of existing JSON pools; transparent forward migration.

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
  uint32 codec_version = 1;           // = 2 (the JSON format is the implicit v1); future value -> NOT_IMPLEMENTED
  uint64 shard_version = 2;
  uint64 fence_round = 3;
  map<string, RefPayload> refs = 4;   // decode is order-independent; see Determinism
  repeated JournalRecord  journal = 5;// insertion order preserved (repeated fields keep order)
}
```

`UInt128 tree_id` <-> 16 raw bytes via a fixed big-endian convention (`writeBinaryBigEndian`-equivalent already used elsewhere in the codebase); a `RefPayload.tree_id` / `JournalRecord.tree_id` whose length != 16 on decode -> `CORRUPTED_DATA`.

### Determinism

The manifest is **CAS-by-token (ETag), not content-addressed** — byte-determinism is NOT a correctness requirement. We nonetheless serialize **deterministically** (protobuf deterministic mode: `CodedOutputStream::SetSerializationDeterministic(true)`, which sorts `map<>` entries by key; `repeated` keeps insertion order) so golden tests are stable and output matches the current JSON codec's canonical ordering (refs name-sorted, mutable_files name-sorted, journal in insertion order). Decode is order-independent regardless.

## Codec API

`CasRootShardCodec.{h,cpp}` keeps the public surface unchanged so call sites (`CasStore.cpp:269,412,580`) are untouched:

- `String encodeRootShard(const RootShard &)` — now builds a `RootShardManifest`, sets `codec_version=2`, serializes deterministically. Maps the existing `RootShard`/`RefPayload`/`JournalRecord` structs 1:1 onto the proto messages.
- `RootShard decodeRootShard(std::string_view data)` — **dispatches on the first byte**:
  - `data` empty -> `CORRUPTED_DATA`.
  - `data[0] == '{'` (0x7B) -> **legacy JSON path** (the current decoder, kept verbatim, renamed `decodeRootShardJson`).
  - else -> **protobuf path** (`decodeRootShardProto`): `ParseFromString` into `RootShardManifest`; on parse failure -> `CORRUPTED_DATA`; `codec_version > 2` -> `NOT_IMPLEMENTED`; `op == JOURNAL_OP_UNSPECIFIED` or `tree_id.size() != 16` -> `CORRUPTED_DATA`.
  - First-byte dispatch is unambiguous: a `RootShardManifest` always begins with field-1's tag byte `0x08` (varint `codec_version`), never `0x7B`.

Fail-closed parity with today: wrong format / malformed / unknown→(n/a, positional) / missing required semantics / bad hash length / bad enum -> `CORRUPTED_DATA`; future `codec_version` -> `NOT_IMPLEMENTED`.

## Compatibility / migration

- **Write-new, read-both.** A B164 binary always writes protobuf; it reads both JSON (legacy) and protobuf. Every `mutateShard` already does read→modify→write, so the first mutation of each shard in an existing pool transparently rewrites its manifest JSON→protobuf. No migration step, no downtime, no operator action.
- **Forward-only.** Once a pool is written by a B164 binary, a *pre*-B164 binary cannot decode the new manifests (it has no protobuf path). This is a one-way format upgrade. Documented as a compatibility note; acceptable for the CA PoC (single-version deployments). The legacy JSON decoder is retained indefinitely so mixed/old pools keep working during rollout.

## Build wiring (precedent: BuzzHouse)

- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Proto/CMakeLists.txt`: `protobuf_generate(LANGUAGE cpp OUT_VAR ... PROTOS cas_root_shard.proto)` -> static lib `clickhouse_cas_proto` (mirrors `src/Client/BuzzHouse/Proto/CMakeLists.txt`).
- `src/CMakeLists.txt`: `if (TARGET ch_contrib::protobuf) target_link_libraries(dbms PRIVATE clickhouse_cas_proto)` (mirrors line 860).
- Guarded by `ch_contrib::protobuf` (always on in normal builds; the codec includes a `#if USE_PROTOBUF`-style guard with a clear build error if absent, since the CA disk now hard-depends on it).

## Debuggability

`clickhouse-disks ca-decode-manifest <disk> <path>` reads a manifest object and prints JSON (protobuf reflection `MessageToJsonString`, or maps back through the JSON encoder). Restores the introspection the §4 JSON gave, reusing the `clickhouse-disks` CA command pattern established by the #5 fsck work.

## Testing (TDD)

`src/Disks/tests/gtest_cas_rootshard_codec.cpp` (new):
- **Round-trip**: encode→decode == identity for: empty manifest; refs with multi-key `mutable_files`; a large journal (e.g. 2430 records); `tree_id` boundary values (all-zero, all-FF).
- **Golden**: a fixed `RootShard` → fixed bytes (deterministic serialization) — guards accidental format drift.
- **Legacy JSON decode**: a captured real JSON manifest still decodes to the expected `RootShard` (back-compat).
- **First-byte dispatch**: a JSON blob and a protobuf blob each route to the right decoder.
- **Fail-closed**: truncated bytes, random garbage, `op=0`, `tree_id` length≠16, `codec_version=99` → the right exception code.
- **Size/CPU micro-check** (informational, not a gate): encode+decode time and byte size, JSON vs protobuf, on a 2430-record journal — records the achieved win in the test log.

All existing `Cas*`/`CaWiring*` suites must stay green (the codec is transparent to them).

## Risks

1. **Build integration** — de-risked by the BuzzHouse precedent; verify the generated headers' include path and the `dbms` link.
2. **protobuf `map<>` serialization order** — handled by deterministic serialization (for golden tests only; correctness is order-independent).
3. **Forward-only format** — documented; the legacy JSON reader is retained so rollout and old pools are unaffected.
4. **proto3 zero-value ambiguity** — irrelevant here: all manifest fields are always set; the only enum-zero case is `JOURNAL_OP_UNSPECIFIED`, which is an explicit fail-closed sentinel.
