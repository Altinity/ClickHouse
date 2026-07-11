# Pluggable blob hash — Phase 2 (sha256 via variable-length digest) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox syntax.

**Goal:** admit sha256 (32-byte digest) as a selectable CAS blob hash by generalizing the fixed 128-bit blob
identity to a pool-scoped variable-length digest, WITHOUT breaking cityHash128/xxh3-128 (128-bit) pools.

**Design:** `docs/superpowers/specs/2026-07-11-cas-pluggable-blob-hash-design.md` §12 (consulted 2026-07-11).
Read §12 before starting — it carries the rationale, the verified `srcEdgeRunKey`/`blobShard` analysis, and
the two silent-leak hazards.

**Prereq:** Phase 1 landed (`f2142d72601..eceacc2ad1d`): `BlobHashAlgo` enum, config `blob_hash`,
`PoolMeta.blob_hash_algo` (fail-close), algo path segment, hash wired into the write/read path. Phase 2
builds on that.

## Global Constraints (from §12 — every task inherits these)

- **Representation:** a **strong type** `struct BlobDigest { std::array<uint8_t,32> bytes; auto operator<=>(...) const = default; }`, big-endian, tail beyond the pool's `blob_hash_len` is zero. NOT a bare array, NOT a `String`.
- **Widen ONLY the content digest.** Internal 128-bit hashes stay `UInt128`: `payload_digest` (keep CityHash128), `RunRef.checksum`, `sourceEdgeId`, `GcLease.owner`, `GcHeartbeat.owner`, `manifestCleanupShard`. The strong type must make the compiler flag every content-digest site.
- **Default (cityHash128) and xxh3-128 pools stay bit-for-bit identical** — same keys, same shard routing, same on-wire bytes. Any change that reshards or reorders an existing 128-bit pool is a bug.
- **`blobShard` = BE-u64 of `digest[0:8]` mod gc_shards — an explicit BE read** (bit-identical to today's `blob_hash >> 64`). A native-endian memcpy read is WRONG (silent reshard).
- **The two silent-leak sites (`CasGc.cpp` condemn sweep + `CasFsck.cpp`, both `hexToU128()` inside `catch(...) continue`) MUST be ported in the SAME commit that widens the digest** — else a sha256 pool silently leaks every blob and fsck reports false-clean. This is the crux.
- **len-drift mitigation:** route ALL digest↔hex/bytes conversion through ONE codec object constructed from `PoolMeta` (no free functions taking a bare `len`); add a zero-tail `chassert` at codec boundaries.
- **No TLA+ re-gate** (width-generalization below the model abstraction) — gate with the unit tests in Task 7.
- C++ style: Allman braces; "exception" not "crash"; wrap `function`/`class` names in inline code.

## Tasks

### Task 1 — `BlobDigest` strong type + the one `PoolMeta`-scoped codec
- `Core/CasIds.h` (or a new `Core/CasBlobDigest.h`): `struct BlobDigest` (BE array, `<=>`, `UInt128Hash`-style hasher for unordered_map). A `DigestCodec` object constructed from `PoolMeta` (holds `blob_hash_len`): `toHex(BlobDigest)`, `fromHex(std::string_view)` (accepts `2*len` hex, rejects others), `toBytesBE(BlobDigest) -> String` (exactly `len` bytes), `fromBytesBE(std::string_view)` (requires `len` bytes), `shardOf(BlobDigest) -> uint64` (BE-u64 of bytes[0:8]). Drop the old free `hexToU128`/`u128ToHex`/`u128ToBytesBE` for CONTENT digests (keep them for internal `UInt128` ids). Tests: round-trip at len=16 and len=32; `fromHex`/`fromBytesBE` reject wrong widths (`BAD_ARGUMENTS`); `shardOf(digest)` == `static_cast<uint64_t>(oldU128 >> 64)` for 100 random 128-bit digests (bit-identical); zero-tail `chassert` fires on a non-zero tail in a len=16 pool.

### Task 2 — `Sha256BlobHasher`
- `Core/CasBlobHasher.{h,cpp}` (+ `CasXXH3.h` sibling pattern for OpenSSL): implement the `Sha256` cases of `makeBlobHashingWriteBuffer` and `blobHashHexOneShot` via OpenSSL EVP streaming SHA-256 (`EVP_DigestInit_ex`/`Update`/`Final`; `Common/OpenSSLHelpers` has one-shot `encodeSHA256`). 64-hex output. Test: sha256 streaming (chunked) == one-shot == a known golden for a fixed input.

### Task 3 — migrate `ManifestEntry` + manifest codec
- `Core/CasManifestCodec.{h,cpp}`: `ManifestEntry.blob_hash` → `BlobDigest`. Move the fixed-16-B per-entry assumption to a manifest HEADER `digest_len` field (once, not per entry). Encode/decode via the `DigestCodec`. Test: manifest round-trip with 16-B and 32-B digests; an old-format (no digest_len header) decodes as len=16.

### Task 4 — migrate the source-edge run key + `blobShard` (the settlement core)
- `Core/CasBlobInDegree.{h,cpp}`: `srcEdgeRunKey` = `digest[0:len] ++ source_id(16 BE)`; `parseSrcEdgeRunKey` reads the width from the run header. Bump `kSourceEdgeKeySchema` to 2 (32-B digest / 48-B key); schema 1 stays 16-B/32-B; `assertSourceEdgeRunHeader` fail-closes unknown schemas. `BlobDelta.blob_hash`/`hash` → `BlobDigest`. `Core/CasGcShardPlan.h` `blobShard`/`owns` take `BlobDigest`, shard = `DigestCodec::shardOf`. The retired-in-snapshot `kCondemned` rows carry no digest (key-only) — the zero-sentinel `(digest, source_id=0)` and the 2-cursor merge just retype. Tests: schema-2 run-key round-trip; key order == `(digest, source_id)` with sentinel-first at len=32; a schema-2 key read by a schema-1-only path → `NOT_IMPLEMENTED` (not `CORRUPTED_DATA`).

### Task 5 — migrate GC/.meta/dedup/outcome + PORT THE TWO SILENT SITES (same commit)
- `Core/CasBlobMeta.h` (`loadMeta`/`putMetaIfAbsent`/`casMeta`/`deleteMetaExact` key), `CasStore.h` dedup cache, `CasGcOutcomes.h`/`CasGc.h`/`CasGcFormats.h` `hash` fields, `CasGenerationSeal` → `BlobDigest`.
- **CRITICAL (same commit):** port `CasGc.cpp:~2002` (condemn sweep) and `CasFsck.cpp:~258/331/394/458` from `hexToU128(...)` in `catch(...) continue` to the `DigestCodec::fromHex` at pool width — else a sha256 pool silently leaks. Add a test that a sha256-pool blob IS seen by the condemn sweep + fsck (not classified foreign).

### Task 6 — config + `PoolMeta.blob_hash_len` + envelope
- `PoolMeta.blob_hash_len` (16/32, derived from `blob_hash_algo`) recorded + fail-close-validated (Phase 1 already fail-closes the algo; len rides it). `MetadataStorageFactory.cpp`: drop the Phase-1 `sha256 → NOT_IMPLEMENTED` guard (now supported). `CasBuild.cpp:73` write-path digest via `DigestCodec`. Test: a `sha256` disk config creates a pool with `blob_hash_len=32`; reopen fail-closes a config-vs-recorded mismatch.

### Task 7 — unit-test gate + integration + soak
- The gate tests (per §12): shard-assignment equality old-vs-new; key-order preservation; schema-2 round-trip; old-reader-meets-schema-2 → `NOT_IMPLEMENTED`; the two-silent-sites coverage from Task 5.
- Integration (`with_rustfs`): a `blob_hash=sha256` disk — INSERT/SELECT correct, blobs under `blobs/sha256/<shard>/<64-hex>`, GC dangling=0, fsck clean (proving the ported sweep sees sha256 blobs).
- Soak: a sha256 variant lane, `dangling==0`; confirm no silent leak (forced-GC residual 0).

## Self-review
Spec coverage: representation (T1), Sha256 hasher (T2), manifest (T3), settlement run-key + shard (T4), GC/meta/dedup + the two silent sites (T5), config/PoolMeta/envelope (T6), gate+integration+soak (T7). The load-bearing constraints (strong type, widen-only-content, bit-identical 128-bit routing, same-commit silent-site port, one PoolMeta codec, zero-tail chassert) are in Global Constraints and repeated at their tasks.
