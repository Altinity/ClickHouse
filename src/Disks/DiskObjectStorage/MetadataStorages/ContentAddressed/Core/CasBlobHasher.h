#pragma once

#include <IO/WriteBuffer.h>
#include <base/types.h>
#include <cstdint>
#include <memory>
#include <string_view>

namespace DB::Cas
{

/// The blob content-address hash function, selectable per pool (see the CAS pluggable-blob-hash
/// design doc, docs/superpowers/specs/2026-07-11-cas-pluggable-blob-hash-design.md). Phase 1 keeps
/// the blob identity a fixed 128-bit `UInt128` for both `CityHash128` and `XXH3_128`. Phase 2 adds
/// `Sha256` as a 256-bit digest (both `makeBlobHashingWriteBuffer` and `blobHashHexOneShot` below
/// implement it); everywhere else that still assumes a fixed 128-bit blob identity is tracked as a
/// separate part of the Phase 2 variable-length digest refactor.
enum class BlobHashAlgo : uint8_t
{
    CityHash128 = 1,
    XXH3_128 = 2,
    Sha256 = 3,
};

/// The blob PATH SEGMENT for `algo`, e.g. `<pool>/blobs/<algo>/<shard>/<hex>`: `"ch128"` | `"xxh3"` |
/// `"sha256"`. Throws `BAD_ARGUMENTS` for an out-of-range enum value.
std::string_view blobHashAlgoName(BlobHashAlgo algo);

/// The digest byte width for `algo`: 16 for the 128-bit hashes (`CityHash128`, `XXH3_128`), 32 for
/// the 256-bit `Sha256` (CAS pluggable-blob-hash Phase 2, design §12). Derives `PoolMeta::blob_hash_len`
/// from the pool's recorded `blob_hash_algo` and sizes the `DigestCodec` (`CasBlobDigest.h`). Throws
/// `BAD_ARGUMENTS` for an out-of-range enum value (same fail-closed contract as `blobHashAlgoName`).
uint64_t blobHashLenFor(BlobHashAlgo algo);

/// Parses the per-disk `blob_hash` CONFIG value: `"cityhash128"` | `"xxh3-128"` | `"sha256"`. Throws
/// `BAD_ARGUMENTS` on any other value (fail-closed).
BlobHashAlgo parseBlobHashAlgo(std::string_view config_value);

/// A streaming hash-and-passthrough `WriteBuffer`: every byte written is forwarded unchanged to the
/// nested sink AND folded into the running digest, exposed as lowercase hex once flushed (32 hex
/// chars for the 128-bit hashes, 64 hex chars for `Sha256` -- see `blobHashLenFor`) (`getHashHex`
/// calls `next()` first, mirroring `HashingWriteBuffer::getHash`). It does not finalize or cancel the
/// underlying sink -- that stays the caller's responsibility, the same contract `CaContentWriteBuffer`
/// already relies on for `HashingWriteBuffer` and its nested sink.
class IHashingWriteBuffer : public WriteBuffer
{
public:
    explicit IHashingWriteBuffer(Position ptr = nullptr, size_t size = 0) : WriteBuffer(ptr, size) {}
    ~IHashingWriteBuffer() override = default;

    /// Flushes any pending bytes (like `HashingWriteBuffer::getHash`) and returns the digest as
    /// lowercase hex (length depends on `algo`, see `blobHashLenFor`). May be called only once useful
    /// data has stopped flowing; does not itself finalize the buffer (mirrors
    /// `HashingWriteBuffer::getHash`, which also does not finalize).
    virtual String getHashHex() = 0;
};

/// Builds the streaming hash-and-passthrough buffer for `algo` over `sink`. `sink` must outlive the
/// returned buffer (the same lifetime contract as `HashingWriteBuffer`). `CityHash128` returns a
/// thin adapter over the existing `HashingWriteBuffer` (chunked, `DBMS_DEFAULT_HASHING_BLOCK_SIZE`
/// block, chained `CityHash128WithSeed`) so its hash values stay byte-identical to today -- the CAS
/// default MUST NOT change any existing hash value. `XXH3_128` uses the xxhash library's streaming
/// `XXH3_128bits` state. `Sha256` uses OpenSSL's streaming EVP SHA-256 digest (`EVP_DigestUpdate` per
/// chunk) -- like `XXH3_128`, its streaming digest agrees with the one-shot digest, so there is no
/// chunked convention to preserve.
std::unique_ptr<IHashingWriteBuffer> makeBlobHashingWriteBuffer(BlobHashAlgo algo, WriteBuffer & sink);

/// One-shot hash of `bytes` for `algo`, used by the re-hash / copy-forward path (`poolContentHash` in
/// `CasBuild.cpp`) and by tests. `CityHash128` goes through `HashingReadBuffer` -- the SAME chunked
/// convention the write side uses (a one-shot `CityHash128WithSeed` call diverges from the chunked
/// hash for any payload larger than one hash block). `XXH3_128` uses the one-shot `XXH3_128bits`
/// call, which is defined to agree with the streaming digest. `Sha256` uses OpenSSL's one-shot
/// `encodeSHA256`, which is defined to agree with the streaming EVP digest above.
String blobHashHexOneShot(BlobHashAlgo algo, std::string_view bytes);

}
