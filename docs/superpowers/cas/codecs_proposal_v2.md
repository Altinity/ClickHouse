---
description: 'Proposal v2 for CAS persisted formats: one universal 8-byte preamble, three body families, uniform versioning, optional compression for big metadata, and a predictable codec code layout.'
sidebar_label: 'CAS codecs proposal v2'
sidebar_position: 11
slug: /superpowers/cas/codecs-proposal-v2
title: 'CAS Codecs Proposal v2'
doc_type: 'reference'
---

# CAS Codecs Proposal v2 {#cas-codecs-proposal-v2}

**Status:** proposal. Companion to the audit in `codecs.md`, which maps the current on-disk reality.
The audit proposes local fixes inside the existing structure; this document proposes the structure
itself. Where the two disagree, this document wins for new work.

**Scope:** every persisted format under
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`. Pre-release rules apply: no
persisted data exists in the wild, so there is no dual-read compatibility scaffolding — one cutover,
one new baseline.

## Design Goals {#design-goals}

1. **One mental model.** A reader of any CAS object follows the same three steps: read the preamble,
   gate on version, decode the body by family. No exceptions, no special-case objects.
2. **Versionable by construction.** Every object carries `writer_version` and
   `compatibility_version` in the same two byte slots, gated *before* any body parsing. There is no
   third version field anywhere.
3. **Predictable code layout.** Every object has exactly one proto message (or one binary layout
   comment), one `encodeX` / `decodeX` pair whose bodies follow a fixed shape, one `FormatId` entry,
   one golden test. Adding an object is a 6-step checklist, and each step has one obvious location.
4. **Streaming where necessary, materialized where sufficient.** Unbounded-cardinality data streams
   with O(one block) resident memory. Bounded metadata is materialized whole, with explicit
   fail-closed size caps.
5. **Compression for all bigger metadata objects**, optional per write, always supported on read.
6. **Parse efficiency.** Exact-size single allocations, zero-copy record cursors, no per-record
   heap traffic on the hot merge paths.

Non-goals: changing object *contents* or the protocol semantics (journals, tokens, ack-floor);
introducing new serialization dependencies (FlatBuffers, Cap'n Proto); re-keying any object.

## Current State, Compressed {#current-state-compressed}

The audit identifies five body families plus one exception. The concrete structural problems v2
removes:

- **Five families where three suffice.** `PartManifest` (`CAPT`) is a hybrid: a custom binary
  header wrapping an embedded `CARN` record stream that the decoder materializes anyway — it pays
  for streaming machinery it never uses. `GcHeartbeat` is a sixth, unversioned, magic-less shape.
- **Version gate runs after the parse.** Pure-protobuf objects check `compatibility_version` only
  after `ParseFromArray` succeeds on the whole body. A future-format object that no longer parses
  reports `CORRUPTED_DATA` instead of `UNKNOWN_FORMAT_VERSION` — the fail-closed rule fires with
  the wrong diagnosis.
- **Version-field naming drift.** `RunFile` and `PartManifest` carry a field named `format_version`
  that is actually passed to `checkCompatibility`, i.e. it *is* `compatibility_version`. The
  envelope names the same slots correctly. Three spellings of one concept.
- **Boilerplate ×8.** Every protobuf codec hand-writes the same ~25 lines: set header, serialize,
  check empty, parse, check magic, check compatibility. Eight copies today (`CasRootShardCodec`,
  `CasGcFormats` ×2, `CasGcOutcomes`, `CasPoolMeta`, `CasServerRoot` ×3), each a chance to drift.
- **No compression anywhere.** `RunHeader.codec` admits only `0`. Protobuf bodies are raw. The
  biggest metadata objects — root shards with long journals, retired sets full of ETag strings,
  outcome logs, part manifests with paths and inline bytes — are exactly the compressible ones.
- **No at-rest integrity for protobuf metadata.** A flipped bit in a `varint` can silently change a
  value and still parse. Only `RunFile` (block/footer CRC) and the envelope (`header_hash`) detect
  corruption today.
- **Per-record allocations on hot paths.** `RunFileReader::next` copies key and payload into
  caller-owned `String`s for every record; GC merge loops run this over millions of records.

## The v2 Model {#the-v2-model}

### Rule 1: One Universal Preamble {#rule-1-one-universal-preamble}

Every self-describing CAS object begins with the same 8 bytes, little-endian:

```text
CasPreamble (8 bytes, fixed, little-endian):
  [0,4)  magic                  4 ASCII bytes, per object type ("CARS", "CABL", ...)
  [4,6)  writer_version   u16   forensic: the build generation that wrote this
  [6,8)  compatibility_version u16   read gate: > G_BUILD => UNKNOWN_FORMAT_VERSION
