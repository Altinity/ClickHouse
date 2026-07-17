#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasBlobHasher.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <base/defines.h>
#include <base/extended_types.h>
#include <base/hex.h>
#include <base/types.h>
#include <Common/Exception.h>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

/// Strongly typed value types and codecs shared by the content-addressed metadata and object paths.
///
/// These types deliberately keep protocol identity, content digests, backend tokens, and their
/// textual forms distinct. Their ordering and equality operators make them usable as ordered keys;
/// hash specializations below support unordered containers. Conversion to a storage key or wire
/// representation remains explicit at the boundary that owns that representation.
///
/// Opaque namespace under which root manifests live. The core never interprets its contents:
/// the wiring composes strings like "srv1/<table_uuid>" or "shadow/<backup>/<table_uuid>".
/// Layout only validates its shape (non-empty, no leading/trailing or empty segments,
/// no segment equal to the reserved "_files").
/// Construction from `String` is explicit, preventing unrelated identifier types from being mixed;
/// the underlying string is exposed only through `string` at object-storage boundaries.
class RootNamespace
{
public:
    RootNamespace() = default;
    explicit RootNamespace(String value_) : value(std::move(value_)) {}
    const String & string() const { return value; }
    auto operator<=>(const RootNamespace &) const = default;
    bool operator==(const RootNamespace &) const = default;

private:
    String value;
};

/// Returns 32 lowercase hex chars encoding `v`.
inline String u128ToHex(const UInt128 & v)
{
    return getHexUIntLowercase(v);
}

/// Parses 32-char lowercase (or uppercase) hex string to UInt128.
/// Throws BAD_ARGUMENTS on wrong length or non-hex characters.
inline UInt128 hexToU128(const String & hex)
{
    if (hex.size() != 32)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "hexToU128: expected 32 hex chars, got {}", hex.size());

    // Validate each character is a valid hex digit (unhex(c) returns 0xff for invalid chars).
    for (char c : hex)
    {
        if (::unhex(c) == 0xff)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "hexToU128: invalid hex character '{}'", c);
    }

    return unhexUInt<UInt128>(hex.data());
}

}

namespace std
{

template <>
struct hash<DB::Cas::RootNamespace>
{
    size_t operator()(const DB::Cas::RootNamespace & value) const noexcept
    {
        return std::hash<std::string>{}(value.string());
    }
};

}

namespace DB::Cas
{

/// A content digest whose width is selected by its hash algorithm. The fixed 32-byte big-endian
/// storage accommodates the existing 128-bit algorithms (`cityHash128` and `xxh3-128`) and
/// `sha256`; only the first `blobHashLenFor(algo)` bytes are meaningful and the remaining bytes
/// must be zero. A fixed array avoids a per-manifest-entry allocation that a variable `String`
/// would require for 32-byte digests.
///
/// This type is reserved for content hashes. Protocol identifiers such as `payload_digest`,
/// `RunRef::checksum`, source-edge identifiers, lease owners, and cleanup shards remain
/// `UInt128`, because widening the content digest does not change their separate wire or ordering
/// contracts. A blob's complete identity is `BlobRef`, which pairs this digest with its algorithm.
struct BlobDigest
{
    std::array<uint8_t, 32> bytes{};

    auto operator<=>(const BlobDigest &) const = default;
    bool operator==(const BlobDigest &) const = default;

    /// Converts a 128-bit content hash to the common representation: big-endian in `bytes[0:16]`
    /// and zero in the tail. This is the bridge for the 128-bit hash algorithms.
    static BlobDigest fromU128(const UInt128 & v)
    {
        BlobDigest d;
        for (int i = 0; i < 16; ++i)
            d.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(static_cast<UInt8>(v >> (8 * (15 - i))));
        return d;
    }

