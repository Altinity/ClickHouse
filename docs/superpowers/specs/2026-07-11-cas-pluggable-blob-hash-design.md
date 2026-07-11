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