```

This is already the exact layout of the first 8 bytes of `CasEnvelope` — the envelope needs no
change to comply. Everything else moves to it.

Consequences:

- **Sniffing is trivial.** `head -c 8` identifies and version-gates any object. One helper,
  `sniffPreamble`, serves decoders, `fsck`, `CasInspect`, and error messages.
- **The gate runs first.** `checkCompatibility` fires on the raw preamble before any body byte is
  interpreted — a future object always reports `UNKNOWN_FORMAT_VERSION`, never a parse error.
- **`CasHeader` leaves the proto schema.** The protobuf messages stop carrying magic and versions
  as field 1; the preamble owns them. One source of truth instead of two. The proto field number is
  `reserved` so it is never reused.

The cost: metadata objects are no longer decodable with bare `protoc --decode` from byte 0. The
recovery recipe stays one shell stage — skip the fixed frame, optionally decompress — and
`CasInspect` learns to dump any framed object. This trade is taken deliberately: gate-before-parse,
compression, and at-rest CRC are worth more than protoc-purity, and the format stays third-party
reimplementable with universal primitives (fixed struct + `zstd` + `protobuf`).

### Rule 2: Exactly Three Body Families {#rule-2-exactly-three-body-families}

Everything after the preamble belongs to one of three families. The magic (via `FormatId`)
determines the family statically; no object ever switches family.

| Family | Continuation after preamble | Parsing contract | Used for |
|---|---|---|---|
| **Proto** | 24-byte frame tail, then protobuf body (optionally compressed) | materialized whole, size-capped | all control/metadata objects |
| **Stream** | 8-byte stream header, then CRC-framed sorted blocks + footer index | streamed, O(one block) resident, `seek` via footer | unbounded sorted record data |
| **Payload** | envelope core `[8,94)` + TLV area, then raw payload bytes | fixed offsets, ranged reads, zero hot-path parsing | content-addressed blob bytes |

Raw passthrough files (namespace verbatim files, mountpoint objects) remain a deliberate
**non-family**: not self-describing, no preamble, CAS never interprets the bytes. Unchanged.

### Proto Family: The Framed Protobuf Object {#proto-family}

```text
CasProtoFrame (32 bytes total, little-endian):
  [0,8)   CasPreamble
  [8]     body_codec   u8    0 = none, 1 = zstd (whole body, one zstd frame)
  [9]     flags        u8    must be 0 in v1 (reader rejects nonzero)
  [10,12) reserved     u16   must be 0
  [12,16) body_crc32c  u32   CRC32C over the stored body bytes [32, 32+body_len)
  [16,24) body_len     u64   stored (possibly compressed) body length; must equal remaining bytes
  [24,32) raw_len      u64   uncompressed body length; == body_len when body_codec = 0