    /// Reads `bytes[0:16]` as big-endian into a `UInt128`. The conversion is meaningful only for a
    /// 128-bit digest; the caller is responsible for selecting that width and the tail is ignored.
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

/// Converts a `BlobDigest` using one algorithm's width. A codec must be obtained from the algorithm
/// through `codecFor`, never from a pool-wide width: a pool may contain multiple algorithms. Hex
/// and raw-byte conversions accept and produce exactly the selected width. `shardOf` reads the
/// first eight digest bytes in big-endian order, preserving the existing shard mapping for every
/// 128-bit digest.
class DigestCodec
{
public:
    /// Creates a codec for a supported digest width: 16 bytes for the 128-bit algorithms or
    /// 32 bytes for `sha256`. Any other width violates the per-algorithm representation invariant.
    explicit DigestCodec(uint64_t digest_len_) : len(digest_len_)
    {
        chassert(len == 16 || len == 32, "DigestCodec: digest length must be 16 or 32 bytes");
    }

    /// Renders exactly `2 * len` lowercase hex chars.
    String toHex(const BlobDigest & d) const
    {
        checkZeroTail(d, "toHex");
        return hexString(d.bytes.data(), len);
    }

    /// Requires exactly `2 * len` hex chars; throws `BAD_ARGUMENTS` otherwise (wrong
    /// width or a non-hex character). Zero-fills the tail beyond `len`.
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

    /// Serializes exactly `len` raw bytes, big-endian (i.e. `bytes[0:len]`).
    String toBytesBE(const BlobDigest & d) const
    {
        checkZeroTail(d, "toBytesBE");
        return String(reinterpret_cast<const char *>(d.bytes.data()), len);
    }

    /// Requires exactly `len` bytes; throws `BAD_ARGUMENTS` otherwise. Zero-fills the
    /// tail beyond `len`.
    BlobDigest fromBytesBE(std::string_view b) const
    {
        if (b.size() != len)
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "DigestCodec::fromBytesBE: expected {} bytes for the pool's digest width, got {}", len, b.size());

        BlobDigest d;
        memcpy(d.bytes.data(), b.data(), len);
        return d;
    }

    /// Returns the first eight digest bytes as a big-endian `uint64_t`. Keep this explicit rather
    /// than using a native-endian `memcpy`: changing the byte order would silently remap shards on
    /// little-endian hosts and break compatibility with the 128-bit hash mapping.
    uint64_t shardOf(const BlobDigest & d) const
    {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | d.bytes[static_cast<size_t>(i)];
        return v;
    }

private:
    uint64_t len;

    /// Checks the representation invariant that bytes beyond the selected width are zero. This is
    /// a debug-only internal assertion; wrong-width external input is rejected by the decoding
    /// methods above.
    void checkZeroTail(const BlobDigest & d, [[maybe_unused]] const char * what) const
    {
        for (uint64_t i = len; i < d.bytes.size(); ++i)
            chassert(d.bytes[i] == 0, fmt::format("DigestCodec::{}: non-zero byte at tail position {} (len={})", what, i, len));
    }
};

/// The complete blob identity is the pair of hash algorithm and
/// digest. A bare digest is NOT a blob identity anywhere -- `ch128` and `xxh3` digests are both
/// 16-byte, so the same digest value under two algos names two DIFFERENT objects. BlobRef is
/// constructed ONLY where algo and digest are born together (the write mint / the hasher) or read
/// together (a durable form: settlement key, blob path, manifest entry, envelope). Every other
/// site COPIES BlobRefs -- never assemble one from an algo and a digest obtained separately.
struct BlobRef
{
    BlobHashAlgo algo = BlobHashAlgo::CityHash128;
    BlobDigest digest{};

