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
/// the blob identity a fixed 128-bit `UInt128` for both `CityHash128` and `XXH3_128`. `Sha256` is
/// PARSED here (the enum admits it, `parseBlobHashAlgo` accepts the config spelling) but every
/// consumer of the value -- `makeBlobHashingWriteBuffer` and `blobHashHexOneShot` below -- rejects
/// it with `NOT_IMPLEMENTED` until Phase 2's variable-length digest refactor lands.
enum class BlobHashAlgo : uint8_t
{
    CityHash128 = 1,
    XXH3_128 = 2,
    Sha256 = 3,
};

/// The blob PATH SEGMENT for `algo`, e.g. `<pool>/blobs/<algo>/<shard>/<hex>`: `"ch128"` | `"xxh3"` |
/// `"sha256"`. Throws `BAD_ARGUMENTS` for an out-of-range enum value.
std::string_view blobHashAlgoName(BlobHashAlgo algo);

/// Parses the per-disk `blob_hash` CONFIG value: `"cityhash128"` | `"xxh3-128"` | `"sha256"`. Throws
/// `BAD_ARGUMENTS` on any other value (fail-closed). Note that `"sha256"` parses successfully here --
/// rejecting it as `NOT_IMPLEMENTED` in Phase 1 is the CALLER's job (the config layer), the same way
/// `makeBlobHashingWriteBuffer` and `blobHashHexOneShot` reject it for the streaming/one-shot paths.
BlobHashAlgo parseBlobHashAlgo(std::string_view config_value);

/// A streaming hash-and-passthrough `WriteBuffer`: every byte written is forwarded unchanged to the
/// nested sink AND folded into the running digest, exposed as 32 lowercase hex chars once flushed
/// (`getHashHex` calls `next()` first, mirroring `HashingWriteBuffer::getHash`). It does not
/// finalize or cancel the underlying sink -- that stays the caller's responsibility, the same
/// contract `CaContentWriteBuffer` already relies on for `HashingWriteBuffer` and its nested sink.
class IHashingWriteBuffer : public WriteBuffer
{
public:
    explicit IHashingWriteBuffer(Position ptr = nullptr, size_t size = 0) : WriteBuffer(ptr, size) {}
    ~IHashingWriteBuffer() override = default;

    /// Flushes any pending bytes (like `HashingWriteBuffer::getHash`) and returns the digest as 32
    /// lowercase hex chars. May be called only once useful data has stopped flowing; does not itself
    /// finalize the buffer (mirrors `HashingWriteBuffer::getHash`, which also does not finalize).
    virtual String getHashHex() = 0;
};

/// Builds the streaming hash-and-passthrough buffer for `algo` over `sink`. `sink` must outlive the
/// returned buffer (the same lifetime contract as `HashingWriteBuffer`). `CityHash128` returns a
/// thin adapter over the existing `HashingWriteBuffer` (chunked, `DBMS_DEFAULT_HASHING_BLOCK_SIZE`
/// block, chained `CityHash128WithSeed`) so its hash values stay byte-identical to today -- the CAS
/// default MUST NOT change any existing hash value. `XXH3_128` uses the xxhash library's streaming
/// `XXH3_128bits` state. `Sha256` throws `NOT_IMPLEMENTED` (Phase 2).
std::unique_ptr<IHashingWriteBuffer> makeBlobHashingWriteBuffer(BlobHashAlgo algo, WriteBuffer & sink);

/// One-shot hash of `bytes` for `algo`, used by the re-hash / copy-forward path (`poolContentHash` in
/// `CasBuild.cpp`) and by tests. `CityHash128` goes through `HashingReadBuffer` -- the SAME chunked
/// convention the write side uses (a one-shot `CityHash128WithSeed` call diverges from the chunked
/// hash for any payload larger than one hash block). `XXH3_128` uses the one-shot `XXH3_128bits`
/// call, which is defined to agree with the streaming digest. `Sha256` throws `NOT_IMPLEMENTED`
/// (Phase 2).
String blobHashHexOneShot(BlobHashAlgo algo, std::string_view bytes);

}