```

Decode order, fixed for every Proto object:

1. `sniffPreamble` — magic must equal the expected `FormatId` magic (`CORRUPTED_DATA` otherwise),
   `compatibility_version` gated (`UNKNOWN_FORMAT_VERSION`).
2. Validate frame: `flags == 0`, `reserved == 0`, known `body_codec`, `body_len` equals exactly the
   remaining object bytes, `raw_len <= cap(FormatId)` — the cap check happens **before** any
   allocation, so a corrupted length can never trigger a multi-GiB transient allocation.
3. Verify `body_crc32c` over the stored bytes.
4. If compressed: one exact-size `raw_len` allocation, one-shot `zstd` decompress into it
   (cross-check zstd's own content size against `raw_len`).
5. `ParseFromArray` over the flat buffer, then per-object invariant checks.

Everything up to step 5 lives in **one** shared helper pair:

```cpp
template <typename Msg>
String encodeFramedProto(FormatId id, const Msg & msg, FrameCompression compression);

template <typename Msg>
Msg decodeFramedProto(FormatId id, std::string_view data, std::string_view what);
```

A per-object codec is then *only* the struct↔proto field mapping plus invariant validation — the
part that is genuinely per-object. The eight hand-written header/parse/magic/gate blocks disappear.

Why 32 bytes and not less: `raw_len` buys the exact single allocation and the decompression-bomb
guard; `body_crc32c` buys at-rest corruption detection protobuf cannot give; both are cheap
against S3 object overheads, even for the tiny singletons (`ServerEpoch`, `Owner`).

### Stream Family: The Record Stream {#stream-family}

The `CARN` block-framed design is kept — it is the right tool for unbounded sorted data: bounded
writer memory, CRC-framed blocks, sparse footer index, ranged `seek`, deterministic bytes,
streaming open with a fixed request profile. v2 changes only the header, the naming, and the
reader's allocation behavior.

```text
CasStreamHeader (16 bytes total, little-endian):
  [0,8)   CasPreamble               (replaces magic[4] + format_version u16 — the u16 pair now
                                     matches every other object; the old 13-byte header is gone)
  [8]     kind         u8           record kind (typed contract, see below)
  [9]     key_schema   u8           fixed per kind
  [10]    block_codec  u8           0 = none; 1 = zstd-per-block (reserved, see Compression)
  [11]    flags        u8           must be 0
  [12,16) block_size   u32          target block size
