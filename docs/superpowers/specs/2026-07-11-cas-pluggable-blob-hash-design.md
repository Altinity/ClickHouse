---
description: 'Design for a per-disk selectable CAS blob content-hash function. Default stays cityHash128 v1.0.2; adds xxh3-128 (a stronger 128-bit hash) and sha256 (256-bit). The hash identity is encoded in blob paths and recorded pool-wide. Phased: Phase 1 = 128-bit selectable (cityHash128 + xxh3-128, identity stays UInt128); Phase 2 = the variable-length-digest refactor that admits sha256.'
sidebar_label: 'CAS pluggable blob hash'
sidebar_position: 22
slug: /superpowers/specs/cas-pluggable-blob-hash
title: 'CAS pluggable blob hash (selectable content-address function)'
doc_type: 'reference'
---

# CAS pluggable blob hash — design {#title}

**Status:** DESIGN (2026-07-11). **Branch:** `cas-gc-rebuild`. **Motivation:** the current blob content
address is `cityHash128` v1.0.2 — a non-cryptographic 128-bit hash with known structural weaknesses, weaker
than its bit-width suggests. Make the blob hash **selectable per disk**, encode the hash identity in blob
paths, keep **cityHash128 the default**, and add **xxh3-128** (a stronger 128-bit hash) and **sha256** (a
256-bit cryptographic hash). CA is pre-release: no data migration, formats may change freely.

## 1. The pervasive fixed-`UInt128` constraint (why this is phased) {#constraint}

The blob content address is a fixed `UInt128` (16 bytes) baked into ~10 core structures and ~30 files:
`ManifestEntry.blob_hash`, `PendingBlob`, the source-edge run key (`srcEdgeRunKey` =
`blob_hash(16 BE) ++ source_id(16 BE)`), GC delete/outcome rows, the per-hash `.meta` API, the dedup cache,
GC shard routing (`blobShard` uses `blob_hash >> 64`), and the fixed-16-byte (de)serialization in
`CasCodecUtil` + the 32-char gate in `CasIds.hexToU128`. **cityHash128 and xxh3-128 are both 128-bit** and
fit this unchanged; **only sha256 (256-bit) breaks it.** Therefore:

- **Phase 1 — 128-bit selectable (this spec's implementable scope):** cityHash128 (default) + xxh3-128,
  identity stays `UInt128`, hash id in the path + `PoolMeta`. Directly fixes the stated collision-weakness
  concern at minimal risk. No change to any settlement/GC serialization.
- **Phase 2 — variable-length digest (specced §7, deferred):** replace the fixed `UInt128` identity with a
  length-prefixed digest so sha256's 32-byte output is admissible. A large, careful refactor of the
  settlement/GC/token core; its own plan + TLA/soak validation.

## 2. Configuration {#config}

Per-disk config key `blob_hash`, read in `registerContentAddressedMetadataStorage`
(`MetadataStorageFactory.cpp`), following the validated-string-key pattern of `server_root_id`:

- `blob_hash` = `cityhash128` (**default**) | `xxh3-128` | `sha256`. Unknown value → `BAD_ARGUMENTS`
  (fail-closed). Phase 1 accepts `cityhash128`|`xxh3-128`; `sha256` throws `NOT_IMPLEMENTED` until Phase 2.
- The choice is **fixed at pool creation** and recorded in `PoolMeta` (§4). On reopen, the pool's recorded
  algo is authoritative; a disk config that disagrees with an existing pool's recorded algo → `BAD_ARGUMENTS`
  (fail-closed — never silently re-hash an existing pool).

## 3. Hash identity in blob paths {#paths}

Today: `<pool>/blobs/<2-char-shard>/<hex digest>`. New: the hash algo is encoded as a path segment so blobs
are self-describing and two algos can never collide in the key space (e.g. after a config change on a fresh
pool):

`<pool>/blobs/<algo>/<2-char-shard>/<hex digest>`  where `<algo>` ∈ {`ch128`, `xxh3`, `sha256`}.

`CasLayout::blobKey` / `shardedKey` (`CasLayout.h:45-48,334-340`) gain the algo segment; `objectKey(layout,
kind, hash, algo)` threads the algo. The `.meta` sibling stays `blobKey + ".meta"`. `staging/` is unchanged
(staging keys are random, not content-addressed).

## 4. `PoolMeta` records the hash algo {#poolmeta}

`PoolMeta` (`CasPoolMeta.h:20-32`, `PoolMetaProto`) gains `uint8_t blob_hash_algo` (default `1` = cityHash128,
`2` = xxh3-128, `3` = sha256) and (Phase 2) `uint8_t blob_hash_len` (16 or 32). `createOrValidate`
(`CasPoolMeta.cpp:110-138`) validates it like `root_shards`/`blob_header_len`: on an existing pool the
recorded algo is authoritative and a mismatching config fails closed. This makes the pool's hash a durable,
pool-wide invariant while the path segment keeps individual blobs self-describing.

## 5. Streaming hash dispatch {#dispatch}

The blob body is hashed **while streaming** (`HashingWriteBuffer`, block size 2048). Introduce a small
strategy over the streaming hash so `CaContentWriteBuffer` and `poolContentHash` pick the algo:

- `enum class BlobHashAlgo : uint8_t { CityHash128 = 1, XXH3_128 = 2, Sha256 = 3 };`
- A streaming hasher interface `IBlobHasher` with `update(const char*, size_t)` + `finalizeHex() -> String`,
  and concrete impls:
  - `CityHash128BlobHasher` — wraps the existing `HashingWriteBuffer` convention (2048-block chained
    `CityHash128WithSeed`) exactly, so cityHash128 blobs are byte-identical to today (the default MUST NOT
    change any existing hash value).
  - `XXH3_128BlobHasher` — the xxhash library's streaming `XXH3_128bits` state (`XXH3_128bits_reset`/
    `_update`/`_digest`), rendered to 32 hex chars.
  - (Phase 2) `Sha256BlobHasher` — OpenSSL EVP streaming SHA-256 (`OpenSSLHelpers`), 64 hex chars.
