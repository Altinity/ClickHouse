#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.h>
#include <base/defines.h>
#include <base/extended_types.h>
#include <base/hex.h>
#include <base/types.h>
#include <Common/Exception.h>
#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}
}

namespace DB::Cas
{

/// The blob CONTENT digest (CAS pluggable-blob-hash Phase 2, design §12): a pool-scoped
/// variable-length digest that generalizes the fixed 128-bit `UInt128` blob identity so a
/// 256-bit hash (`sha256`) is admissible alongside the existing 128-bit hashes (`cityHash128`,
/// `xxh3-128`). Always 32 bytes wide, big-endian; only the FIRST `PoolMeta::blob_hash_len` bytes
/// (the pool's recorded digest width, 16 or 32) are meaningful -- bytes beyond that are always
/// zero. A fixed-size `std::array` (not a `String`): design §12 rejected a variable `String`
/// because a 32-byte `sha256` digest exceeds libc++'s 22-byte SSO, forcing one heap allocation
/// per `ManifestEntry` on the manifest-decode READ path (part-folder validate-on-hit) and costing
/// MORE memory than the fixed array even for the 32-byte case.
///
/// This is the CONTENT digest ONLY (design §12 "widen ONLY the content digest"): internal
/// 128-bit ids -- `payload_digest`, `RunRef::checksum`, `sourceEdgeId`, `GcLease::owner`,
/// `GcHeartbeat::owner`, `manifestCleanupShard` -- stay `UInt128` and do NOT become a
/// `BlobDigest`. This task (Phase 2 Task 1) introduces the type and its codec ONLY; no existing
/// `UInt128 blob_hash` field is migrated yet (later tasks do that one structure at a time).
struct BlobDigest
{
    std::array<uint8_t, 32> bytes{};

    auto operator<=>(const BlobDigest &) const = default;
    bool operator==(const BlobDigest &) const = default;

    /// For 128-bit pools (`cityHash128`, `xxh3-128`): the existing `UInt128` blob hash, written
    /// big-endian into `bytes[0:16]`, tail zero. Lets later tasks migrate `UInt128 blob_hash`
    /// fields to `BlobDigest` incrementally, one structure at a time, without a flag day.
    static BlobDigest fromU128(const UInt128 & v)
    {
        BlobDigest d;
        for (int i = 0; i < 16; ++i)
            d.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(static_cast<UInt8>(v >> (8 * (15 - i))));
        return d;
    }

    /// Inverse of `fromU128`: reads `bytes[0:16]` as big-endian back into a `UInt128`. Only
    /// meaningful for a digest actually produced at a 128-bit pool's width -- `bytes[16:32]` are
    /// ignored (they are zero at a 128-bit pool's codec boundary).
    UInt128 toU128() const
    {
        UInt128 v = 0;
        for (int i = 0; i < 16; ++i)
            v = (v << 8) | static_cast<UInt8>(bytes[static_cast<size_t>(i)]);
        return v;
    }
};

/// Hasher for `BlobDigest` as an `unordered_map`/`unordered_set` key. This is an in-process hash
/// table key, not a content address, so a cheap FNV-1a mix over the raw bytes is sufficient -- no
/// cryptographic property is needed here.
struct BlobDigestHash
{
    size_t operator()(const BlobDigest & d) const noexcept
    {
        size_t h = 1469598103934665603ull; /// FNV-1a 64-bit offset basis
        for (uint8_t b : d.bytes)
        {
            h ^= b;
            h *= 1099511628211ull; /// FNV-1a 64-bit prime
        }
        return h;
    }
};

/// The ONE pool-scoped digest<->hex/bytes codec (design §12 "len-drift" mitigation, the biggest
/// Phase 2 risk): every digest<->hex or digest<->bytes conversion routes through an object
/// constructed from the pool's recorded `PoolMeta::blob_hash_len` (16 for `cityHash128`/
/// `xxh3-128`, 32 for `sha256`) -- NEVER a free function taking a bare length. `toHex`/`fromHex`
/// render/parse exactly `2 * digestLen()` lowercase hex chars; `toBytesBE`/`fromBytesBE`
/// (de)serialize exactly `digestLen()` raw bytes; `shardOf` reads the first 8 bytes big-endian --
/// bit-identical to today's `blob_hash >> 64` for every 128-bit digest, the gate this whole
/// refactor must never break (design §12).
class DigestCodec
{
public:
    explicit DigestCodec(uint64_t blob_hash_len_) : len(blob_hash_len_)
    {
        chassert(len == 16 || len == 32, "DigestCodec: digest length must be 16 or 32 bytes");
    }