```

Blocks and footer are unchanged from the current `RunFile` implementation (record framing, block
CRC32C, footer CRC32C, `footer_len` trailer, absolute block offsets).

Renames, per the audit's naming note: the format is the "`CARN` record stream"; the C++ types
become `RecordStreamWriter`, `RecordStreamReader`, `RecordStreamMerger` (file
`CasRecordStream.h/.cpp`). "Run" jargon survives only in GC-internal variable names if at all.

**Typed opens are the only opens.** Callers never construct a reader from raw bytes plus faith;
they call a typed helper that validates `kind`, `key_schema`, `block_codec`, and block-size bounds
before any record is interpreted:

```cpp
RecordStreamReader openBlobTargetStream(Backend & backend, const String & key);
RecordStreamReader openBlobTargetStream(std::string_view borrowed_bytes);
```

Unknown `kind` fails closed. Enum values without a live producer are deleted from the enum (kept
only as a "never reuse" comment), per the audit's YAGNI rule.

**Zero-copy cursor.** `next` yields `std::string_view` key/payload valid until the next `next` /
`seek` call (they point into the resident block). The merger copies only its per-reader front
records. The block buffer is allocated once at the hard cap and reused across blocks — block loads
stop allocating entirely. The byte-loop `le32of` helpers become `unalignedLoadLittleEndian` on the
bounds-checked window.

### Payload Family: The Blob Envelope {#payload-family}

`CABL` is already correct under v2 rules: its core begins with the exact `CasPreamble`, versions
are gated fail-closed, the header is fixed-size with a constant pool-wide payload offset
(`blob_header_len`), and the TLV area handles extension with a critical-flag fail-closed path.
One deliberate wire change: the guard slot `[86,94)` switches from `header_hash` (CityHash64) to
`header_crc32c` (u32 at `[86,90)`, zero u32 at `[90,94)`) — the guard-primitive unification
described in [Integrity Model](#integrity-model). The recipe is unchanged (computed with the slot
zeroed); everything else is byte-identical.

Frame-level compression is structurally excluded here: payload identity *is* the raw bytes
(`logical_hash` over `[header_len, EOF)`), ranged reads must hit constant offsets, and part files
are ClickHouse-compressed already.

## Versioning {#versioning}

One rule, stated once:

- `writer_version` — forensic only. Never branched on.
- `compatibility_version` — the only read gate. Reader fails closed when it exceeds `G_BUILD`
  (`UNKNOWN_FORMAT_VERSION`). Writers stamp `currentCompatibilityVersion`.
- **There is no `format_version`.** The concept is deleted, not renamed. A layout change *is* a
  compatibility event and rides `compatibility_version` + `changePoints`, the existing machinery in
  `CasFormat.h`. `changePoints` stays, and gains its first real test at the first genuine bump.

Evolution recipes per family:

| Change | Recipe |
|---|---|
| Proto: additive field, safe to ignore | new proto field number; no version bump (unchanged rule from `cas_format.proto`) |
| Proto: breaking / reinterpreting | bump `compatibility_version` via `currentCompatibilityVersion`; decoder branches on the preamble value; prune the old arm when the floor rises |
| Stream: new record kind | new `kind` value + new typed open helper; old readers never open it (typed opens fail closed) |
| Stream: record/blocking layout change | `compatibility_version` bump, same as Proto breaking |
| Frame/preamble layout itself | new magic. The preamble is the one thing that must never change shape |
| Payload: new header field | TLV extension; critical flag if ignoring it would be wrong (existing mechanism) |

Pool-level gating (`PoolMeta.min_reader_generation`) is unchanged and orthogonal.

## Compression {#compression}

- **Proto family:** whole-body single-frame `zstd` behind `body_codec = 1`. Written when
  `raw_len >= threshold` (default 4 KiB; below that the frame overhead dominates and the singletons
  stay byte-inspectable). Readers accept both values unconditionally — "optional" is a writer-side
  policy knob, never a reader capability. This covers every bigger metadata object: root shards
  (journals compress extremely well — repeated proto structure, ref names, epochs), retired sets
  (ETag strings), outcome logs, fold seals, part manifests (paths + inline file bytes).
- **Stream family:** `block_codec` is reserved but ships as `0`. Rationale: after the part manifest
  moves to the Proto family (below), the remaining stream producers are GC blob-target /
  source-edge runs whose keys and payloads are high-entropy 128-bit hashes — compression buys
  almost nothing there. The byte is in the header so per-block `zstd` (compress each block payload
  independently; index offsets stay physical; `seek` unaffected) can be enabled later without a
  layout change.
- **Payload family:** structurally exempt (see above).

**Determinism constraint.** Compressed bytes are only as stable as the vendored `zstd` version, so
objects written through `putDeterministicArtifact` (fold seals, GC runs — adoption compares bytes)
**must write `body_codec = 0`**. Enforced in code, not convention: `encodeFramedProto` takes the
compression policy from a per-`FormatId` table, and the deterministic formats pin `None` there.
Everything else (CAS-by-token objects, where determinism is a golden-test nicety, not a protocol
requirement) is free to compress; golden tests for compressed formats assert on decoded content
plus a pinned-zstd byte snapshot.

Codec choice: raw single-frame `zstd`, not the ClickHouse `CompressedWriteBuffer` wire format. The
CH format brings per-64-KiB checksummed frames and `clickhouse-compressor` tooling, but ties a
portable third-party-reimplementable protocol to a ClickHouse-internal framing; plain `zstd` is one
call in every language, and `body_crc32c` + `raw_len` already cover integrity and allocation.
zstd's own frame checksum stays disabled — integrity coverage must not depend on `body_codec`
(see [Integrity Model](#integrity-model)).

## Integrity Model {#integrity-model}

The guards look different per family; the rule behind them is single and uniform:

> **Every byte range that CAS interprets is covered by exactly one fail-closed guard, verified
> before the bytes are trusted, at the granularity of the read unit.** Bytes CAS does not
> interpret are guarded by whoever does. No range is double-covered; none is silently uncovered.

"Read unit" is why the mechanisms differ, and it is the whole answer to "why is this not one
mechanism everywhere". A Proto object is read and parsed whole — one guard over the whole body is
necessary and sufficient; per-block framing would be complexity with no consumer. A Stream object
is *never* read whole (ranged `seek`, streaming fold) — a whole-object checksum is physically
unverifiable at read time, so the guard sits on the units that are actually read: the block and
the footer. The envelope core is one fixed 94-byte read unit — one guard over it. Same rule,
three read shapes; this is the same reason LSM and columnar formats checksum per block, not per
file.

Two primitives, two disjoint roles, never mixed:

- **CRC32C — corruption guard.** Detects flipped bits; verified on every read of its unit;
  failure is `CORRUPTED_DATA`. Used for: Proto body, Stream block, Stream footer, envelope core.
- **CityHash128 — identity/equality.** Names content or proves two write-once encodings are the
  same object; verified where identity is *consumed* (dedup, adoption, `fsck`), not on every read.
  Used for: blob content hash (`logical_hash`), `RunRefProto.checksum`.

Consequential cleanups:

- The envelope's `header_hash` (CityHash64 — a third primitive doing a CRC's job, "diagnostics
  quality" per its own comment) becomes `header_crc32c`: u32 at `[86,90)`, zero u32 at `[90,94)`.
  The core stays 94 bytes and the recipe is unchanged (computed with the slot zeroed). CityHash64
  leaves the protocol entirely.
- zstd's internal frame checksum (XXH64) stays **disabled**: `body_crc32c` already covers the
  stored body, and coverage must not depend on `body_codec` — a guard that exists only when
  compression is on is exactly the inconsistency this section exists to ban.
- `RunRefProto.checksum` is redefined (comment fix) as CityHash128 over the full stored object
  bytes — which is what writers already store; the "footer checksum" comment was stale. It is
  identity for adoption and `fsck`, **not** a read-path guard: blocks already carry CRCs, and a
  full-object check would force reading the whole run.
- `PartManifest.payload_digest` is **dropped**. Its guard role is subsumed by `body_crc32c`; it
  was never a key, never dedup, never verified on read. One less field, one less "is this
  load-bearing?" question. (`refMatchesBody` and the namespace check — the real fail-closed
  validations — stay.)

The full coverage map, including the deliberate delegations:

| Read unit | Guard | Verified |
|---|---|---|
| Preamble + frame fields | field validation (magic, versions, flags, lengths, codec domain) | every decode, first |
| Proto body (stored bytes) | `body_crc32c` | every decode, before decompress/parse |
| Stream block | block CRC32C | every block load |
| Stream footer | footer CRC32C | every footer load |
| Envelope core | `header_crc32c` | every envelope decode |
| Blob payload (ranged reads) | **delegated** to the embedded format: part data files carry ClickHouse's own per-frame checksums; small files travel inline in the manifest, covered by its `body_crc32c`. Full-object verification via the content hash lives in `fsck`, not on the read path | at use / `fsck` |
| Whole write-once stream | `RunRefProto.checksum` (identity) | adoption + `fsck` only |
| Raw passthrough files | **none at the CAS layer** — the bytes must be preserved verbatim, so no frame can be added; integrity belongs to the file's own format. The one genuinely unguarded surface: accepted and documented, not accidental | — |

## Object Dispositions {#object-dispositions}

| Object | Magic | v2 family | Compression | Change from today |
|---|---|---|---|---|
| Blob | `CABL` | Payload | none (structural) | guard slot only: `header_hash` → `header_crc32c` (primitive unification); payload bytes untouched |
| Pool metadata | `CAPM` | Proto | below threshold in practice | reframe; drop `CasHeader` field |
| Root shard | `CARS` | Proto | **yes** — the flagship win | reframe; drop `CasHeader` field |
| Part manifest | `CAPT` | **Proto** (was hybrid) | yes (paths, inline bytes) | new `PartManifestProto` with `repeated ManifestEntryProto`; embedded `CARN` stream and `RunKind::ManifestEntries` deleted; `payload_digest` dropped; `.proto` key suffix finally truthful |
| Record stream | `CARN` | Stream | reserved per-block | 16-byte v2 header; typed opens; zero-copy cursor; rename to `RecordStream` |
| GC state | `CAGT` | Proto | below threshold usually | reframe; drop `CasHeader` field |
| GC heartbeat | `CAHB` (new) | Proto | no (tiny) | was the 24-byte unversioned exception; becomes a two-field framed proto (`owner`, `hb_seq`). Written every few seconds — the 32-byte frame is noise. The exception dies |
| Fold seal | `CAFS` | Proto | **no — deterministic** | reframe; enum-domain validation on coverage fields |
| Blob-target run | `CARN` | Stream | no — deterministic | typed open |
| Part-manifest cleanup run | `CARN` | — | — | **removed** with `FoldSealProto.part_manifest_cleanup` and `partManifestCleanupKey`, per the audit: sealed but never read. If replay-from-durable-object is ever wanted, it returns as a Stream with a real reader and tests |
| Retired set | `CART` | Proto | yes | reframe; enum-domain validation (`kind`, `token_type`) |
| Outcome log | `CAGO` | Proto | yes | reframe; enum-domain validation |
| Owner anchor | `CAOW` | Proto | no (tiny) | reframe |
| Server epoch | `CAEP` | Proto | no (tiny) | reframe |
| Mount lease | `CAML` | Proto | no (tiny) | reframe |
| Namespace verbatim / mountpoint files | — | raw passthrough | — | none (explicitly out of the frame system) |

Subformats (`ManifestRefProto`, `RunRefProto`, `Token`, `OwnerBindingProto`) are unchanged as wire
shapes; their decode-side enum-domain validation (audit's "Tighten Decoder Strictness") is part of
this proposal's implementation, not a separate effort.

## Size Caps {#size-caps}

Every Proto decode enforces `raw_len <= cap(FormatId)` before allocating. Defaults:

| Class | Cap | Bound argument |
|---|---|---|
| Singletons (`CAPM`, `CAGT`, `CAOW`, `CAEP`, `CAML`, `CAHB`) | 1 MiB | fixed field count |
| Root shard `CARS` | 64 MiB | journal trim policy bounds the journal; refs bounded by table count per shard |
| Part manifest `CAPT` | 256 MiB | entries bounded by files-per-part; inline bytes bounded by the inline threshold × file count |
| Retired set / outcome log / fold seal | 256 MiB | GC sharding bounds per-shard cardinality |

A cap hit is `CORRUPTED_DATA` (these are 100–1000× above realistic sizes; hitting one means a
corrupt length or a protocol bug, and fail-closed is correct for both). Caps are constants next to
the codec, revisited with real soak numbers.

Stream family needs no body cap: resident memory is structurally one block (hard cap 1 MiB) plus
the footer index, and the footer parser already bound-checks every entry against the CRC-verified
footer body.

## Code Layout {#code-layout}

The convention, stated as the checklist for adding (or finding) a persisted object:

1. **One proto message** in `cas_format.proto` (Proto family only), no `CasHeader` field, schema
   evolution rules in the file header apply.
2. **One `FormatId` entry + magic** in `CasFormat.h` / `magicFor`, and one row in the proto file's
   magic table comment. The table is complete or CI fails (a unit test asserts every `FormatId` has
   a magic, a family, a cap, and a compression policy).
3. **One codec pair** `encodeX` / `decodeX` in the owning subsystem's codec file, with the fixed
   shape: map fields → call `encodeFramedProto` / call `decodeFramedProto` → validate invariants.
   Nothing else. If a codec pair contains the words `ParseFromArray` or `SerializeToString`, review
   rejects it — that logic lives in the shared helper only.
4. **One storage key** in `CasLayout`, documented with owner and lifecycle.
5. **One golden-bytes test + one corruption test** in `src/Disks/tests/gtest_cas_codecs.cpp` (or
   the format's own test file), driven by the shared harness (below).
6. **One row in `codecs.md`** (the audit doc doubles as the format registry).

File layout after the change:

```text
Core/
  CasFormat.h/.cpp        FormatId, magics, G_BUILD, changePoints, checkCompatibility,
                          per-format {family, cap, compression policy} table
  CasFrame.h/.cpp         CasPreamble + sniffPreamble + CasProtoFrame +
                          encodeFramedProto / decodeFramedProto + zstd body codec
  CasRecordStream.h/.cpp  Stream family: writer / reader / merger + typed opens (was CasRunFile)
  CasEnvelope.h/.cpp      Payload family (unchanged)
  CasCodecUtil.h          u128 wire forms, readFixedBytes, decodeGuarded (unchanged)
  CasRootShardCodec.*     mapping + invariants only
  CasManifestCodec.*      mapping + invariants only (entries now plain proto)
  CasGcFormats.*          mapping + invariants only (heartbeat joins the family here)
  CasGcOutcomes.*         mapping + invariants only
  CasPoolMeta.*           mapping + invariants only
  CasServerRoot.*         mapping + invariants only