- `CaContentWriteBuffer` takes a `BlobHashAlgo` and builds the matching hasher instead of a hardcoded
  `HashingWriteBuffer`. `poolContentHash` (`CasBuild.cpp:72-78`) takes the pool's algo. The `HashingWriteBuffer`
  used elsewhere for run-file/manifest CHECKSUMS (not blob-body content) is UNCHANGED — those stay cityHash128
  (they are internal integrity checksums, not the content address).

## 6. `hash_algo` envelope field becomes meaningful {#envelope}

The envelope's `hash_algo` (`CasEnvelope.h:61`, currently inert `=1`) is set to the pool's `BlobHashAlgo` in
`buildHeader`. `decodeEnvelopeHeader` MAY validate it against the pool algo (fail-closed on mismatch) — a
cheap self-consistency check. This is diagnostic/defensive; the authoritative algo is `PoolMeta` + the path
segment.

## 7. Phase 2 — variable-length digest (deferred, specced) {#phase2}

To admit sha256 (32-byte digest), the fixed `UInt128` blob identity becomes a length-prefixed digest. Sketch
(its own plan + validation later):

- Introduce `struct BlobDigest { BlobHashAlgo algo; String bytes; }` (or keep `UInt128` for 128-bit algos and
  a variant for 256-bit — TBD in the Phase 2 brainstorm). `CasIds` hex round-trip becomes length-aware
  (drop the 32-char gate).
- `CasCodecUtil` gains length-prefixed digest (de)serialization; every `UInt128 blob_hash` field migrates.
- `srcEdgeRunKey` becomes `algo(1) ++ len(1) ++ digest ++ source_id(16)` (source_id stays `UInt128` — it is an
  internal id, not a content hash). `blobShard` takes the first 8 bytes of the digest (all algos ≥ 16 bytes).
- The exact-token GC delete and `.meta` keying are digest-keyed but algo-agnostic; the token (ETag) model is
  unchanged (the token is the object ETag, independent of the content-hash width).
- Phase 2 needs a TLA/soak pass because it touches the settlement run-key layout and GC shard routing.

## 8. Invariants {#invariants}

- **Default unchanged:** with no `blob_hash` config (or `cityhash128`), every blob hash value + key is
  byte-for-byte identical to today (the default path takes the existing cityHash128 convention verbatim).
- **Pool-wide durability:** the pool's algo is fixed at creation and recorded in `PoolMeta`; a config that
  disagrees with an existing pool fails closed (never re-hash existing data).
- **Self-describing keys:** the algo segment in the path means two algos never collide, even across a config
  change on a fresh pool.
- **Checksums stay cityHash128:** run-file/manifest integrity checksums are internal and unchanged; only the
  blob CONTENT address is selectable.
- **Fail-closed:** unknown algo, or `sha256` before Phase 2, throws rather than silently defaulting.

## 9. Testing {#testing}

- Phase 1 gtests: config parse (default/xxh3/unknown-throws/sha256-not-implemented); a blob written with
  `xxh3-128` gets an `xxh3` path segment and a 32-hex xxh3 digest; cityHash128 blobs are byte-identical to
  today (golden); `PoolMeta` records + validates the algo (mismatch fails closed); streaming vs one-shot
  xxh3 agree.
- Integration (`with_rustfs`): a disk with `blob_hash=xxh3-128`, INSERT/SELECT correct, blobs land under
  `blobs/xxh3/...`; a second disk `blob_hash=cityhash128` lands under `blobs/ch128/...` (or the legacy
  layout — see §10); dedup works within an algo.
- Soak: an xxh3-128 variant lane, `dangling==0`.

## 10. Open question — legacy layout for the default {#open}