    explicit DigestCodec(const PoolMeta & pool_meta) : DigestCodec(pool_meta.blob_hash_len)
    {
    }

    uint64_t digestLen() const { return len; }

    /// Renders exactly `2 * digestLen()` lowercase hex chars.
    String toHex(const BlobDigest & d) const
    {
        checkZeroTail(d, "toHex");
        return hexString(d.bytes.data(), len);
    }

    /// Requires exactly `2 * digestLen()` hex chars; throws `BAD_ARGUMENTS` otherwise (wrong
    /// width or a non-hex character). Zero-fills the tail beyond `digestLen()`.
    BlobDigest fromHex(std::string_view hex) const
    {
        if (hex.size() != 2 * len)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "DigestCodec::fromHex: expected {} hex chars for a {}-byte digest, got {}",
                2 * len, len, hex.size());

        for (char c : hex)
        {
            if (unhex(c) == 0xff)
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "DigestCodec::fromHex: invalid hex character '{}'", c);
        }

        BlobDigest d;
        for (uint64_t i = 0; i < len; ++i)
            d.bytes[i] = unhex2(hex.data() + i * 2);
        return d;
    }

    /// Serializes exactly `digestLen()` raw bytes, big-endian (i.e. `bytes[0:digestLen()]`).
    String toBytesBE(const BlobDigest & d) const
    {
        checkZeroTail(d, "toBytesBE");
        return String(reinterpret_cast<const char *>(d.bytes.data()), len);
    }

    /// Requires exactly `digestLen()` bytes; throws `BAD_ARGUMENTS` otherwise. Zero-fills the
    /// tail beyond `digestLen()`.
    BlobDigest fromBytesBE(std::string_view b) const
    {
        if (b.size() != len)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "DigestCodec::fromBytesBE: expected {} bytes for the pool's digest width, got {}", len, b.size());

        BlobDigest d;
        memcpy(d.bytes.data(), b.data(), len);
        return d;
    }

    /// BE-u64 of `bytes[0:8]` -- an EXPLICIT big-endian read, bit-identical to today's
    /// `static_cast<uint64_t>(blob_hash >> 64)` for every 128-bit digest (design §12). This must
    /// NEVER become a native-endian `memcpy` read: that would silently reshard on a
    /// little-endian host (the `ShardReducer` misroute hazard the design calls out).
    uint64_t shardOf(const BlobDigest & d) const
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | d.bytes[static_cast<size_t>(i)];
        return v;
    }

private:
    uint64_t len;

    /// len-drift guard (design §12): bytes beyond the pool's digest width must always be zero at
    /// a codec boundary. Debug-only (`chassert`) -- this is an internal invariant check, not a
    /// release fail-close; a genuinely wrong-width INPUT is already rejected loudly by
    /// `fromHex`/`fromBytesBE` above.
    void checkZeroTail(const BlobDigest & d, [[maybe_unused]] const char * what) const
    {
        for (uint64_t i = len; i < d.bytes.size(); ++i)
            chassert(d.bytes[i] == 0, fmt::format("DigestCodec::{}: non-zero byte at tail position {} (len={})", what, i, len));
    }
};

}