    auto operator<=>(const BlobRef &) const = default;
    bool operator==(const BlobRef &) const = default;
};

/// Hasher for unordered_map/unordered_set keys (in-process only, not a content address).
struct BlobRefHash
{
    size_t operator()(const BlobRef & r) const noexcept
    {
        size_t h = BlobDigestHash{}(r.digest);
        h ^= static_cast<size_t>(r.algo) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

/// Returns the codec whose width belongs to `algo`; callers must not substitute a pool-wide width.
inline DigestCodec codecFor(BlobHashAlgo algo)
{
    return DigestCodec(blobHashLenFor(algo));
}

/// Bare lowercase hex of the digest at the algo's width -- for OBJECT KEY construction only
/// (the algo lives in the key's path segment `blobs/<algo>/...`).
inline String blobHexOf(const BlobRef & r)
{
    return codecFor(r.algo).toHex(r.digest);
}

/// Human/log identity: "<algoName>:<hex>", e.g. "sha256:ab12...". Rendered ids must never be a
/// bare hex (ambiguous across algos) -- events, inspect JSON and error messages use this.
inline String blobIdOf(const BlobRef & r)
{
    return String(blobHashAlgoName(r.algo)) + ":" + blobHexOf(r);
}

/// The compact reference a root journal stores for a part manifest. It is not a string key:
/// `CasLayout::manifestKey` derives the object key from this reference and the owning namespace.
/// The namespace is deliberately absent because it comes from the owning root context and must not
/// be serialized into the journal reference.
///
///   writer_epoch         - durable monotone writer epoch allocated under the mounted `server_root_id`;
///                          never reused for that server root.
///   build_sequence       - monotone build sequence inside one writer incarnation; part of identity
///                          and of the build-scoped debris prefix.
///   manifest_ordinal     - monotone ordinal inside one build, rendered as `000001.zst` in the key.
struct ManifestRef
{
    uint64_t writer_epoch = 0;
    uint64_t build_sequence = 0;
    uint32_t manifest_ordinal = 0;

    bool operator==(const ManifestRef & o) const = default;

    /// Total order for std::map / std::set keys. Field order is arbitrary but stable.
    bool operator<(const ManifestRef & o) const
    {
        return std::tie(writer_epoch, build_sequence, manifest_ordinal)
             < std::tie(o.writer_epoch, o.build_sequence, o.manifest_ordinal);
    }
};

/// The namespace-qualified protocol identity used by GC for source edges, blob deltas, cleanup work,
/// and addressing. It is the pair `(root_namespace, ManifestRef)`.
/// Two namespaces may legally carry the same `ManifestRef` tuple without addressing the same object;
/// therefore those structures must use `ManifestId`, never `ManifestRef` alone.
struct ManifestId
{
    RootNamespace root_namespace;   /// owning namespace; NOT part of the journal ref
    ManifestRef ref;

    bool operator==(const ManifestId & o) const = default;

    bool operator<(const ManifestId & o) const
    {
        if (root_namespace.string() != o.root_namespace.string())
            return root_namespace.string() < o.root_namespace.string();
        return ref < o.ref;
    }
};

inline constexpr uint32_t kMaxManifestOrdinal = 999999;

/// Six-digit filename for a per-build part-manifest ordinal: `000001.zst` through `999999.zst`
/// (the registered v3 stored suffix for `FormatId::PartManifest`). `0` is reserved as an invalid
/// sentinel and is never emitted.
inline String manifestOrdinalFileName(uint32_t manifest_ordinal)
{
    if (manifest_ordinal == 0 || manifest_ordinal > kMaxManifestOrdinal)
        throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
            "Manifest ordinal must be in [1, {}], got {}", kMaxManifestOrdinal, manifest_ordinal);
    return fmt::format("{:06}{}", manifest_ordinal, storedSuffix(FormatId::PartManifest));
}

/// Formats a manifest reference for logs and diagnostics; this is not an object-key encoding.
inline String manifestRefDebugString(const ManifestRef & ref)
{
    return fmt::format("{}:{}:{}", ref.writer_epoch, ref.build_sequence, ref.manifest_ordinal);
}

}

/// Hash specializations for the identity types. Each combines exactly the fields used by its
/// corresponding `operator==`, so equal keys always have equal hashes.
namespace std
{

template <>
struct hash<DB::Cas::ManifestRef>
{
    size_t operator()(const DB::Cas::ManifestRef & r) const
    {
        const size_t h1 = std::hash<uint64_t>{}(r.writer_epoch);
        const size_t h2 = std::hash<uint64_t>{}(r.build_sequence);
        const size_t h3 = std::hash<uint32_t>{}(r.manifest_ordinal);
        size_t h = h1;
        h ^= h2 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h ^= h3 + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

template <>
struct hash<DB::Cas::ManifestId>
{
    size_t operator()(const DB::Cas::ManifestId & id) const
    {
        const size_t h1 = std::hash<::String>{}(id.root_namespace.string());
        const size_t h2 = std::hash<DB::Cas::ManifestRef>{}(id.ref);
        return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
    }
};

}

namespace DB::Cas
{

/// How a backend identifies one physical incarnation of an object key.
enum class TokenType : uint8_t
{
    ETag = 1,        /// S3-family / Azure
    Generation = 2,  /// GCS (binding deferred; fail-closed until probed)
    Emulated = 3,    /// test backends (in-memory fake, Local emulation)
};

/// A backend-native incarnation token. Opaque; sent back to the backend EXACTLY as observed.
struct Token
{
    String value;
    TokenType type = TokenType::ETag;