Does the DEFAULT cityHash128 keep the CURRENT path `blobs/<shard>/<hex>` (no algo segment) for continuity,
or move to `blobs/ch128/<shard>/<hex>`? Since CA is pre-release (no persisted data), moving the default to
`blobs/ch128/...` is cleanest (uniform, self-describing) and is the recommended choice; the legacy no-segment
layout would only matter if we needed on-disk continuity, which we do not. **Recommendation: algo segment for
ALL algos including the default.**

## 12. Phase 2 design (consulted 2026-07-11) {#phase2-design}

Resolves §7's open identity-type question. A strong-model consult (grounded in the code) settled it; summary:

**Representation — (A) a strong `BlobDigest` type.** `struct BlobDigest { std::array<uint8_t, 32> bytes; auto
operator<=>(const BlobDigest&) const = default; }` (big-endian, tail beyond the pool's `blob_hash_len` is
zero). Rejected (B) variable `String`: a 32-byte sha256 digest exceeds libc++'s 22-byte SSO → one heap
allocation per `Blob` `ManifestEntry` on the manifest-decode READ path (part-folder validate-on-hit), and it
costs MORE memory for sha256 (~72 B vs 32 B). (A)'s cost is acceptable (`ManifestEntry` ~80→96 B; `BlobDelta`
40→56 B). A BE `std::array` with default `<=>` yields the SAME total order as today's `UInt128` numeric
compare, so the run merge's `stable_sort` + cross-cursor `dk < key` keep their semantics for free. On-wire
writes exactly `blob_hash_len` bytes (no format bloat).

**Refinement — widen ONLY the content digest.** Internal 128-bit hashes stay `UInt128`: `payload_digest`
(integrity/debug — keep CityHash128, decoupled from the blob algo), `RunRef.checksum`, `sourceEdgeId`,
`GcLease.owner`, `GcHeartbeat.owner`, `manifestCleanupShard`. The strong `BlobDigest` type makes the compiler
enumerate exactly the ~10 content-digest fields and blocks accidental widening of an internal hash.

**`srcEdgeRunKey` + `blobShard` (verified sound).** Run key = `digest[0:blob_hash_len] ++ source_id(16 BE)`;
order preserved (fixed width within one pool); the zero-sentinel `(digest, source_id=0)` still sorts first.
`CondemnedRow` carries NO digest (it's key-only) → retired-in-snapshot settlement untouched. `blobShard` =
BE-u64 of `digest[0:8]` mod `gc_shards` — **bit-identical** to today's `blob_hash >> 64` for every existing
128-bit digest (no reshard on upgrade; mixed-build replicas route identically). MUST be an explicit BE read
(a native-endian memcpy would silently resplit shards → the `ShardReducer` misroute hazard). Bump
`kSourceEdgeKeySchema`: 1 = 16-B digest (32-B key), 2 = 32-B digest (48-B key); `assertSourceEdgeRunHeader`
already fail-closes unknown schemas. Sparse-index seek passes the digest at pool width (prefix-seek holds).

**TWO SILENT fail-open sites — MUST be ported in the SAME commit as the type** (the crux hazard):
1. `CasGc.cpp:~2002` condemn sweep: `hexToU128(k.key.substr(slash+1))` inside `catch(...) { continue; }` —
   `hexToU128` rejects 64-hex, so in a sha256 pool EVERY blob is classified "foreign" and skipped → a **silent
   permanent pool-wide leak** (the never-throw-on-fold discipline turns the unported parse fail-open).
2. `CasFsck.cpp:~258/331/394/458` same parse → fsck silently ignores all sha256 bodies → false-clean pool.
Everything else fails closed + loud (`u128FromBytesBE` throws on ≠16; `CasBuild.cpp:73` `hexToU128` throws
`BAD_ARGUMENTS`; the run-key/manifest parses throw). Move the manifest per-entry 16-B assumption to a manifest
HEADER digest-length field (once, not per entry).

**No TLA+ re-gate** — a width-generalization below the models' opaque-atom abstraction; the touched invariants
(deterministic pure-function routing agreed by all actors; run-key lex order == `(digest, source_id)` with
sentinel-first; settlement semantics) are all preserved and routing is bit-identical for 128-bit pools. Gate
with unit tests: (i) shard-assignment equality (old `>>64` vs new first-8-BE over random digests); (ii)
key-order preservation; (iii) schema-2 run-key round-trip; (iv) old-reader-meets-schema-2 → `NOT_IMPLEMENTED`.

**Biggest risk — len-drift** (the digest value and its meaningful length live apart). An unported site that
renders/serializes the wrong width mints a valid-looking-but-WRONG blob key → object at the wrong key, dedup
misses, and (via the GC sweep's `catch(...) continue`) a silent leak. Mitigations: route ALL digest↔hex/bytes
conversion through ONE codec object constructed from `PoolMeta` (no free functions taking a bare `len`); a
zero-tail `chassert` at codec boundaries; and port the two silent sites in the same commit that introduces the
type.