```

Error taxonomy is enforced in `CasFrame.cpp` once, instead of eight times: bad magic / truncation /
CRC / bad frame field / enum out of domain → `CORRUPTED_DATA`; future `compatibility_version` /
unknown critical TLV → `UNKNOWN_FORMAT_VERSION`. Per-object code can only *add* invariant checks,
not weaken the split.

**Shared test harness.** One parameterized fixture takes `{FormatId, encode fn, decode fn, sample
object}` and automatically runs: round-trip equality; golden bytes (pinned hex, compressed and
uncompressed arms); truncation at every byte boundary of the frame → `CORRUPTED_DATA`; flipped byte
in body → `CORRUPTED_DATA` (CRC); `compatibility_version + 1` → `UNKNOWN_FORMAT_VERSION`; wrong
magic (another format's valid object) → `CORRUPTED_DATA`; nonzero `flags`/`reserved` →
`CORRUPTED_DATA`; `raw_len` over cap → `CORRUPTED_DATA` with no allocation (asserted via memory
tracker in debug). Every format gets all of this for the price of one registration line — which is
what makes "easy to change" real: a layout mistake fails ten pinned tests, not a soak run.

## Memory And Parse Efficiency {#memory-and-parse-efficiency}

Consolidated from the sections above, as the checklist reviewers hold the implementation to:

- Proto decode: exactly one body allocation (`raw_len`), sized before reading; decompress one-shot
  into it; parse from the flat buffer. No `String` staging copies.
- Length checks precede allocations everywhere (the existing `readFixedBytes` discipline, extended
  to the frame fields and `raw_len`).
- Stream reader: one block buffer allocated at the hard cap and reused; `next` returns views into
  it; merger copies only front records; scalar loads via `unalignedLoadLittleEndian`.
- Stream writer: unchanged bounded-memory contract (one in-flight block + footer index).
- Encode paths keep the sort-of-pointers pattern (`encodeRetiredSet`, `encodePartManifest`) — sort
  keys, not bodies.
- Blob hot path: unchanged — constant-offset ranged reads, no per-read header parse.

## Alternatives Considered {#alternatives-considered}

- **Status quo (pure protobuf + `CasHeader` field 1).** Keeps `protoc --decode` purity, but the
  version gate runs after the parse, compression has no place to live, integrity has no place to
  live, and the header is set/checked by hand in every codec. Rejected.
- **Everything protobuf, including runs.** One family fewer, but unbounded-cardinality GC data
  loses bounded-memory streaming and ranged `seek`; a multi-GiB run as one proto message is a
  non-starter. Rejected — the Stream family exists for a reason.
- **FlatBuffers / Cap'n Proto for zero-parse metadata.** Optimizes the wrong thing: CAS metadata
  objects are small and read whole over object storage; network dominates parse by orders of
  magnitude. New dependency, new toolchain, no protoc-compatible portability story. Rejected.
- **ClickHouse `CompressedWriteBuffer` format for bodies.** Idiomatic in-repo and brings framed
  checksums, but binds a portable storage protocol to ClickHouse-internal framing. Plain `zstd` is
  one call in any language. Rejected (revisit only if streaming decompress of huge Proto bodies
  ever matters — it should not, given the caps).
- **32-byte frame on Stream/Payload too (full frame uniformity).** The Stream footer and the
  envelope TLV area already own integrity and extension for their families; forcing `body_len` /
  `raw_len` onto them adds fields with no consumer. Uniformity lives at the 8-byte preamble, where
  it is real.

## Migration Plan {#migration-plan}

Pre-release, single cutover, no dual-read arms. Order chosen so each step lands green on its own:

1. **`CasFrame.h/.cpp`** — preamble, frame, `zstd` body codec, shared helper pair, shared test
   harness. Pure addition, nothing wired.
2. **Reframe the eight Proto objects** — drop `CasHeader` from proto messages (reserve field 1),
   rewrite each codec pair as mapping-only, register in the per-format table, migrate tests to the
   harness. Add the enum-domain validations from the audit while touching each decoder.
3. **`CAPT` → Proto family** — `PartManifestProto`, delete the embedded stream path,
   `RunKind::ManifestEntries`, and `payload_digest`.
4. **Stream v2** — 16-byte header, typed opens, zero-copy cursor, renames, delete the
   part-manifest-cleanup run + `FoldSealProto.part_manifest_cleanup` + `partManifestCleanupKey`.
5. **Heartbeat → `CAHB`** framed proto (the fixed-record codec is deleted) **+ the envelope guard
   swap** — `header_hash` → `header_crc32c` in `CasEnvelope`, golden tests re-pinned.
6. **Compression on** for `CARS` / `CART` / `CAGO` / `CAPT` behind the per-format policy table
   (deterministic formats pinned to `None`); golden tests pin both arms.
7. **Docs + tooling** — update `codecs.md` to describe v2 as reality (it remains the registry),
   teach `CasInspect` to sniff/dump any framed object, update the stale JSON/`CAGS` comments the
   audit lists.

Steps 1–2 are the bulk of the value (uniform gate, integrity, layout); 3–5 delete the exceptions;
6 is a switch-flip; 7 is hygiene. Each step is soak-testable independently — the formats are
CAS-by-token or write-once, so a half-migrated *codebase* is fine as long as each *format* cuts
over atomically in one commit.

## What This Buys, Concretely {#what-this-buys-concretely}

- Families: 5 + 1 exception → **3 + explicit raw passthrough**; zero unversioned objects.
- Version fields: 3 names, 2 gate positions → **1 name pair, 1 position, gated pre-parse**.
- Codec boilerplate: ~25 lines × 8 objects → **1 shared helper + mapping-only codecs**.
- Integrity: protobuf metadata unguarded → **CRC32C on every metadata body**, under one stated
  rule (one guard per read unit, verified before trust).
- Integrity primitives: CRC32C + CityHash64 + CityHash128 in mixed roles → **two primitives with
  disjoint roles** (CRC32C = corruption guard, CityHash128 = identity); CityHash64 leaves the
  protocol.
- Compression: none → **all bigger metadata objects, optional, reader-universal**.
- Allocations: per-record copies on GC merge, unbounded corrupted-length allocs → **one exact
  allocation per Proto decode, zero-copy stream cursor, cap-before-alloc everywhere**.
- Adding a format: archaeology → **a 6-step checklist where every step has one location and the
  test harness writes the failure modes for you**.