    bool empty() const { return value.empty(); }
    bool operator==(const Token &) const = default;
};

/// The ordered ref-transaction identifier. A successful
/// writer mount establishes a strictly newer `writer_epoch`; within an epoch the mounted writer
/// allocates `ref_sequence` from a `Pool`-wide strictly increasing counter at append time. Both fields
/// are nonzero for a valid id -- {0, 0} is never a real transaction. `writer_epoch` is the primary
/// ordering component, so tuple order matches the intended timeline even across an epoch restart that
/// resets `ref_sequence` back to one.
struct RefTxnId
{
    uint64_t writer_epoch = 0;
    uint64_t ref_sequence = 0;

    auto operator<=>(const RefTxnId &) const = default;
};

/// Renders the canonical form: two fixed-width, lower-case, 16-digit hexadecimal numbers joined by
/// '-' (e.g. "0000000000000007-000000000000008e"). Lexical order of the render equals tuple order of
/// `id`, because '-' (0x2d) sorts below every hex digit character and both fields are fixed-width.
/// Throws LOGICAL_ERROR if either field is zero: this render becomes an object key, and an invalid id
/// must never silently produce a well-formed-looking one.
inline String renderRefTxnId(const RefTxnId & id)
{
    if (id.writer_epoch == 0 || id.ref_sequence == 0)
        throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR,
            "RefTxnId: writer_epoch and ref_sequence must both be nonzero, got {}-{}",
            id.writer_epoch, id.ref_sequence);
    return getHexUIntLowercase(id.writer_epoch) + "-" + getHexUIntLowercase(id.ref_sequence);
}

/// Parses the canonical form only: exactly 33 characters, '-' at index 16, exactly 16 lower-case hex
/// digits ('0'-'9', 'a'-'f') either side, and both parsed fields nonzero. Any other shape -- short,
/// long, upper-case, non-hex, misplaced separator, or a zero component -- returns nullopt rather than
/// throwing, since parsing an untrusted listed key is an ordinary "is this ours" question.
inline std::optional<RefTxnId> parseRefTxnId(std::string_view s)
{
    constexpr size_t kFieldLen = 16;
    constexpr size_t kTotalLen = kFieldLen * 2 + 1;
    if (s.size() != kTotalLen || s[kFieldLen] != '-')
        return std::nullopt;

    /// Strict lower-case-hex-only parse: `unhexUInt` (base/hex.h) also accepts upper-case, which the
    /// canonical form must reject, so digits are validated and accumulated by hand here.
    const auto parseField = [](std::string_view field) -> std::optional<uint64_t>
    {
        uint64_t value = 0;
        for (char c : field)
        {
            uint64_t digit;
            if (c >= '0' && c <= '9')
                digit = static_cast<uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<uint64_t>(c - 'a' + 10);
            else
                return std::nullopt;
            value = (value << 4) | digit;
        }
        return value;
    };

    const auto epoch = parseField(s.substr(0, kFieldLen));
    const auto seq = parseField(s.substr(kFieldLen + 1, kFieldLen));
    if (!epoch || !seq || *epoch == 0 || *seq == 0)
        return std::nullopt;
    return RefTxnId{*epoch, *seq};
}

}
